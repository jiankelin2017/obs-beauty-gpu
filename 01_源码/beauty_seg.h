#pragma once
// ===========================================================================
// beauty_seg —— 方案 E / P0：整帧语义分割结果（影子层）
//
// 设计要点：
//   * 默认完全不加载模型、不推理、不占显存。只有 BEAUTY_SEG=1 且
//     BEAUTY_SEG_MODEL 指向 .onnx 时才启动。
//   * 分割结果走独立的 256×256 RGBA8 纹理，不改动现有 skin_tex / eye_tex
//     的 R8 契约（beautyCoreCopyMask、protect_buf、四处 effect 绑定都不动）。
//   * R=皮肤概率 / G=五官保护 / B=非肤置信 / A=美白专用皮肤。
//   * 类别索引可由环境变量覆盖；2026-11-05 已逐类核对，这份权重是标准
//     CelebAMask-HQ 19 类顺序（skin=1，hair=17，cloth=16，nose=10，eye_g=6…）。
//     核对脚本：06_离线验证/onnx_probe/seg_class_verify.py。
//
// **输入口径（2026-11-05 已修）**：这份 BiSeNet 吃的是「人脸裁切」。喂整帧
// letterbox 会把脸压成几十像素、退化成大片糊块；换成紧脸裁切后 conf
// 0.46→0.76、峰值 0.995，19 类逐类可对。所以 skinsegRun 必须拿到本帧人脸框。
//
// **尺寸必须是 512（2026-11-05 实测，硬约束）**：这份"动态"模型只改了输入
// 声明（dim_param），图内部仍写死 512。用插件自带的 onnxruntime 1.20.1 喂
// 256/288/320/384 会在 Run 内部直接 **整数除零崩溃**（0xC0000094，整个进程
// 挂掉，不是返回错误）。只有 512 能跑。
// 复核工具：`06_离线验证/segtest/ortprobe.exe model.onnx 512 256`。
// 耗时（本机 4 线程）：512 ≈ 95 ms，1 线程 ≈ 276 ms → 调用方必须节流。
// ===========================================================================

#include <cstdint>

namespace beautycore {

struct FaceBox;   // beauty_face.h（只用指针，避免头文件互引）

struct SegFrame {
    enum { W = 512, H = 512, BYTES = W * H * 4 };
    uint8_t rgba[BYTES];   // 行主序 RGBA8（1 MB）
    // uv：**帧归一化**的采样矩形 (x0, y0, w, h)。着色器反变换为
    //   suv = (uvPix - uv.xy) / uv.zw
    // 裁脸时是裁切框在整帧中的矩形；整帧直填时恒为 (0,0,1,1)。
    float   uv[4];
    uint32_t seq;          // 递增序号；0 = 尚无结果
    uint32_t ms;           // 本次推理耗时（毫秒）
    int      ok;           // 1 = 本帧有效
    int      cropped;      // 1 = 输入是人脸裁切（R 通道可信）；0 = 退化为整帧
};

// 是否启用（读 BEAUTY_SEG）。false 时下面两个函数都直接返回 false。
bool skinsegEnabled();

// 跑一帧。srcRGBA 是 RGBA8、行主序、尺寸 w×h。face 可为空：为空 / 框退化时
// 退回整帧拉伸，并把 cropped 置 0。
//
// **两套坐标必须分开（2026-11-05 修）**：GPU 链开着时，worker 拿到的是宿主的
// **缩略图**（640×640，全帧等比 letterbox 在里面），而 FaceBox 是 buildMaskInto
// 转回来的**全帧几何坐标**。两者混用会让裁切框算错（实测 uv 退化成 (0,0,1,1)，
// 等于没裁）。所以：
//   · (geomW, geomH) 是几何空间尺寸（全帧）；(w, h) 是 src 的像素尺寸。
//   · 缩略图关系是 src = geom * mapScale + (mapOx, mapOy)。
//   · mapScale <= 0 或 geomW/H 为 0 → 老路径：像素系 == 几何系。
bool skinsegRun(const uint8_t* srcRGBA, int w, int h,
                int geomW, int geomH, float mapScale, int mapOx, int mapOy,
                const FaceBox* face, SegFrame* out);

}  // namespace beautycore

// ---- C 导出（渲染线程只读，绝不阻塞）--------------------------------------
// out 由调用方分配（512×512×4 = 1 MB，不要放栈上）。返回 false = 还没有有效结果。
extern "C" bool beautyCoreCopySeg(beautycore::SegFrame* out);
extern "C" int  beautyCoreSegReady();   // 1 = 至少成功跑过一次
// 当前快照的 seq（0 = 还没有结果）。渲染线程先用它判「有没有新结果」，
// 避免每帧白拷 1 MB —— CopySeg 是全量 memcpy。
extern "C" unsigned beautyCoreSegSeq();
// 主动初始化（就是 skinsegEnabled 的一次调用）。返回 1 = 模型已就绪。
// 用于宿主在开机时打印「BEAUTY_SEG=1 但没加载成功」的原因 —— 否则这条路径
// 是**静默失败**：开了 seg 却什么都没发生，看不出是环境变量错了还是模型路径错了。
extern "C" int beautyCoreSegInit();
extern "C" const char* beautyCoreSegError();   // 最近一次失败原因（可能为空串）
