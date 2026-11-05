// ===========================================================================
// beauty_skinseg.cpp —— 方案 E / P0：人脸裁切语义分割影子层
//
// 默认 BEAUTY_SEG=0：本文件里的 ORT 会话一个都不创建，零开销。
// 打开方式（三个环境变量）：
//   BEAUTY_SEG=1
//   BEAUTY_SEG_MODEL=<bisenet_face_dyn.onnx 的完整路径>
//   BEAUTY_SEG_THREADS=4       （默认 4；512 输入实测 1 线程 276ms、4 线程 95ms、
//                               8 线程 57ms —— 这是**每帧**成本，务必配合节流）
//   BEAUTY_SEG_EVERY=12        （节流帧数，由 beauty_core.cpp 的调用方控制）
//   BEAUTY_SEG_MARGIN=1.8      （裁切框外扩倍数）
//
// 类别索引可用 BEAUTY_SEG_CH_SKIN / _NONSKIN / _FACE 覆盖。
// 2026-11-05 逐类核对（`06_离线验证/onnx_probe/seg_class_verify.py`）：这份权重
// 就是标准 CelebAMask-HQ 19 类顺序，不是"未确认"。核对方式是把 argmax 图按
// 调色板逐类对到原图画面上——皮肤=桃色、头发=棕、眼镜=橄榄、鼻=青绿、唇=粉、
// 耳=橙、领口=蓝、背景=近黑，19 类里能看到的每一类都对上了（含眼镜人像那一张
// 的 ch6=eye_g 恰好套住镜片）。
//
//   ch0 background   ch1 skin        ch2 l_brow     ch3 r_brow
//   ch4 l_eye        ch5 r_eye       ch6 eye_g      ch7 l_ear
//   ch8 r_ear        ch9 ear_r      ch10 nose      ch11 mouth
//   ch12 u_lip      ch13 l_lip      ch14 neck      ch15 neck_l
//   ch16 cloth      ch17 hair       ch18 hat
//
// **重要前提**：这份模型吃的是「人脸裁切」。整帧喂进去会退化成大片糊块
// （实测两个类占 84%），此时 ch16=衣服 会被误读成"脸+手"。裁切后同一帧
// conf 0.46→0.76、峰值 0.80→0.995。**裁切已接进本文件（2026-11-05 P0-2）**：
// 由调用方传本帧人脸框，见下方 cropRect。
//
// **两个档位（2026-11-05 加 BEAUTY_SEG_PROFILE）**：
//   bisenet19（默认）：BiSeNet 19 类，512 NCHW，[-1,1]，53 MB，4 线程 98 ms。
//   selfie6         ：MediaPipe selfie-multiclass 6 类，256 NHWC，[-1,1]，
//                     16.4 MB，4 线程 **45.6 ms**（插件自带 ORT 1.20.1 实测，
//                     `ortprobe.exe selfie.onnx 1x256x256x3`）。
//   6 类 = 0 背景 / 1 头发 / 2 身体皮肤 / 3 脸部皮肤 / 4 衣服 / 5 其他。
//   **换它的理由**：body_skin 与 face_skin 是两类，手被判成 body_skin 时天然
//   不进皮肤掩码（合成素材实测手区 body_skin=0.589、face_skin=0.158）。
//
// **尺寸硬约束（只对 bisenet19 成立，别推广到 selfie6）**：BiSeNet 的"动态"模型
// 只改了输入声明，图内部仍写死 512；用插件自带 onnxruntime 1.20.1 喂 256/288/
// 320/384 会在 Run 里整数除零、**整个进程崩掉**（0xC0000094）。selfie6 是原生
// 256，喂 256 正常（已验证）。复核：`06_离线验证/segtest/ortprobe.exe <model> 512`
// 或 `... 1x256x256x3`。
// ===========================================================================

#include "beauty_seg.h"
#include "beauty_face.h"
#include "beauty_onnx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace beautycore {
namespace {

const int SW = SegFrame::W;            // 输出纹理边长 512（着色器契约，别改）

// 语义归并表（CelebAMask-HQ 19 类，见文件头注释）。
//   非肤 = 背景 0 / 衣服 16 / 头发 17 / 帽子 18
//   五官 = 眉 2,3 / 眼 4,5 / 眼镜 6 / 鼻 10 / 嘴 11 / 上下唇 12,13
// 2026-11-05 修正：旧代码把 ch1(皮肤) 与 ch15(项链) 算进"非肤"，等于自己抵自己。
const unsigned kNonSkinMask = (1u << 0) | (1u << 16) | (1u << 17) | (1u << 18);
const unsigned kFaceMask = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
                           (1u << 6) | (1u << 10) | (1u << 11) | (1u << 12) |
                           (1u << 13);

struct State {
    std::once_flag once;
    OnnxModel model;
    bool enabled = false;
    bool loaded = false;
    std::string err;
    int chSkin = 1;                    // CelebAMask: 1 = skin
    int chNonSkin = 18;                // 额外叠加用（默认已含 0/16/17）
    int chFace = 10;                   // 额外叠加用（默认已含 2..6/10..13）
    int threads = 4;

    /* 模型档位（2026-11-05 新增 BEAUTY_SEG_PROFILE）----------------------
     * kBisenet19：原 BiSeNet，19 类，输入 512 NCHW，[-1,1]。53 MB / 98 ms。
     * kSelfie6  ：MediaPipe selfie-multiclass，6 类，输入 256 NHWC，[-1,1]。
     *             16.4 MB / 45.6 ms（插件自带 ORT 1.20.1、4 线程，实测）。
     *             类表 0 背景 / 1 头发 / 2 身体皮肤 / 3 脸部皮肤 / 4 衣服 / 5 其他。
     *             它的价值在于 **body_skin 与 face_skin 分开**：手被判成 body_skin
     *             时，只取 face_skin 当皮肤就能把手挡掉（合成素材实测 0.589）。
     * 两者共用同一套 SegFrame（512 RGBA 纹理），selfie6 的结果由 256 最近邻放大
     * 到 512，下游着色器与纹理契约一行不动。 */
    int kind = 0;                      // 0 = bisenet19，1 = selfie6
    int inSize = 512;                  // 模型输入边长
    int classes = 19;                  // 模型输出类数
    bool nhwc = false;                 // 输入/输出是否 NHWC
    bool in01 = false;                 // 归一化：[0,1] 还是 [-1,1]
    float bodyMix = 0.0f;              // selfie6 专用：body_skin 并入皮肤的系数

    std::vector<int64_t> shape{1, 3, 512, 512};
    std::vector<float> inBuf;          // 复用的输入缓冲（3 MB）
    std::mutex mx;
    SegFrame front{};                  // 渲染线程读的快照
    SegFrame back{};                   // worker 写的工作区
    std::atomic<uint32_t> seq{0};
    std::atomic<int> ready{0};
};
State g;

int envInt(const char* k, int def) {
    const char* v = std::getenv(k);
    if (!v || !*v) return def;
    int n = std::atoi(v);
    return (n > 0 && n < 10000) ? n : def;
}

void initOnce() {
    const char* e = std::getenv("BEAUTY_SEG");
    g.enabled = (e && e[0] == '1');
    if (!g.enabled) return;

    // 档位：默认沿用 BiSeNet（行为不变）；BEAUTY_SEG_PROFILE=selfie6 才是新模型
    const char* pf = std::getenv("BEAUTY_SEG_PROFILE");
    if (pf && (std::strcmp(pf, "selfie6") == 0 || std::strcmp(pf, "selfie_multiclass") == 0)) {
        g.kind = 1;
        g.inSize = 256;
        g.classes = 6;
        g.nhwc = true;
        g.in01 = false;                 // MediaPipe 口径：(x-127.5)/127.5
        const char* bm = std::getenv("BEAUTY_SEG_BODY");
        float b = (bm && *bm) ? (float)std::atof(bm) : 0.0f;
        g.bodyMix = (b > 0.0f) ? (b > 1.0f ? 1.0f : b) : 0.0f;
    }
    // 允许用环境变量改输入尺寸（仅调试用；改错会踩模型内部写死的尺寸）
    g.inSize = envInt("BEAUTY_SEG_INPUT", g.inSize);
    if (g.nhwc) g.shape = {1, g.inSize, g.inSize, 3};
    else        g.shape = {1, 3, g.inSize, g.inSize};

    g.threads = envInt("BEAUTY_SEG_THREADS", 4);
    g.chSkin = std::min(envInt("BEAUTY_SEG_CH_SKIN", 1), g.classes - 1);
    g.chNonSkin = std::min(envInt("BEAUTY_SEG_CH_NONSKIN", 18), g.classes - 1);
    g.chFace = std::min(envInt("BEAUTY_SEG_CH_FACE", 10), g.classes - 1);

    const char* path = std::getenv("BEAUTY_SEG_MODEL");
    if (!path || !*path) {
        std::lock_guard<std::mutex> lk(g.mx);
        g.err = "BEAUTY_SEG_MODEL 未设置";
        return;
    }
    // 插件目录自带 onnxruntime.dll（1.20.1 / API 20），LoadLibraryA 会命中。
    // 若部署到别的目录，可在宿主里调 onnxSetRuntimeDir。
    {
        std::string e;
        if (!g.model.load(path, e, g.threads)) {
            std::lock_guard<std::mutex> lk(g.mx);
            g.err = e;
            return;
        }
    }
    if (g.model.outputCount() < 1) {
        std::lock_guard<std::mutex> lk(g.mx);
        g.err = "模型无输出";
        return;
    }

    // F｜输入尺寸的两道校验。喂错尺寸会让 ORT 在内部做整数除法时除零、
    // 整个进程挂掉（0xC0000094）—— 这种崩法"返回 false 再回退"兜不住，
    // 所以必须在加载时就拒绝。
    {
        // 第一道：档位硬约束。bisenet19 的"动态"模型（bisenet_face_dyn.onnx）
        // **只改了输入声明，图内部仍写死 512**，所以声明为动态也不能放过它
        // —— 实测喂 256/288/320/384 都会崩。selfie6 是原生 256，无此约束
        //（见文件头"尺寸硬约束"一段）。
        if (g.kind == 0 && g.inSize != 512) {
            std::lock_guard<std::mutex> lk(g.mx);
            g.err = "bisenet19 档位的输入边长必须是 512（当前 BEAUTY_SEG_INPUT=" +
                    std::to_string(g.inSize) +
                    "）。该模型的图内部写死 512，声明为动态也不行，喂别的尺寸会崩进程";
            return;
        }

        // 第二道：与模型声明的形状比对。动态维（<=0）跳过；长度不同也判不符。
        const std::vector<int64_t>& ms = g.model.inputShape();
        bool ok = (ms.size() == g.shape.size());
        for (size_t i = 0; ok && i < ms.size(); ++i)
            if (ms[i] > 0 && ms[i] != g.shape[i]) ok = false;
        if (!ok) {
            std::string want, have;
            for (size_t i = 0; i < ms.size(); ++i)
                want += (i ? "x" : "") + std::to_string(ms[i]);
            for (size_t i = 0; i < g.shape.size(); ++i)
                have += (i ? "x" : "") + std::to_string(g.shape[i]);
            std::lock_guard<std::mutex> lk(g.mx);
            g.err = "模型输入尺寸 " + want + " 与 BEAUTY_SEG_INPUT 推出的 " + have +
                    " 不一致，已拒绝加载（尺寸不符会让 ONNX Runtime 崩掉整个进程）";
            return;
        }
    }
    g.loaded = true;
}

// 分割输入的口径（2026-11-05 P0-2）：**人脸裁切**。
//
// 为什么必须裁：这份 BiSeNet 训练时吃的是人脸近景。整帧喂到 256 时脸只占几十
// 个像素，同一张脸实测 conf 0.455 / 峰值 0.80 / 两个类吃掉 84% 的画面；裁成
// 紧脸后 conf 0.757 / 峰值 0.995 / 19 类逐类可对。
//
// 裁法：以人脸框中心取正方形，边长 = max(fw,fh) * BEAUTY_SEG_MARGIN（默认 1.8），
// 平移回帧内后**直接拉伸到 S×S，不补黑边**。黑边必须去掉：早前实测黑边本身
// 会点亮 ch1/ch18，等于给模型喂了不存在的"背景/皮肤"。
//
// uv 记裁切框在整帧中的归一化矩形，着色器据此反查；框外由 mask.hlsl 的软边把
// 乘法因子拉回 1.0（= 不折扣，保留几何结果）。没有可用人脸框时退回整帧拉伸，
// 并把 cropped 置 0 提醒调用方此时 R 通道不可信。

float g_margin = -1.0f;

float segMargin() {
    if (g_margin > 0.0f) return g_margin;
    const char* v = std::getenv("BEAUTY_SEG_MARGIN");
    float m = (v && *v) ? (float)std::atof(v) : 1.8f;
    if (!(m >= 1.0f && m <= 4.0f)) m = 1.8f;   // 太松等于整帧，太紧切掉发际线
    g_margin = m;
    return m;
}

// 算分割输入区（**几何空间** = 全帧，人脸框所在的坐标系）。
// 以框中心取正方形，边长 = max(fw,fh) * BEAUTY_SEG_MARGIN。
// 返回 false = 框不可用，调用方退回整帧。
bool cropRect(const FaceBox* f, int gw, int gh, int& x0, int& y0, int& side) {
    if (!f || !(f->w > 0.5f) || !(f->h > 0.5f)) return false;
    float s = std::max(f->w, f->h) * segMargin();
    const float lim = (float)std::min(gw, gh);   // 保住正方形，避免拉伸
    if (s > lim) s = lim;
    if (!(s >= 8.0f)) return false;
    side = (int)std::lround(s);
    if (side < 8) side = 8;
    if (side > gw) side = gw;
    if (side > gh) side = gh;
    x0 = (int)std::lround(f->x + f->w * 0.5f - side * 0.5f);
    y0 = (int)std::lround(f->y + f->h * 0.5f - side * 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + side > gw) x0 = gw - side;
    if (y0 + side > gh) y0 = gh - side;
    return true;
}

// 源 RGBA8 的 [x0,y0,cw,ch] 区 → N×N 的 RGB float。
// 最近邻足够：缩略图本来就是给模型吃的，且省一次双线性的 4 次采样。
// 整片填满，不留黑边（整帧退回时就是整帧拉伸，此时模型本来就不可信）。
//
// 2026-11-05：按档位切三件事 —— N（256/512）、布局（NHWC/NCHW）、归一化（[0,1]/[-1,1]）。
// MediaPipe 的模型吃 NHWC + (x-127.5)/127.5；BiSeNet 吃 NCHW + (x/255)*2-1。
void fillInput(const uint8_t* src, int w, int h, int x0, int y0, int cw, int ch,
               float* dst) {
    const int N = g.inSize;
    const bool nhwc = g.nhwc;
    const float k = g.in01 ? (1.0f / 255.0f) : (2.0f / 255.0f);
    const float bias = g.in01 ? 0.0f : -1.0f;
    for (int y = 0; y < N; ++y) {
        int sy = y0 + (int)((y + 0.5f) * ch / N);
        if (sy >= y0 + ch) sy = y0 + ch - 1;
        if (sy >= h) sy = h - 1;
        const uint8_t* row = src + (size_t)sy * w * 4;
        for (int x = 0; x < N; ++x) {
            int sx = x0 + (int)((x + 0.5f) * cw / N);
            if (sx >= x0 + cw) sx = x0 + cw - 1;
            if (sx >= w) sx = w - 1;
            const uint8_t* p = row + (size_t)sx * 4;
            const size_t di = (size_t)y * N + x;
            // 源是 BGRA 还是 RGBA 由宿主决定；这里按 RGBA，可在
            // BEAUTY_SEG_BGR=1 时交换（默认 0）。
            const float r = p[0] * k + bias;
            const float gg = p[1] * k + bias;
            const float bb = p[2] * k + bias;
            if (nhwc) {
                float* q = dst + di * 3;
                q[0] = r; q[1] = gg; q[2] = bb;
            } else {
                dst[di] = r;
                dst[(size_t)N * N + di] = gg;
                dst[(size_t)2 * N * N + di] = bb;
            }
        }
    }
}

}  // namespace

bool skinsegEnabled() {
    std::call_once(g.once, initOnce);
    return g.enabled && g.loaded;
}

bool skinsegRun(const uint8_t* srcRGBA, int w, int h,
                int geomW, int geomH, float mapScale, int mapOx, int mapOy,
                const FaceBox* face, SegFrame* out) {
    if (!skinsegEnabled() || !srcRGBA || !out || w < 2 || h < 2) return false;

    // 几何空间（人脸框所在）与像素空间（src 实际分辨率）可能不同：GPU 链开着
    // 时 worker 只拿得到宿主的 640×640 缩略图，换算关系 src = geom*scale + o。
    const bool mapped = (mapScale > 0.0f) && geomW > 0 && geomH > 0;
    const int gw = mapped ? geomW : w;
    const int gh = mapped ? geomH : h;

    // 有可用人脸框 → 裁框；否则整帧（此时 R 通道不可信，cropped 置 0）。
    int gx0 = 0, gy0 = 0, gside = 0;
    bool cropped = cropRect(face, gw, gh, gx0, gy0, gside);

    // 几何空间的框 → src 像素空间的框
    int x0, y0, side;
    if (mapped) {
        x0 = (int)std::lround(gx0 * mapScale) + mapOx;
        y0 = (int)std::lround(gy0 * mapScale) + mapOy;
        side = (int)std::lround(gside * mapScale);
    } else {
        x0 = gx0; y0 = gy0; side = gside;
    }
    // 收进 src 边界；收完还太小就退回整帧（宁可不裁，也别拿半张糊图喂模型）。
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (side > w - x0) side = w - x0;
    if (side > h - y0) side = h - y0;
    if (side < 16) cropped = false;
    if (!cropped) { x0 = 0; y0 = 0; side = 0; }

    if (cropped) {
        out->uv[0] = (float)gx0 / (float)gw;
        out->uv[1] = (float)gy0 / (float)gh;
        out->uv[2] = (float)gside / (float)gw;
        out->uv[3] = (float)gside / (float)gh;
        out->cropped = 1;
    } else {
        x0 = 0; y0 = 0; side = 0;
        out->uv[0] = 0.0f; out->uv[1] = 0.0f;
        out->uv[2] = 1.0f; out->uv[3] = 1.0f;
        out->cropped = 0;
    }

    // 输入缓冲复用：N×N×3 float（512 时 3 MB / 256 时 0.75 MB），每次推理都
    // 新建/释放会平白多一次大块分配（节流后仍是每几百毫秒一次）。常驻一份即可。
    // 只有 async worker 线程会调 skinsegRun，无需加锁。
    const size_t inElems = (size_t)3 * g.inSize * g.inSize;
    if (g.inBuf.size() != inElems) g.inBuf.assign(inElems, 0.0f);
    if (cropped)
        fillInput(srcRGBA, w, h, x0, y0, side, side, g.inBuf.data());
    else
        fillInput(srcRGBA, w, h, 0, 0, w, h, g.inBuf.data());

    std::vector<OnnxTensor> outs;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!g.model.run(g.inBuf.data(), g.shape, outs, err) || outs.empty()) {
        // 持锁写：beautyCoreSegError() 可能在别的线程读它。
        // outs 为空但 run() 成功时 err 还是空串，补一句，否则宿主拿到空消息。
        std::lock_guard<std::mutex> lk(g.mx);
        g.err = err.empty() ? std::string("推理无输出") : err;
        return false;
    }
    const uint32_t inferMs = (uint32_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const OnnxTensor& o = outs[0];
    const float* logits = o.data.data();
    const int N = g.inSize;
    const int Cc = g.classes;
    const size_t plane = (size_t)N * N;
    if (o.data.size() < (size_t)Cc * plane) return false;

    /* 输出纹理固定 512（SegFrame 契约），模型输出可能是 256：
     * 这里按**最近邻放大**写满，x2 时就是简单的 (x>>1, y>>1)。
     * 放大而不是让纹理跟着变小，是为了不牵动 beautyCoreCopySeg 与宿主那张
     * 512×512 纹理的绑定 —— 那条链一行都不用改。 */
    uint8_t* dst = out->rgba;
    for (int oy = 0; oy < SW; ++oy) {
        const int iy = (int)((int64_t)oy * N / SW);
        for (int ox = 0; ox < SW; ++ox) {
            const int ix = (int)((int64_t)ox * N / SW);
            const size_t i = (size_t)iy * N + ix;

            // 数值稳定的 softmax。两种布局：
            //   NCHW（BiSeNet）  ：第 c 类在 logits[c*plane + i]
            //   NHWC（selfie6）  ：第 c 类在 logits[i*C + c]
            float mx = g.nhwc ? logits[i * Cc] : logits[i];
            for (int c = 1; c < Cc; ++c) {
                const float v = g.nhwc ? logits[i * Cc + c] : logits[(size_t)c * plane + i];
                mx = std::max(mx, v);
            }
            float sum = 0.0f;
            float pSkin = 0.0f, pNon = 0.0f, pFace = 0.0f;
            for (int c = 0; c < Cc; ++c) {
                const float v = g.nhwc ? logits[i * Cc + c] : logits[(size_t)c * plane + i];
                const float e = std::exp(v - mx);
                sum += e;
                if (g.kind == 0) {
                    if (c == g.chSkin) pSkin = e;
                    if ((kNonSkinMask >> c & 1u) || c == g.chNonSkin) pNon += e;
                    if ((kFaceMask >> c & 1u) || c == g.chFace) pFace += e;
                } else {
                    /* selfie6 的 6 类：
                     *   0 背景  1 头发  2 身体皮肤（手/脖子/手臂）  3 脸部皮肤  4 衣服  5 其他（眼镜等）
                     * 皮肤只认 face_skin —— 这正是换这份模型的理由：手被判成 body_skin
                     * 时天然被排除。脖子也在 body_skin 里，需要它时用 BEAUTY_SEG_BODY 把
                     * 2 类按系数并进 R（默认 0，即"只磨脸"）。
                     * 非肤取 0/1/4/5；G（五官保护）用 5（配饰 ≈ 眼镜/耳机）。 */
                    if (c == 3) pSkin = e;
                    if (c == 2) pSkin += e * g.bodyMix;
                    if (c == 0 || c == 1 || c == 4 || c == 5) pNon += e;
                    if (c == 5) pFace = e;
                }
            }
            const float inv = 1.0f / std::max(sum, 1e-6f);
            float skin = std::min(pSkin * inv * 2.0f, 1.0f);   // ×2 拉开动态范围
            float non = std::min(pNon * inv, 1.0f);
            float face = std::min(pFace * inv * 2.0f, 1.0f);
            const size_t d = ((size_t)oy * SW + ox) * 4;
            // R 皮肤 / G 五官 / B 非肤 / A 美白专用（暂等于 R）
            dst[d + 0] = (uint8_t)std::lround(skin * 255.0f);
            dst[d + 1] = (uint8_t)std::lround(face * 255.0f);
            dst[d + 2] = (uint8_t)std::lround(non * 255.0f);
            dst[d + 3] = (uint8_t)std::lround(skin * 255.0f);
        }
    }
    out->seq = g.seq.fetch_add(1, std::memory_order_relaxed) + 1;
    out->ms = inferMs;
    out->ok = 1;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.front = *out;
    }
    g.ready.store(1, std::memory_order_relaxed);
    return true;
}

}  // namespace beautycore

// ---- C 导出 ---------------------------------------------------------------
extern "C" bool beautyCoreCopySeg(beautycore::SegFrame* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lk(beautycore::g.mx);
    if (!beautycore::g.front.ok) return false;
    std::memcpy(out, &beautycore::g.front, sizeof(beautycore::SegFrame));
    return true;
}

extern "C" int beautyCoreSegReady() {
    return beautycore::g.ready.load(std::memory_order_relaxed);
}

extern "C" unsigned beautyCoreSegSeq() {
    std::lock_guard<std::mutex> lk(beautycore::g.mx);
    return beautycore::g.front.ok ? (unsigned)beautycore::g.front.seq : 0u;
}

extern "C" int beautyCoreSegInit() {
    return beautycore::skinsegEnabled() ? 1 : 0;
}

extern "C" const char* beautyCoreSegError() {
    // g.err 会在 worker 线程的失败分支被重新赋值；直接返回 c_str() 等于把
    // 一个随时可能被重新分配、释放的指针交给调用方（目前宿主只在 init 后读
    // 一次，所以没爆，但这是埋在运行期的雷）。加锁取一份调用线程本地快照，
    // 返回的指针在**本线程下次调用**之前有效。
    static thread_local std::string snap;
    {
        std::lock_guard<std::mutex> lk(beautycore::g.mx);
        snap = beautycore::g.err;
    }
    return snap.c_str();
}
