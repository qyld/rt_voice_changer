#pragma once
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

#include <filesystem>
#include <string>
#include <vector>

struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> f;     // float 数据
    std::vector<int64_t> i;   // int64 数据 (二选一)
    static Tensor F(std::vector<int64_t> s, std::vector<float> d) { Tensor t; t.shape = std::move(s); t.f = std::move(d); return t; }
    static Tensor I(std::vector<int64_t> s, std::vector<int64_t> d) { Tensor t; t.shape = std::move(s); t.i = std::move(d); return t; }
};

// device: -1 = CPU; >=0 = DirectML 显卡序号 (DXGI EnumAdapters1 顺序, Intel/AMD/NVIDIA 通用)
class OnnxModel {
public:
    OnnxModel(Ort::Env& env, const std::filesystem::path& path, int threads, int device = -1) : device_(device) {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(threads);
        so.SetInterOpNumThreads(1);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (device >= 0) {
#ifdef _WIN32
            so.DisableMemPattern();  // DirectML 要求
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(so, device));
#else
            throw std::runtime_error("GPU 仅支持 Windows DirectML");
#endif
        } else {
            so.AddConfigEntry("session.intra_op.allow_spinning", "1");
        }
#ifdef _WIN32
        session_ = Ort::Session(env, path.wstring().c_str(), so);
#else
        session_ = Ort::Session(env, path.string().c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t k = 0; k < session_.GetInputCount(); ++k)
            in_names_.push_back(session_.GetInputNameAllocated(k, alloc).get());
        for (size_t k = 0; k < session_.GetOutputCount(); ++k)
            out_names_.push_back(session_.GetOutputNameAllocated(k, alloc).get());
    }

    bool on_gpu() const { return device_ >= 0; }

    // 运行, 返回第一个输出 (float)
    std::vector<float> run(std::vector<Tensor>& inputs, std::vector<int64_t>* out_shape = nullptr) {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> vals;
        for (auto& t : inputs) {
            if (!t.i.empty())
                vals.push_back(Ort::Value::CreateTensor<int64_t>(mem, t.i.data(), t.i.size(), t.shape.data(), t.shape.size()));
            else
                vals.push_back(Ort::Value::CreateTensor<float>(mem, t.f.data(), t.f.size(), t.shape.data(), t.shape.size()));
        }
        std::vector<const char*> in, out;
        for (auto& s : in_names_) in.push_back(s.c_str());
        for (auto& s : out_names_) out.push_back(s.c_str());
        auto res = session_.Run(Ort::RunOptions{nullptr}, in.data(), vals.data(), vals.size(), out.data(), 1);
        auto info = res[0].GetTensorTypeAndShapeInfo();
        if (out_shape) *out_shape = info.GetShape();
        const float* p = res[0].GetTensorData<float>();
        return std::vector<float>(p, p + info.GetElementCount());
    }

private:
    Ort::Session session_{nullptr};
    std::vector<std::string> in_names_, out_names_;
    int device_ = -1;
};
