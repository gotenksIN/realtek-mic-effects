#pragma once

#include <cstdint>
#include <fftw3.h>
#include <span>
#include <vector>

namespace rmic {

// 4 kHz decimated GCC-PHAT bulk acoustic/buffer delay estimator.
// Searches positive delays [0 .. max_delay_ms] where mic lags play.
class BulkDelayEstimator {
public:
    BulkDelayEstimator() = default;
    ~BulkDelayEstimator();

    BulkDelayEstimator(const BulkDelayEstimator &) = delete;
    BulkDelayEstimator &operator=(const BulkDelayEstimator &) = delete;

    [[nodiscard]] bool init(uint32_t sample_rate, uint32_t max_delay_ms = 512);
    void reset();

    // Feed synchronized capture and mono reference blocks of arbitrary length.
    // Only the first min(rec.size(), play_mono.size()) samples are used.
    void feed(std::span<const float> rec, std::span<const float> play_mono);

    // Current confirmed delay in full-rate samples (>= 0).
    int32_t delay_samples() const { return confirmed_delay_; }

private:
    static constexpr uint32_t kFftSize = 4096;
    static constexpr uint32_t kBins = kFftSize / 2 + 1;
    static constexpr uint32_t kEvalInterval = 128; // decimated samples (~32 ms)
    static_assert((kFftSize & (kFftSize - 1)) == 0, "ring indexing needs a power of 2");

    struct Lpf2 {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        float step(float x) {
            const double dx = static_cast<double>(x);
            const double y = b0 * dx + z1;
            z1 = b1 * dx - a1 * y + z2;
            z2 = b2 * dx - a2 * y;
            return static_cast<float>(y);
        }
        void reset() { z1 = z2 = 0.0; }
    };

    void run_gcc_phat();

    uint32_t sample_rate_ = 48000;
    uint32_t decim_ = 12;
    uint32_t max_lag_dec_ = 2048;
    uint32_t decim_phase_ = 0;

    Lpf2 rec_lpf_{};
    Lpf2 play_lpf_{};

    std::vector<float> rec_ring_;
    std::vector<float> play_ring_;
    uint32_t ring_pos_ = 0;
    uint32_t eval_counter_ = 0;

    float *fft_in_ = nullptr;
    fftwf_complex *fft_x_ = nullptr;
    fftwf_complex *fft_y_ = nullptr;
    fftwf_complex *fft_c_ = nullptr;
    float *ifft_out_ = nullptr;
    fftwf_plan plan_fwd_ = nullptr;
    fftwf_plan plan_inv_ = nullptr;

    std::vector<float> s_yx_re_;
    std::vector<float> s_yx_im_;
    float play_energy_ = 0.0f;

    int32_t candidate_delay_ = 0;
    uint32_t candidate_hits_ = 0;
    int32_t confirmed_delay_ = 0;
};

} // namespace rmic
