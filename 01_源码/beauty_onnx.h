#pragma once
// ---------------------------------------------------------------------------
// beauty_onnx —— ONNX Runtime 的最小 C++ 封装（只用 C API）
//
// 为什么用 ONNX Runtime 而不是 OpenCV DNN：
//   * ONNX Runtime 是 MIT 许可，可商用、可闭源分发（符合本次「开源可商用」边界）；
//   * 导出的是**纯 C 接口**，不存在 C++ ABI 版本匹配问题（OpenCV 的 lib 就存在）；
//   * 运行时只有一个 onnxruntime.dll，约 11.6 MB，比 OpenCV 小得多。
//
// 这一层刻意做得很薄：只负责「加载模型 / 查形状 / 喂张量 / 取张量」，
// 具体模型的前后处理放在各自的文件里（beauty_face.cpp 等），互不污染。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace beautycore {

// 指定 onnxruntime.dll 所在目录（完整路径加载）。
//
// 为什么必须指定：机器上可能装着别的软件自带的 onnxruntime.dll（本机就有一个
// 1.17.1 在 PATH 里），而 LoadLibrary("onnxruntime.dll") 会按搜索顺序抓到它，
// 版本对不上时 GetApi 直接返回 nullptr —— 表现为「模型加载失败」而看不出原因。
// 调用方（插件或测试程序）应当在初始化时传入自己 DLL 所在目录。
void onnxSetRuntimeDir(const char* dir);

struct OnnxTensor {
    std::vector<int64_t> shape;   // 例如 {1, 3, 640, 394}
    std::vector<float> data;      // 行主序展平
};

class OnnxModel {
public:
    OnnxModel() {}
    ~OnnxModel();
    OnnxModel(const OnnxModel&) = delete;
    OnnxModel& operator=(const OnnxModel&) = delete;

    // 加载模型。path 必须是 ANSI 可表示的路径（调用方负责转换）。
    // 线程数 <= 0 表示交给 ONNX Runtime 自己决定（默认用全部物理核）。
    bool load(const char* path, std::string& err, int threads = 1);

    bool loaded() const { return session_ != nullptr; }

    // 第一个输入的形状（-1 表示动态维）。加载后即可查，用于校验输入尺寸。
    const std::vector<int64_t>& inputShape() const { return inShape_; }
    int outputCount() const { return (int)outNames_.size(); }
    const char* outputName(int i) const {
        return (i >= 0 && i < (int)outNames_.size()) ? outNames_[(size_t)i].c_str() : "";
    }

    // 跑一次推理。输入形状必须与模型一致（动态维按传入值走）。
    bool run(const float* input, const std::vector<int64_t>& inShape,
             std::vector<OnnxTensor>& outs, std::string& err);

    void release();

private:
    void* env_ = nullptr;        // OrtEnv*
    void* session_ = nullptr;    // OrtSession*
    void* memInfo_ = nullptr;    // OrtMemoryInfo*
    std::vector<std::string> outNames_;
    std::vector<int64_t> inShape_;
    std::string inName_;
};

}  // namespace beautycore
