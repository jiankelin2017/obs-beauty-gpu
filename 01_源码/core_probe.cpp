// core_probe.cpp —— 自研引擎（CoreEngine）的离线对拍入口
//
// 只编译进离线验证 DLL，不进 OBS 插件 DLL。
// 目的：不加载参考实现就能跑自研核，把它和 参考实现的 golden 输出逐档对拍。
//
// 用法：进程内先设 BEAUTY_ENGINE_IMPL=core，再调 beautyEngine() 拿到 CoreEngine。
// 导出（__stdcall，ctypes 用 WinDLL 调用）：
//   BeautyCoreRun(rawIn, rawOut, w, h, blur, radius, ss, sc, down, msgBuf, msgCap) -> 0 成功
//   BeautyCoreMask(rawIn, w, h, outMaskPath, msgBuf, msgCap) -> 0 成功
//     outMaskPath 写「8 字节头（int32 mw, int32 mh）+ mw*mh 字节掩码」，
//     与 golden_capture.cpp 的 BeautyDumpMask 同一格式，可直接互相比较。

#include "beauty_core.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

std::string utf8ToAcp(const char* s) {
    if (!s || !*s) return std::string();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::string(s);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    int m = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return std::string(s);
    std::string a((size_t)m, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &a[0], m, nullptr, nullptr);
    a.resize((size_t)m - 1);
    return a;
}

void setMsg(char* buf, int cap, const char* fmt, ...) {
    if (!buf || cap <= 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(buf, (size_t)cap, _TRUNCATE, fmt, args);
    va_end(args);
}

int g_slot = -1;
// 美白/红润（离线测试用；生产走面板）
double g_whiten = 0.0;
double g_red = 0.0;
double g_clarity = 0.0;   // 面板「清晰」0..100
double g_sharpen = 0.0;   // 面板「锐化」0..100（CoreEngine 0.6 起）
double g_gsharpen = 0.0;  // 面板「全局锐化」0..100
// 五官局部五项（0..100）。顺序固定 = 面板顺序：
//   0 五官立体 1 亮眼 2 去黑眼圈 3 去法令纹 4 祛斑痘
double g_region[5] = {0, 0, 0, 0, 0};
// 3.1.27 美形三项（0..100）。顺序：0 瘦脸 1 大眼 2 下巴
double g_shape[3] = {0, 0, 0};

}  // namespace

extern "C" {

__declspec(dllexport) int __stdcall BeautyCoreRun(
    const char* rawIn, const char* rawOut, int width, int height,
    double blur, int radius, float sigmaSpace, float sigmaColor, int down,
    char* msgBuf, int msgCap) {
    if (msgBuf && msgCap > 0) msgBuf[0] = 0;
    if (!rawIn || !rawOut || width <= 0 || height <= 0) {
        setMsg(msgBuf, msgCap, "bad args");
        return 2;
    }
    // 强制走自研引擎（无论外部环境怎么设）
    // 离线对拍默认走**同步**检测：异步的「第 k 帧用哪份掩码」取决于线程调度，
    // 逐字节哈希会飘。要测异步就由调用方显式传 BEAUTY_ASYNC_DETECT=1。
    if (!getenv("BEAUTY_ASYNC_DETECT")) _putenv_s("BEAUTY_ASYNC_DETECT", "0");
    _putenv_s("BEAUTY_ENGINE_IMPL", "core");

    const std::string inA = utf8ToAcp(rawIn);
    const std::string outA = utf8ToAcp(rawOut);
    const size_t need = (size_t)width * (size_t)height * 4;

    FILE* f = nullptr;
    if (fopen_s(&f, inA.c_str(), "rb") != 0 || !f) {
        setMsg(msgBuf, msgCap, "cannot open input: %s", inA.c_str());
        return 3;
    }
    std::vector<unsigned char> buf(need);
    const size_t got = fread(&buf[0], 1, need, f);
    fclose(f);
    if (got != need) {
        setMsg(msgBuf, msgCap, "input too short: got %zu of %zu", got, need);
        return 4;
    }

    if (radius > 0)
        beautyCoreSetSmoothParams(radius, sigmaSpace, sigmaColor, down);

    BeautySettings s;
    for (int i = 0; i < P_COUNT; i++) s.panel[i] = 0.0;
    s.panel[P_BLUR] = blur;
    s.panel[P_WHITEN] = g_whiten;
    s.panel[P_RED] = g_red;
    s.panel[P_CLARITY] = g_clarity;
    s.panel[P_SHARPEN] = g_sharpen;
    s.panel[P_GLOBAL_SHARP] = g_gsharpen;
    s.panel[P_THREED] = g_region[0];
    s.panel[P_EYEBRIGHT] = g_region[1];
    s.panel[P_POUCH] = g_region[2];
    s.panel[P_NASOLABIAL] = g_region[3];
    s.panel[P_DELSPOT] = g_region[4];
    s.panel[P_THIN_FACE] = g_shape[0];
    s.panel[P_BIG_EYE] = g_shape[1];
    s.panel[P_CHIN] = g_shape[2];

    IBeautyEngine& e = beautyEngine();
    if (strcmp(e.id(), "core") != 0) {
        setMsg(msgBuf, msgCap, "engine is '%s', not core", e.id());
        return 5;
    }
    // 必须显式初始化：CoreEngine 在这里加载 YuNet 模型（OBS 里由 preheat 触发）。
    // 少了这一步，掩码永远是「不可用」，磨皮会退化成全帧。
    {
        std::string ierr;
        if (!e.ensureReady(ierr)) {
            setMsg(msgBuf, msgCap, "ensureReady failed: %s", ierr.c_str());
            return 9;
        }
    }
    if (g_slot < 0) {
        std::string err;
        g_slot = e.acquireSlot(err);
        if (g_slot < 0) {
            setMsg(msgBuf, msgCap, "acquireSlot failed: %s", err.c_str());
            return 6;
        }
    }

    std::vector<unsigned char> out(need);
    if (!e.process(g_slot, &buf[0], &out[0], width, height, s)) {
        setMsg(msgBuf, msgCap, "process failed");
        return 7;
    }
    if (fopen_s(&f, outA.c_str(), "wb") != 0 || !f) {
        setMsg(msgBuf, msgCap, "cannot write output: %s", outA.c_str());
        return 8;
    }
    fwrite(&out[0], 1, need, f);
    fclose(f);
    setMsg(msgBuf, msgCap, "OK engine=%s blur=%.0f ms=%.2f faces=%d region=%d err=[%s]",
           e.id(), blur, e.lastCoreMs(), e.lastFaces(), e.regionActive() ? 1 : 0,
           e.lastErr());
    return 0;
}

// 性能开关：在 BeautyCoreRun 之前调用，控制线程数与 ROI 裁剪。
// 存在的理由是 A/B 测量 —— 必须能在同一份二进制上分别跑「全帧单线程」和
// 「ROI 多线程」，否则优化收益只能靠改源码重编译去猜。
__declspec(dllexport) void __stdcall BeautyCoreSetPerf(int threads, int useRoi) {
    beautyCoreSetPerf(threads, useRoi);
}

__declspec(dllexport) void __stdcall BeautyCoreSetEdgeK(int k) {
    beautyCoreSetEdgeK(k);
}

__declspec(dllexport) void __stdcall BeautyCoreSetProtectK(int k) {
    beautyCoreSetProtectK(k);
}

__declspec(dllexport) void __stdcall BeautyCoreSetLimitThr(int thr) {
    beautyCoreSetLimitThr(thr);
}

__declspec(dllexport) void __stdcall BeautyCoreSetFreq(int mode, float sigma,
                                                       float magLo, float magHi) {
    beautyCoreSetFreq(mode, sigma, magLo, magHi);
}

__declspec(dllexport) void __stdcall BeautyCoreSetFreqGrain(float g) {
    beautyCoreSetFreqGrain(g);
}

__declspec(dllexport) void __stdcall BeautyCoreSetGuide(int mode, int radius, float eps) {
    beautyCoreSetGuide(mode, radius, eps);
}

__declspec(dllexport) void __stdcall BeautyCoreSetLowPath(int path, int refRadius) {
    beautyCoreSetLowPath(path, refRadius);
}

__declspec(dllexport) void __stdcall BeautyCoreSetClarity(float maxGain, int radius,
                                                          float kneeLo, float kneeHi) {
    beautyCoreSetClarity(maxGain, radius, kneeLo, kneeHi);
}

// 面板「清晰」0..100（与界面同尺度）
__declspec(dllexport) void __stdcall BeautyCoreSetClarityPanel(double v) {
    g_clarity = v;
}

// 离线扫参用：核半径 / σSpace / σColor（生产走默认值，不调这里）
__declspec(dllexport) void __stdcall BeautyCoreSetSmooth(int radius, float sigmaSpace,
                                                         float sigmaColor) {
    beautyCoreSetSmoothParams(radius, sigmaSpace, sigmaColor, 1);
}

// 美白/红润（0..100，与面板同尺度）
__declspec(dllexport) void __stdcall BeautyCoreSetTone(double whiten, double red) {
    g_whiten = whiten;
    g_red = red;
}

// 锐化 / 全局锐化（0..100，与面板同尺度）。CoreEngine 0.6 起。
__declspec(dllexport) void __stdcall BeautyCoreSetDetail(double face, double global) {
    g_sharpen = face;
    g_gsharpen = global;
}

// 五官局部五项（idx: 0 立体 1 亮眼 2 去黑眼圈 3 法令纹 4 祛斑痘；值 0..100）
__declspec(dllexport) void __stdcall BeautyCoreSetRegion(int idx, double v) {
    if (idx >= 0 && idx < 5) g_region[idx] = v;
}

// 3.1.27 美形三项（idx: 0 瘦脸 1 大眼 2 下巴；值 0..100）
__declspec(dllexport) void __stdcall BeautyCoreSetShape(int idx, double v) {
    if (idx >= 0 && idx < 3) g_shape[idx] = v;
}

// 上一次 BeautyCoreRun 的六段分项耗时（毫秒）。
// 索引：0 掩码 / 1 磨皮 / 2 色调 / 3 五官 / 4 细节 / 5 美形。
//
// 存在的理由：`ms=` 是整条管线的时间，单段占比小的时候会被调度抖动淹没。
// 例如美型段在 720p 只有 2~3 ms，而整帧在 8~20 ms 之间跳，用「改档位前后
// 两次总耗时相减」求差值，样本噪声比被测对象还大（实测差值在 -12 ~ +0 之间
// 乱走，中位数不可信）。引擎自己记的 markSeg 边界没有这个问题。
__declspec(dllexport) void __stdcall BeautyCoreSeg(double* out6) {
    if (!out6) return;
    for (int i = 0; i < 6; i++) out6[i] = 0.0;
    beautyCoreSegMs(out6);
}

// 把自研引擎这一帧的掩码落盘，格式与 BeautyDumpMask 一致
// （8 字节头：int32 mw, int32 mh；随后 mw*mh 字节 0/255）。
//
// 存在的理由：自研只磨「肤色 ∩ 人脸框」，参考实现的改动区更大（含脖子/肩）。
// 对拍必须用自研掩码当分母，否则「只磨了脸」会被读成「几乎没磨」。
__declspec(dllexport) int __stdcall BeautyCoreMask(
    const char* rawIn, int width, int height,
    const char* outMask, char* msgBuf, int msgCap) {
    if (msgBuf && msgCap > 0) msgBuf[0] = 0;
    if (!rawIn || !outMask || width <= 0 || height <= 0) {
        setMsg(msgBuf, msgCap, "bad args");
        return 2;
    }
    // 离线对拍默认走**同步**检测：异步的「第 k 帧用哪份掩码」取决于线程调度，
    // 逐字节哈希会飘。要测异步就由调用方显式传 BEAUTY_ASYNC_DETECT=1。
    if (!getenv("BEAUTY_ASYNC_DETECT")) _putenv_s("BEAUTY_ASYNC_DETECT", "0");
    _putenv_s("BEAUTY_ENGINE_IMPL", "core");

    const std::string inA = utf8ToAcp(rawIn);
    const std::string outA = utf8ToAcp(outMask);
    const size_t need = (size_t)width * (size_t)height * 4;

    FILE* f = nullptr;
    if (fopen_s(&f, inA.c_str(), "rb") != 0 || !f) {
        setMsg(msgBuf, msgCap, "cannot open input: %s", inA.c_str());
        return 3;
    }
    std::vector<unsigned char> buf(need);
    const size_t got = fread(&buf[0], 1, need, f);
    fclose(f);
    if (got != need) {
        setMsg(msgBuf, msgCap, "input too short: got %zu of %zu", got, need);
        return 4;
    }

    IBeautyEngine& e = beautyEngine();
    if (strcmp(e.id(), "core") != 0) {
        setMsg(msgBuf, msgCap, "engine is '%s', not core", e.id());
        return 5;
    }
    {
        std::string ierr;
        if (!e.ensureReady(ierr)) {
            setMsg(msgBuf, msgCap, "ensureReady failed: %s", ierr.c_str());
            return 6;
        }
    }

    std::vector<unsigned char> mask((size_t)width * (size_t)height, 0);
    const bool ok = beautyCoreComputeMask(&buf[0], width, height, &mask[0]);
    if (!ok) {
        setMsg(msgBuf, msgCap, "no mask this frame (%s)", e.lastErr());
        return 7;
    }

    if (fopen_s(&f, outA.c_str(), "wb") != 0 || !f) {
        setMsg(msgBuf, msgCap, "cannot write mask: %s", outA.c_str());
        return 8;
    }
    int hdr[2] = {width, height};
    fwrite(hdr, sizeof(int), 2, f);
    fwrite(&mask[0], 1, (size_t)width * (size_t)height, f);
    fclose(f);

    size_t px = 0;
    for (size_t i = 0, n = (size_t)width * (size_t)height; i < n; i++)
        if (mask[i]) px++;
    setMsg(msgBuf, msgCap, "OK mask=%dx%d px=%zu (%.2f%%) faces=%d",
           width, height, px,
           100.0 * (double)px / (double)((size_t)width * (size_t)height),
           e.lastFaces());
    return 0;
}

}  // extern "C"
