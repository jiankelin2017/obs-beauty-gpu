// beauty_onnx.cpp —— ONNX Runtime C API 的最小封装实现
//
// 只依赖 onnxruntime_c_api.h（纯 C 接口）。所有句柄以 void* 保存，避免把头文件
// 的 C 类型泄漏到项目其它地方。

#include "beauty_onnx.h"

#include <onnxruntime_c_api.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace beautycore {

namespace {

std::string g_ortDir;     // onnxruntime.dll 所在目录（可空 = 走默认搜索）
std::string g_ortVersion; // 实际加载到的 ORT 版本（版本不匹配时用于报错）

}  // namespace

void onnxSetRuntimeDir(const char* dir) {
    g_ortDir = (dir && *dir) ? dir : "";
}

namespace {
// 动态加载 ONNX Runtime。
//
// 为什么不用导入库硬链接：那会让 obs-beauty.dll 在**加载阶段**就依赖
// onnxruntime.dll —— 用户少装一个文件，整个滤镜就加载失败。动态加载后最坏
// 情况只是「自研引擎降级」，默认的 core 路径完全不受影响。
//
// 也正因为如此，编译时**不链接** onnxruntime.lib（见 build_manual.bat）。
const OrtApi* api() {
    static const OrtApi* a = nullptr;
    static bool tried = false;
    if (tried) return a;
    tried = true;

    // 优先按「调用方给的目录」用完整路径加载，避免抓到系统里另一个版本的
    // onnxruntime.dll（本机就有一个 1.17.1 在 PATH 中，版本不匹配时
    // GetApi 会静默返回 nullptr）。
    HMODULE h = nullptr;
    if (!g_ortDir.empty()) {
        const std::string full = g_ortDir + "\\onnxruntime.dll";
        h = LoadLibraryA(full.c_str());
    }
    if (!h) h = LoadLibraryA("onnxruntime.dll");
    if (!h) return nullptr;

    typedef const OrtApiBase* (*fnGetApiBase)(void);
    fnGetApiBase f = (fnGetApiBase)GetProcAddress(h, "OrtGetApiBase");
    if (!f) return nullptr;
    const OrtApiBase* base = f();
    if (!base) return nullptr;
    a = base->GetApi(ORT_API_VERSION);
    if (!a) {
        // 版本不匹配：把实际版本记下来，供上层报错
        g_ortVersion = base->GetVersionString();
    }
    return a;
}

// 路径转宽字符。**必须用 CP_ACP**：调用方传进来的是 ANSI（窄字符）路径 ——
// GetModuleFileNameA / argv 都是 ANSI，不是 UTF-8。用 CP_UTF8 转会把中文目录
// 变成乱码，表现为「读不到模型文件」但路径看起来完全正确。
std::wstring acpToWide(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

// 整个模型读进内存。用 CreateSessionFromArray 加载可以彻底绕开路径编码问题
// （ORT 的 CreateSession 只收 wchar_t，而调用方传的是 UTF-8）。
bool readAll(const char* path, std::vector<unsigned char>& out) {
    const std::wstring w = acpToWide(path);
    FILE* f = nullptr;
    if (w.empty() || _wfopen_s(&f, w.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    const size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}
}  // namespace

OnnxModel::~OnnxModel() { release(); }

void OnnxModel::release() {
    const OrtApi* A = api();
    if (!A) { session_ = nullptr; memInfo_ = nullptr; env_ = nullptr; return; }
    if (session_) { A->ReleaseSession((OrtSession*)session_); session_ = nullptr; }
    if (memInfo_) { A->ReleaseMemoryInfo((OrtMemoryInfo*)memInfo_); memInfo_ = nullptr; }
    if (env_) { A->ReleaseEnv((OrtEnv*)env_); env_ = nullptr; }
    outNames_.clear();
    inName_.clear();
    inShape_.clear();
}

bool OnnxModel::load(const char* path, std::string& err, int threads) {
    release();
    if (!path || !*path) { err = "模型路径为空"; return false; }
    const OrtApi* A = api();
    if (!A) {
        err = g_ortVersion.empty()
                  ? std::string("onnxruntime.dll 不可用（缺失，或未找到自带的 1.20.1）")
                  : (std::string("onnxruntime.dll 版本不匹配：加载到 ") + g_ortVersion +
                     "，编译时需要 1.20.x（API 版本 20）。请把自带的 onnxruntime.dll 放到插件目录。");
        return false;
    }

    OrtEnv* env = nullptr;
    if (A->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "beauty", &env) != nullptr) {
        err = "CreateEnv 失败";
        return false;
    }
    env_ = env;

    OrtSessionOptions* opts = nullptr;
    // 返回值必须查：失败时 opts 为 null，后面 set_* 全家都会解引用它。
    if (A->CreateSessionOptions(&opts) != nullptr || !opts) {
        err = "CreateSessionOptions 失败";
        release();
        return false;
    }
    if (threads > 0) A->SetIntraOpNumThreads(opts, threads);
    A->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);

    // 关掉 ORT 的线程自旋（默认是开的）。
    //
    // 这是 3.1.8 的实测发现，不是抄来的：把掩码提交周期从 5 帧改成每帧之后，
    // 进程 CPU 从 0.66 核涨到 **2.02 核** —— 而单次推理只要 14.1ms，30fps 下
    // 摊下来只该多 0.4 核。差额来自 ORT 的 intra-op 线程池：Run() 之间它会
    // **忙等**一段时间等下一次 Run（为的是省掉线程唤醒的那几十微秒）。
    // 检测每 167ms 一次时，忙等几乎碰不到；改成每 33ms 一次，2 个线程就几乎
    // 全程在自旋，白烧 1.4 个核。
    //
    // 关掉之后每次 Run 多几十微秒唤醒开销，相对 14ms 的推理可以忽略。
    // 键名见 ORT 1.14+ 的 session config；我们锁的是 1.20.x（API 20）。
    // 万一未来 ORT 改名，AddSessionConfigEntry 会返回错误 —— 这里刻意忽略返回值，
    // 不让一个「省电优化」把模型加载搞挂。
    {
        OrtStatus* s1 = A->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0");
        if (s1) A->ReleaseStatus(s1);
        OrtStatus* s2 = A->AddSessionConfigEntry(opts, "session.inter_op.allow_spinning", "0");
        if (s2) A->ReleaseStatus(s2);
    }

    std::vector<unsigned char> blob;
    if (!readAll(path, blob)) {
        A->ReleaseSessionOptions(opts);
        err = std::string("读不到模型文件: ") + path;
        release();   // 连同前面已建的 env_ 一起回收
        return false;
    }

    OrtSession* sess = nullptr;
    OrtStatus* st = A->CreateSessionFromArray(env, &blob[0], blob.size(), opts, &sess);
    A->ReleaseSessionOptions(opts);
    if (st != nullptr) {
        err = std::string("CreateSession 失败: ") + A->GetErrorMessage(st);
        A->ReleaseStatus(st);
        release();   // 释放 env_；session_ 此时仍为 null
        return false;
    }
    session_ = sess;

    OrtAllocator* alloc = nullptr;
    A->GetAllocatorWithDefaultOptions(&alloc);

    // 输入：只支持单输入模型（YuNet 就是单输入）
    char* inName = nullptr;
    if (A->SessionGetInputName(sess, 0, alloc, &inName) != nullptr) {
        err = "读取输入名失败";
        return false;
    }
    inName_ = inName;
    alloc->Free(alloc, inName);

    OrtTypeInfo* ti = nullptr;
    if (A->SessionGetInputTypeInfo(sess, 0, &ti) == nullptr) {
        const OrtTensorTypeAndShapeInfo* tsi = nullptr;
        if (A->CastTypeInfoToTensorInfo(ti, &tsi) == nullptr && tsi) {
            size_t nd = 0;
            A->GetDimensionsCount(tsi, &nd);
            inShape_.resize(nd, 0);
            if (nd) A->GetDimensions(tsi, inShape_.data(), nd);
        }
        A->ReleaseTypeInfo(ti);
    }

    size_t nout = 0;
    A->SessionGetOutputCount(sess, &nout);
    for (size_t i = 0; i < nout; i++) {
        char* on = nullptr;
        if (A->SessionGetOutputName(sess, i, alloc, &on) == nullptr) {
            outNames_.push_back(on);
            alloc->Free(alloc, on);
        }
    }

    OrtMemoryInfo* mem = nullptr;
    if (A->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem) != nullptr) {
        // 此时 session_ 已就位：留着它会让 run() 拿到空 memInfo_，
        // CreateTensorWithDataAsOrtValue 直接吃 null。整体回退更安全。
        err = "CreateCpuMemoryInfo 失败";
        release();
        return false;
    }
    memInfo_ = mem;
    return true;
}

bool OnnxModel::run(const float* input, const std::vector<int64_t>& inShape,
                    std::vector<OnnxTensor>& outs, std::string& err) {
    outs.clear();
    if (!session_) { err = "模型未加载"; return false; }
    if (inShape.empty()) { err = "输入形状为空"; return false; }
    const OrtApi* A = api();
    if (!A) { err = "onnxruntime.dll 不可用"; return false; }

    size_t elems = 1;
    for (int64_t d : inShape) {
        if (d <= 0) { err = "输入形状含非法维度"; return false; }
        elems *= (size_t)d;
    }

    std::vector<const char*> inNames{inName_.c_str()};
    OrtValue* inVal = nullptr;
    OrtStatus* st = A->CreateTensorWithDataAsOrtValue(
        (OrtMemoryInfo*)memInfo_, (void*)input, elems * sizeof(float),
        inShape.data(), inShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal);
    if (st != nullptr) {
        err = std::string("CreateTensor 失败: ") + A->GetErrorMessage(st);
        A->ReleaseStatus(st);
        return false;
    }

    std::vector<const char*> outNames;
    for (auto& s : outNames_) outNames.push_back(s.c_str());
    std::vector<OrtValue*> outVals(outNames.size(), nullptr);

    st = A->Run((OrtSession*)session_, nullptr, inNames.data(), &inVal, 1,
                outNames.data(), outNames.size(), outVals.data());
    A->ReleaseValue(inVal);
    if (st != nullptr) {
        err = std::string("Run 失败: ") + A->GetErrorMessage(st);
        A->ReleaseStatus(st);
        return false;
    }

    for (size_t i = 0; i < outVals.size(); i++) {
        OnnxTensor t;
        float* p = nullptr;
        A->GetTensorMutableData(outVals[i], (void**)&p);
        OrtTensorTypeAndShapeInfo* info = nullptr;
        if (A->GetTensorTypeAndShape(outVals[i], &info) == nullptr && info) {
            size_t nd = 0;
            A->GetDimensionsCount(info, &nd);
            t.shape.resize(nd, 0);
            if (nd) A->GetDimensions(info, t.shape.data(), nd);
            size_t n = 0;
            A->GetTensorShapeElementCount(info, &n);
            if (p && n) t.data.assign(p, p + n);
            A->ReleaseTensorTypeAndShapeInfo(info);
        }
        A->ReleaseValue(outVals[i]);
        outs.push_back(std::move(t));
    }
    return true;
}

}  // namespace beautycore
