#pragma once

#include <array>
#include <cstdint>
#include <fftw3.h>
#include <span>
#include <vector>

namespace rmic {

// Two-path (Main + Shadow) Partitioned Block Frequency-Domain Adaptive Filter (PBFDAF)
// using Overlap-Save at native sample rate (44.1 / 48 kHz).
class TwoPathAec {
public:
    static constexpr uint32_t kHop = 256;
    static constexpr uint32_t kFftSize = 1024;
    static constexpr uint32_t kPartLen = 512;
    static constexpr uint32_t kStride = kPartLen / kHop; // 2
    static constexpr uint32_t kBins = kFftSize / 2 + 1;  // 513
    static constexpr uint32_t kMaxPartitions = 24;       // ~256 ms @ 48 kHz
    static constexpr uint32_t kHistLen = 64;             // power of 2 > 2 * kMaxPartitions
    static constexpr uint32_t kDelayRingSize = 32768;    // ~682 ms @ 48 kHz
    static_assert((kHistLen & (kHistLen - 1)) == 0, "ring indexing needs a power of 2");
    static_assert((kDelayRingSize & (kDelayRingSize - 1)) == 0, "ring indexing needs a power of 2");
    static_assert(kHistLen > kStride * kMaxPartitions, "history must cover all partitions");

    TwoPathAec() = default;
    ~TwoPathAec();

    TwoPathAec(const TwoPathAec &) = delete;
    TwoPathAec &operator=(const TwoPathAec &) = delete;

    [[nodiscard]] bool init(uint32_t sample_rate, uint32_t ref_channels, float echo_tail_ms);
    void reset();

    // Update bulk delay in samples (applied to reference ring read pointer).
    void set_bulk_delay(int32_t delay_samples);

    // Process one 256-sample hop.
    // - rec_hop: capture hop
    // - play_hops: [ref_channels][256], or nullptr to pass the capture through
    // - out_hop: echo-cancelled hop
    // - echo_psd_out: optional [kBins] sqrt-Hann(512)-windowed linear echo power spectrum
    void process_hop(std::span<const float, kHop> rec_hop,
                     const float *const *play_hops,
                     std::span<float, kHop> out_hop,
                     float *echo_psd_out);

private:
    uint32_t sample_rate_ = 48000;
    uint32_t ref_channels_ = 2;
    uint32_t num_parts_ = 16;
    int32_t bulk_delay_ = 0;

    // Per-channel reference delay ring and 1024-sample FFT sliding window
    std::vector<std::vector<float>> ref_delay_ring_;
    uint32_t delay_write_pos_ = 0;
    std::vector<std::vector<float>> x_time_buf_;

    // X_hist_[ch * kHistLen + slot][kBins * 2]
    std::vector<float> x_hist_;
    uint32_t hist_head_ = 0;

    // Weights: [(ch * num_parts_ + p) * kBins * 2]
    std::vector<float> w_main_;
    std::vector<float> w_shadow_;

    // Smoothed total reference power per bin
    std::vector<float> s_x_;

    // FFT buffers (fftwf aligned)
    float *fft_real_ = nullptr;
    fftwf_complex *fft_cpx_ = nullptr;
    fftwf_complex *y_main_cpx_ = nullptr;
    fftwf_complex *y_shadow_cpx_ = nullptr;
    fftwf_complex *err_cpx_ = nullptr;
    fftwf_complex *grad_cpx_ = nullptr;
    float *y_main_time_ = nullptr;
    float *y_shadow_time_ = nullptr;
    float *grad_time_ = nullptr;

    fftwf_plan plan_fwd_ = nullptr;
    fftwf_plan plan_inv_ = nullptr;

    std::vector<float> e_main_;
    std::vector<float> e_shadow_;
    std::vector<float> y_main_hist_; // 512 samples for windowed echo PSD
    std::array<float, kPartLen> win_hann_sqrt_{};

    // Per-hop scratch: reference power and NLMS step size per bin
    std::array<float, kBins> px_scratch_{};
    std::array<float, kBins> step_scratch_{};

    // Two-path adaptation metrics
    float p_d_ = 0.0f;
    float p_m_ = 0.0f;
    float p_s_ = 0.0f;
    float p_x_ = 0.0f;
    uint32_t shadow_better_count_ = 0;
};

} // namespace rmic
