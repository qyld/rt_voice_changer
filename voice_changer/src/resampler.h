// 有理数比例多相 Kaiser-sinc 流式重采样器 (任意 in_rate -> out_rate, 如 48000->16000, 40000->48000)
#pragma once
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

class Resampler {
public:
    Resampler() = default;
    Resampler(int in_rate, int out_rate) { init(in_rate, out_rate); }

    void init(int in_rate, int out_rate) {
        int g = std::gcd(in_rate, out_rate);
        L_ = out_rate / g;
        M_ = in_rate / g;
        passthrough_ = (L_ == M_);
        double fc = std::min(1.0, double(L_) / M_) * 0.94;  // 截止频率 (相对输入奈奎斯特)
        D_ = int(std::ceil(16.0 / fc));                       // 半宽 (输入采样数)
        const double beta = 8.0;
        int half = D_ * L_;
        table_.assign(2 * half + 1, 0.f);
        for (int r = -half; r <= half; ++r) {
            double t = double(r) / L_;                       // 输入采样时间单位
            double x = fc * t;
            double s = (std::abs(x) < 1e-12) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            double w = t / D_;
            double kw = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - w * w))) / bessel_i0(beta);
            table_[r + half] = float(fc * s * kw);
        }
        reset();
    }

    void reset() {
        hist_.assign(2 * D_ + 1, 0.f);  // 负索引补零
        hist_base_ = -(int64_t)(2 * D_ + 1);
        in_count_ = 0;
        out_count_ = 0;
    }

    // 追加 n 个输入, 输出所有可计算的样本到 out (追加)
    void process(const float* x, size_t n, std::vector<float>& out) {
        if (passthrough_) { out.insert(out.end(), x, x + n); return; }
        hist_.insert(hist_.end(), x, x + n);
        in_count_ += (int64_t)n;
        const int half = D_ * L_;
        for (;;) {
            int64_t t = out_count_ * (int64_t)M_;
            int64_t n_hi = t / L_;                  // floor(kM/L)
            if (n_hi > in_count_ - 1) break;
            int ph = int(t - n_hi * L_);
            double acc = 0.0;
            // r = ph - D*L + j*L,  n = n_hi - j
            for (int j = 0; j <= 2 * D_ + 1; ++j) {
                int r = ph - half + j * L_;
                if (r > half) break;
                int64_t idx = n_hi - j - hist_base_;
                if (idx < 0) break;
                acc += double(hist_[(size_t)idx]) * table_[r + half];
            }
            out.push_back(float(acc));
            ++out_count_;
        }
        // 丢弃不再需要的历史
        int64_t need_from = (out_count_ * (int64_t)M_) / L_ - 2 * D_ - 2;
        int64_t drop = need_from - hist_base_;
        if (drop > 4096) {
            hist_.erase(hist_.begin(), hist_.begin() + (size_t)drop);
            hist_base_ += drop;
        }
    }

    int delay_in_samples() const { return passthrough_ ? 0 : D_; }  // 以输入采样计的群延迟

private:
    static double bessel_i0(double x) {
        double s = 1, t = 1;
        for (int k = 1; k < 50; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
        return s;
    }
    int L_ = 1, M_ = 1, D_ = 16;
    bool passthrough_ = true;
    std::vector<float> table_, hist_;
    int64_t hist_base_ = 0, in_count_ = 0, out_count_ = 0;
};
