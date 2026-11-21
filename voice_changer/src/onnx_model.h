#pragma once
#include <onnxruntime_cxx_api.h>
#ifdef _WIN32
#include <dml_provider_factory.h>
#endif

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "vc_log.h"

struct Tensor {
    std::vector<int64_t> shape;
    std::vector<float> f;     // float 数据
    std::vector<int64_t> i;   // int64 数据 (二选一)
    static Tensor F(std::vector<int64_t> s, std::vector<float> d) { Tensor t; t.shape = std::move(s); t.f = std::move(d); return t; }
    static Tensor I(std::vector<int64_t> s, std::vector<int64_t> d) { Tensor t; t.shape = std::move(s); t.i = std::move(d); return t; }
};

// device: -1 = CPU; >=0 = DirectML 显卡序号 (DXGI EnumAdapters1 顺序, Intel/AMD/NVIDIA 通用)
// 显卡上运行失败 (驱动不支持某算子/显存不足等) 时, 自动用 CPU + fp32 模型重建并重试, 不让程序崩溃。
class OnnxModel {
public:
    OnnxModel(Ort::Env& env, const std::filesystem::path& path, int threads, int device = -1,
              const std::filesystem::path& cpu_path = {})
        : env_(env), threads_(threads), cpu_path_(cpu_path.empty() ? path : cpu_path), name_(path.filename().u8string()) {
        create(path, device);
    }

    bool on_gpu() const { return device_ >= 0; }
    bool fell_back() const { return fell_back_; }
    const std::string& name() const { return name_; }

    // 运行, 返回第一个输出 (float)
    std::vector<float> run(std::vector<Tensor>& inputs, std::vector<int64_t>* out_shape = nullptr) {
        if (device_ < 0) return run_once(inputs, out_shape);
        try {
            if (fake_fail()) throw std::runtime_error("模拟显卡执行失败 (VC_FAKE_GPU_FAIL)");
            return run_once(inputs, out_shape);
        } catch (const std::exception& e) {
            vc_log("[显卡] " + name_ + " 在显卡上执行失败, 改用 CPU: " + e.what());
            create(cpu_path_, -1);
            fell_back_ = true;
            return run_once(inputs, out_shape);
        }
    }

private:
    void create(const std::filesystem::path& path, int device) {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(threads_);
        so.SetInterOpNumThreads(1);
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const bool fake = std::getenv("VC_FAKE_GPU") != nullptr;  // 测试: 用 CPU 冒充显卡, 验证回退逻辑
        if (device >= 0 && !fake) {
#ifdef _WIN32
            so.DisableMemPattern();  // DirectML 要求
            const OrtDmlApi* dml = nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dml));
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML(so, device));
#else
            throw std::runtime_error("GPU 仅支持 Windows DirectML");
#endif
        } else if (device < 0) {
            so.AddConfigEntry("session.intra_op.allow_spinning", "1");
        }
#ifdef _WIN32
        session_ = Ort::Session(env_, path.wstring().c_str(), so);
#else
        session_ = Ort::Session(env_, path.string().c_str(), so);
#endif
        device_ = device;
        in_names_.clear();
        out_names_.clear();
        Ort::AllocatorWithDefaultOptions alloc;
        for (size_t k = 0; k < session_.GetInputCount(); ++k)
            in_names_.push_back(session_.GetInputNameAllocated(k, alloc).get());
        for (size_t k = 0; k < session_.GetOutputCount(); ++k)
            out_names_.push_back(session_.GetOutputNameAllocated(k, alloc).get());
    }

    bool fake_fail() const {
        const char* f = std::getenv("VC_FAKE_GPU_FAIL");
        if (!f) return false;
        std::string s(f), base = name_.substr(0, name_.find('.'));
        if (base.size() > 5 && base.compare(base.size() - 5, 5, "_fp16") == 0) base.resize(base.size() - 5);
        return s == "all" || s.find(base) != std::string::npos;
    }

    std::vector<float> run_once(std::vector<Tensor>& inputs, std::vector<int64_t>* out_shape) {
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

    Ort::Env& env_;
    int threads_;
    std::filesystem::path cpu_path_;
    std::string name_;
    Ort::Session session_{nullptr};
    std::vector<std::string> in_names_, out_names_;
    int device_ = -1;
    bool fell_back_ = false;
};
