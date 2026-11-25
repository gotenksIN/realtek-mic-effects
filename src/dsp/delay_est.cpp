#include "delay_est.hpp"

#include "fftw_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rmic {

namespace {
constexpr double kPi = std::numbers::pi;

void design_butterworth_lpf(double fs, double fc,
                            double &b0, double &b1, double &b2,
                            double &a1, double &a2) {
    const double w0 = 2.0 * kPi * fc / fs;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = sin_w0 / std::sqrt(2.0);
    const double a0 = 1.0 + alpha;
    b0 = ((1.0 - cos_w0) * 0.5) / a0;
    b1 = (1.0 - cos_w0) / a0;
    b2 = ((1.0 - cos_w0) * 0.5) / a0;
    a1 = (-2.0 * cos_w0) / a0;
    a2 = (1.0 - alpha) / a0;
}
} // namespace

BulkDelayEstimator::~BulkDelayEstimator() {
    if (plan_fwd_) fftwf_destroy_plan(plan_fwd_);
    if (plan_inv_) fftwf_destroy_plan(plan_inv_);
    if (fft_in_) fftwf_free(fft_in_);
    if (fft_x_) fftwf_free(fft_x_);
    if (fft_y_) fftwf_free(fft_y_);
    if (fft_c_) fftwf_free(fft_c_);
    if (ifft_out_) fftwf_free(ifft_out_);
}

bool BulkDelayEstimator::init(uint32_t sample_rate, uint32_t max_delay_ms) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    decim_ = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(sample_rate_) / 4000.0)));
    const double dec_fs = static_cast<double>(sample_rate_) / static_cast<double>(decim_);
    const uint32_t max_lag = static_cast<uint32_t>(std::ceil(dec_fs * static_cast<double>(max_delay_ms) * 0.001));
    max_lag_dec_ = std::clamp(max_lag, 1u, kFftSize / 2);

    const double cutoff = std::min(1800.0, 0.42 * dec_fs);
    design_butterworth_lpf(sample_rate_, cutoff, rec_lpf_.b0, rec_lpf_.b1, rec_lpf_.b2, rec_lpf_.a1, rec_lpf_.a2);
    design_butterworth_lpf(sample_rate_, cutoff, play_lpf_.b0, play_lpf_.b1, play_lpf_.b2, play_lpf_.a1, play_lpf_.a2);

    rec_ring_.assign(kFftSize, 0.0f);
    play_ring_.assign(kFftSize, 0.0f);
    s_yx_re_.assign(kBins, 0.0f);
    s_yx_im_.assign(kBins, 0.0f);

    if (!fft_in_) fft_in_ = fftwf_alloc_real(kFftSize);
    if (!fft_x_) fft_x_ = fftwf_alloc_complex(kBins);
    if (!fft_y_) fft_y_ = fftwf_alloc_complex(kBins);
    if (!fft_c_) fft_c_ = fftwf_alloc_complex(kBins);
    if (!ifft_out_) ifft_out_ = fftwf_alloc_real(kFftSize);

    if (!fft_in_ || !fft_x_ || !fft_y_ || !fft_c_ || !ifft_out_) {
        return false;
    }

    if (!plan_fwd_ || !plan_inv_) {
        if (!ensure_fftw_planner_thread_safe()) {
            return false;
        }
        if (!plan_fwd_) {
            plan_fwd_ = fftwf_plan_dft_r2c_1d(static_cast<int>(kFftSize), fft_in_, fft_x_, FFTW_ESTIMATE);
        }
        if (!plan_inv_) {
            plan_inv_ = fftwf_plan_dft_c2r_1d(static_cast<int>(kFftSize), fft_c_, ifft_out_, FFTW_ESTIMATE);
        }
    }

    if (!plan_fwd_ || !plan_inv_) {
        return false;
    }

    reset();
    return true;
}

void BulkDelayEstimator::reset() {
    std::fill(rec_ring_.begin(), rec_ring_.end(), 0.0f);
    std::fill(play_ring_.begin(), play_ring_.end(), 0.0f);
    std::fill(s_yx_re_.begin(), s_yx_re_.end(), 0.0f);
    std::fill(s_yx_im_.begin(), s_yx_im_.end(), 0.0f);
    rec_lpf_.reset();
    play_lpf_.reset();
    ring_pos_ = 0;
    decim_phase_ = 0;
    eval_counter_ = 0;
    play_energy_ = 0.0f;
    candidate_delay_ = 0;
    candidate_hits_ = 0;
    confirmed_delay_ = 0;
}

void BulkDelayEstimator::feed(std::span<const float> rec, std::span<const float> play_mono) {
    if (!plan_fwd_) return;

    const size_t n = std::min(rec.size(), play_mono.size());
    // Keep the anti-alias filter state in registers for the whole block.
    Lpf2 rec_lpf = rec_lpf_;
    Lpf2 play_lpf = play_lpf_;
    for (size_t i = 0; i < n; ++i) {
        const float r_f = rec_lpf.step(rec[i]);
        const float p_f = play_lpf.step(play_mono[i]);

        if (++decim_phase_ >= decim_) {
            decim_phase_ = 0;
            rec_ring_[ring_pos_] = r_f;
            play_ring_[ring_pos_] = p_f;
            play_energy_ = 0.995f * play_energy_ + 0.005f * (p_f * p_f);
            ring_pos_ = (ring_pos_ + 1) & (kFftSize - 1);

            if (++eval_counter_ >= kEvalInterval) {
                eval_counter_ = 0;
                if (play_energy_ > 1e-8f) {
                    run_gcc_phat();
                }
            }
        }
    }
    // Flush subnormal state once per block for hosts without FTZ/DAZ.
    if (std::abs(rec_lpf.z1) < 1e-30) rec_lpf.z1 = 0.0;
    if (std::abs(rec_lpf.z2) < 1e-30) rec_lpf.z2 = 0.0;
    if (std::abs(play_lpf.z1) < 1e-30) play_lpf.z1 = 0.0;
    if (std::abs(play_lpf.z2) < 1e-30) play_lpf.z2 = 0.0;
    rec_lpf_ = rec_lpf;
    play_lpf_ = play_lpf;
}

void BulkDelayEstimator::run_gcc_phat() {
    // Restrict-qualified pointers live in inner blocks so that FFTW calls
    // never access the same buffers inside their scope.
    {
        float *__restrict__ fft_in = assume_fftw_aligned(fft_in_);
        const float *__restrict__ play_ring = play_ring_.data();
        // Unwrap play_ring_ into [0..4095] (oldest -> newest)
        for (uint32_t i = 0; i < kFftSize; ++i) {
            fft_in[i] = play_ring[(ring_pos_ + i) & (kFftSize - 1)];
        }
    }
    fftwf_execute_dft_r2c(plan_fwd_, fft_in_, fft_x_);

    {
        float *__restrict__ fft_in = assume_fftw_aligned(fft_in_);
        const float *__restrict__ rec_ring = rec_ring_.data();
        // Unwrap latest 2048 samples of rec_ring_ into [2048..4095], zero [0..2047]
        std::memset(fft_in, 0, sizeof(float) * (kFftSize / 2));
        for (uint32_t i = kFftSize / 2; i < kFftSize; ++i) {
            fft_in[i] = rec_ring[(ring_pos_ + i) & (kFftSize - 1)];
        }
    }
    fftwf_execute_dft_r2c(plan_fwd_, fft_in_, fft_y_);

    {
        // Smooth cross-spectrum and apply PHAT weighting
        constexpr float alpha = 0.85f;
        constexpr float one_minus = 1.0f - alpha;
        const fftwf_complex *__restrict__ fft_x = assume_fftw_aligned(fft_x_);
        const fftwf_complex *__restrict__ fft_y = assume_fftw_aligned(fft_y_);
        fftwf_complex *__restrict__ fft_c = assume_fftw_aligned(fft_c_);
        float *__restrict__ s_re = s_yx_re_.data();
        float *__restrict__ s_im = s_yx_im_.data();
        for (uint32_t k = 0; k < kBins; ++k) {
            const float yr = fft_y[k][0];
            const float yi = fft_y[k][1];
            const float xr = fft_x[k][0];
            const float xi = fft_x[k][1];

            // Y * conj(X)
            const float cr = yr * xr + yi * xi;
            const float ci = yi * xr - yr * xi;

            const float sr = alpha * s_re[k] + one_minus * cr;
            const float si = alpha * s_im[k] + one_minus * ci;
            s_re[k] = sr;
            s_im[k] = si;

            const float mag = std::sqrt(sr * sr + si * si);
            const float inv = 1.0f / (mag + 1e-12f);
            fft_c[k][0] = sr * inv;
            fft_c[k][1] = si * inv;
        }

        // Zero DC and very low bins (< 80 Hz) to ignore hum/DC in delay tracking
        const uint32_t low_cut_bin = std::max(1u, static_cast<uint32_t>(80.0 * kFftSize / (static_cast<double>(sample_rate_) / decim_)));
        for (uint32_t k = 0; k < low_cut_bin && k < kBins; ++k) {
            fft_c[k][0] = 0.0f;
            fft_c[k][1] = 0.0f;
        }
    }

    fftwf_execute_dft_c2r(plan_inv_, fft_c_, ifft_out_);

    // Find peak over valid non-negative lags [0 .. max_lag_dec_].
    // Use magnitude so polarity-inverted acoustic/hardware paths still lock cleanly.
    const float *ifft_out = assume_fftw_aligned(ifft_out_);
    float best_val = 0.0f;
    uint32_t best_lag = 0;
    double abs_sum = 0.0;
    for (uint32_t d = 0; d <= max_lag_dec_; ++d) {
        const float av = std::abs(ifft_out[d]);
        abs_sum += av;
        if (av > best_val) {
            best_val = av;
            best_lag = d;
        }
    }

    const float mean_abs = static_cast<float>(abs_sum / static_cast<double>(max_lag_dec_ + 1));
    const float peak_ratio = best_val / (mean_abs + 1e-9f);

    // Require strong correlation spike before trusting lag estimate
    if (peak_ratio < 5.5f) {
        return;
    }

    const int32_t est_samples = static_cast<int32_t>(best_lag * decim_);
    // Keep ~8 ms causal margin so early reflections stay inside the linear filter
    const int32_t margin_samples = static_cast<int32_t>(0.008 * static_cast<double>(sample_rate_));
    const int32_t aligned_samples = std::max<int32_t>(0, est_samples - margin_samples);
    const int32_t tol = static_cast<int32_t>(2 * decim_);

    if (std::abs(aligned_samples - candidate_delay_) <= tol) {
        if (candidate_hits_ < 100) ++candidate_hits_;
    } else {
        candidate_delay_ = aligned_samples;
        candidate_hits_ = 1;
    }

    // Hysteresis: update confirmed delay only if consistently detected
    if (candidate_hits_ >= 4 && std::abs(candidate_delay_ - confirmed_delay_) > static_cast<int32_t>(3 * decim_)) {
        confirmed_delay_ = candidate_delay_;
    }
}

} // namespace rmic
