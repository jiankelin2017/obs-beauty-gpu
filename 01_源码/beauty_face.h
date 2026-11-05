#pragma once
// ---------------------------------------------------------------------------
// beauty_face —— YuNet 人脸检测（ONNX Runtime 推理 + 自写后处理）
//
// 用途：替代 参考实现美颜库 的 fuFaceProcessor*，给皮肤掩码提供「人脸框」约束。
// 这是 S1 对拍得出的方案：肤色判据 ∩ 人脸框(grow=1.15) → IoU 0.707 / 精确率 0.875，
// 远好于纯肤色判据的 0.251 / 0.254。
//
// 模型：YuNet 2023mar（opencv_zoo，MIT 许可）。固定 640×640 输入、
// 12 个输出（cls/obj/bbox/kps × stride 8/16/32）。
//
// 为什么自己写后处理而不用 OpenCV 的 FaceDetectorYN：OpenCV 会额外带进来
// 二十多 MB 的库，而这里只需要 decode + NMS 一百多行。
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

namespace beautycore {

struct FaceBox {
    float x = 0, y = 0, w = 0, h = 0;   // 原图坐标
    float score = 0;
    // YuNet 的 5 个关键点（原图坐标），顺序为：
    //   0 右眼  1 左眼  2 鼻尖  3 右嘴角  4 左嘴角
    // 用途：构造「五官保护区」，让磨皮在眼睛和嘴巴上减弱。
    // 这是 lumina-beauty 的 kComposeShader 里 uSmoothProtect 的做法。
    float kps[10] = {0};
    bool hasKps = false;
};

class FaceDetector {
public:
    bool load(const char* modelPath, std::string& err);
    bool loaded() const;
    void release();

    // 推理输入边长。
    //
    // 重要事实：我们用的 YuNet 2023mar ONNX **把输入声明成了固定 640x640**
    // （ORT 会报 `Got: 512 Expected: 640`）。OpenCV 能跑 320 是因为它的 dnn
    // 引擎不认模型声明的固定维度，会自己重做 shape 推断 —— 这是 OpenCV 的
    // 行为，不是模型的能力。所以本函数对固定 shape 的模型是**空操作**。
    //
    // 想真正降分辨率只有两条路：换一个原生动态导出的模型，或改模型文件的
    // dim_param 后重新做 shape 推断（后者风险高，网络内部 reshape 依赖具体
    // 尺寸）。在拿到动态模型之前，别指望这条参数能提速。
    void setInputSize(int px);
    bool inputSizeFixed() const { return fixedSize_; }
    int inputSize() const { return inputSize_; }

    // src 是紧凑或带 stride 的图。检出的人脸框以**原图坐标**返回。
    //
    // rotateDeg（2.13.1 / P0-11 阶段 2）：先把源图绕自身中心旋转该角度再送进
    // 模型，取值 0/90/180/270，其余值按 0 处理。rotation 发生在 letterbox 的
    // 重采样里，**不额外分配旋转缓冲**（4K 一帧 33 MB，转一份要两次全帧读写）。
    // ⚠ 开旋转时返回的坐标落在「旋转后整图」坐标系（尺寸 90/270 时宽高互换），
    // 调用方必须用 invRotateFace 反变换回源坐标 —— 见 beauty_core.cpp 的同名说明。
    // conf / nms 是阈值：0.6 / 0.3 与 OpenCV 的默认一致。
    //
    // stride：每像素字节数。传 4 可以直接喂 OBS 的 RGBA 帧，**跳过「先转一份
    // 全分辨率 RGB」的中间缓冲** —— 1080p 下这一份缓冲要写 600 万字节，而
    // 检测只需要 640x640 的图，那份中间缓冲纯属浪费。
    // 面板「人物个数」：只保留分数最高的 N 个脸。NMS 的输出本来就是按分数降序，
    // 所以截断即可，不需要再排一次。默认 8（与 BEAUTY_WARP_MAX_FACES 一致）。
    void setMaxFaces(int n) { maxFaces_ = (n < 1) ? 1 : (n > 8 ? 8 : n); }
    int  maxFaces() const { return maxFaces_; }

    bool detect(const unsigned char* src, int w, int h,
                std::vector<FaceBox>& faces, std::string& err,
                float conf = 0.6f, float nms = 0.3f, int stride = 3,
                int rotateDeg = 0);

    double lastMs() const { return lastMs_; }

    // 本帧所有候选里的最高置信度（**不受 conf 阈值影响**，也不受 NMS 影响）。
    // 用途：检不出脸时判断到底是「模型没看出有脸」还是「看出来了但没到阈值」——
    // 这两种情况的处理完全不同（前者要调尺寸，后者要调阈值）。
    float lastBestScore() const { return bestScore_; }

private:
    void* model_ = nullptr;              // OnnxModel*（避免头文件互相包含）
    std::vector<float> input_;
    std::vector<unsigned char> resized_;
    int inputSize_ = 640;
    int maxFaces_ = 8;
    float bestScore_ = 0.f;
    bool fixedSize_ = false;   // 模型声明了固定输入尺寸
    double lastMs_ = 0.0;
};


}  // namespace beautycore


