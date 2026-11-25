#include "aec.hpp"

#include "fftw_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rmic {

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kInvFft = 1.0f / static_cast<float>(TwoPathAec::kFftSize);
} // namespace

TwoPathAec::~TwoPathAec() {
    if (plan_fwd_) fftwf_destroy_plan(plan_fwd_);
    if (plan_inv_) fftwf_destroy_plan(plan_inv_);
    if (fft_real_) fftwf_free(fft_real_);
    if (fft_cpx_) fftwf_free(fft_cpx_);
    if (y_main_cpx_) fftwf_free(y_main_cpx_);
    if (y_shadow_cpx_) fftwf_free(y_shadow_cpx_);
    if (err_cpx_) fftwf_free(err_cpx_);
    if (grad_cpx_) fftwf_free(grad_cpx_);
    if (y_main_time_) fftwf_free(y_main_time_);
    if (y_shadow_time_) fftwf_free(y_shadow_time_);
    if (grad_time_) fftwf_free(grad_time_);
}

bool TwoPathAec::init(uint32_t sample_rate, uint32_t ref_channels, float echo_tail_ms) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    ref_channels_ = std::clamp(ref_channels, 1u, 2u);

    const float tail_ms = clamp_finite(echo_tail_ms, 20.0f, 256.0f, 170.0f);
    const float tail_samples = tail_ms * 0.001f * static_cast<float>(sample_rate_);
    const uint32_t parts = static_cast<uint32_t>(std::ceil(tail_samples / static_cast<float>(kPartLen)));
    num_parts_ = std::clamp(parts, 4u, kMaxPartitions);

    ref_delay_ring_.assign(ref_channels_, std::vector<float>(kDelayRingSize, 0.0f));
    x_time_buf_.assign(ref_channels_, std::vector<float>(kFftSize, 0.0f));

    x_hist_.assign(ref_channels_ * kHistLen * kBins * 2, 0.0f);
    w_main_.assign(ref_channels_ * num_parts_ * kBins * 2, 0.0f);
    w_shadow_.assign(ref_channels_ * num_parts_ * kBins * 2, 0.0f);
    s_x_.assign(kBins, 1e-6f);

    e_main_.assign(kHop, 0.0f);
    e_shadow_.assign(kHop, 0.0f);
    y_main_hist_.assign(kPartLen, 0.0f);

    for (uint32_t i = 0; i < kPartLen; ++i) {
        const float h = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kPartLen)));
        win_hann_sqrt_[i] = std::sqrt(h);
    }

    if (!fft_real_) fft_real_ = fftwf_alloc_real(kFftSize);
    if (!fft_cpx_) fft_cpx_ = fftwf_alloc_complex(kBins);
    if (!y_main_cpx_) y_main_cpx_ = fftwf_alloc_complex(kBins);
    if (!y_shadow_cpx_) y_shadow_cpx_ = fftwf_alloc_complex(kBins);
    if (!err_cpx_) err_cpx_ = fftwf_alloc_complex(kBins);
    if (!grad_cpx_) grad_cpx_ = fftwf_alloc_complex(kBins);
    if (!y_main_time_) y_main_time_ = fftwf_alloc_real(kFftSize);
    if (!y_shadow_time_) y_shadow_time_ = fftwf_alloc_real(kFftSize);
    if (!grad_time_) grad_time_ = fftwf_alloc_real(kFftSize);

    if (!fft_real_ || !fft_cpx_ || !y_main_cpx_ || !y_shadow_cpx_ ||
        !err_cpx_ || !grad_cpx_ || !y_main_time_ || !y_shadow_time_ || !grad_time_) {
        return false;
    }

    if (!plan_fwd_ || !plan_inv_) {
        if (!ensure_fftw_planner_thread_safe()) {
            return false;
        }
        if (!plan_fwd_) {
            plan_fwd_ = fftwf_plan_dft_r2c_1d(static_cast<int>(kFftSize), fft_real_, fft_cpx_, FFTW_ESTIMATE);
        }
        if (!plan_inv_) {
            plan_inv_ = fftwf_plan_dft_c2r_1d(static_cast<int>(kFftSize), y_main_cpx_, y_main_time_, FFTW_ESTIMATE);
        }
    }

    if (!plan_fwd_ || !plan_inv_) {
        return false;
    }

    reset();
    return true;
}

void TwoPathAec::reset() {
    for (auto &r : ref_delay_ring_) std::fill(r.begin(), r.end(), 0.0f);
    for (auto &x : x_time_buf_) std::fill(x.begin(), x.end(), 0.0f);
    std::fill(x_hist_.begin(), x_hist_.end(), 0.0f);
    std::fill(w_main_.begin(), w_main_.end(), 0.0f);
    std::fill(w_shadow_.begin(), w_shadow_.end(), 0.0f);
    std::fill(s_x_.begin(), s_x_.end(), 1e-6f);
    std::fill(e_main_.begin(), e_main_.end(), 0.0f);
    std::fill(e_shadow_.begin(), e_shadow_.end(), 0.0f);
    std::fill(y_main_hist_.begin(), y_main_hist_.end(), 0.0f);

    delay_write_pos_ = 0;
    hist_head_ = 0;
    bulk_delay_ = 0;
    p_d_ = p_m_ = p_s_ = p_x_ = 0.0f;
    shadow_better_count_ = 0;
}

void TwoPathAec::set_bulk_delay(int32_t delay_samples) {
    const int32_t clamped = std::clamp(delay_samples, 0, static_cast<int32_t>(kDelayRingSize - kFftSize - 1));
    if (std::abs(clamped - bulk_delay_) > 128) {
        bulk_delay_ = clamped;
        // Re-align filter adaptation smoothly after bulk delay jump
        std::fill(w_shadow_.begin(), w_shadow_.end(), 0.0f);
        std::fill(w_main_.begin(), w_main_.end(), 0.0f);
        p_m_ = p_s_ = p_d_;
        shadow_better_count_ = 0;
    }
}

void TwoPathAec::process_hop(std::span<const float, kHop> rec_hop,
                             const float *const *play_hops,
                             std::span<float, kHop> out_hop,
                             float *echo_psd_out) {
    const uint32_t n_ch = ref_channels_;
    const uint32_t n_parts = num_parts_;
    [[assume(n_ch >= 1 && n_ch <= 2)]];
    [[assume(n_parts >= 4 && n_parts <= kMaxPartitions)]];

    if (!play_hops) {
        std::ranges::copy(rec_hop, out_hop.begin());
        if (echo_psd_out) {
            std::memset(echo_psd_out, 0, sizeof(float) * kBins);
        }
        return;
    }

    // 1. Push reference hops into delay rings and read bulk-aligned 256-sample frames
    float hop_x_energy = 0.0f;
    for (uint32_t ch = 0; ch < n_ch; ++ch) {
        const float *src = play_hops[ch];
        float *ring = ref_delay_ring_[ch].data();
        for (uint32_t i = 0; i < kHop; ++i) {
            ring[(delay_write_pos_ + i) & (kDelayRingSize - 1)] = src ? src[i] : 0.0f;
        }

        float *xb = x_time_buf_[ch].data();
        std::memmove(xb, xb + kHop, sizeof(float) * (kFftSize - kHop));

        const uint32_t read_base = (delay_write_pos_ - static_cast<uint32_t>(bulk_delay_)) & (kDelayRingSize - 1);
        for (uint32_t i = 0; i < kHop; ++i) {
            const float s = ring[(read_base + i) & (kDelayRingSize - 1)];
            xb[kFftSize - kHop + i] = s;
            hop_x_energy += s * s;
        }

        // Compute FFT of 1024-sample reference frame and store in x_hist_
        std::memcpy(fft_real_, xb, sizeof(float) * kFftSize);
        fftwf_execute_dft_r2c(plan_fwd_, fft_real_, fft_cpx_);
        float *dst = &x_hist_[(ch * kHistLen + hist_head_) * kBins * 2];
        std::memcpy(dst, fft_cpx_, sizeof(float) * kBins * 2);
    }
    delay_write_pos_ = (delay_write_pos_ + kHop) & (kDelayRingSize - 1);

    hop_x_energy /= static_cast<float>(kHop * n_ch);
    p_x_ = 0.9f * p_x_ + 0.1f * hop_x_energy;

    // 2. Accumulate Main & Shadow frequency-domain echo estimates + partition power.
    // Loop over bins innermost so the compiler can vectorize. Each bin still
    // sums channels and partitions in the same order.
    float mean_sx = 0.0f;
    {
        fftwf_complex *__restrict__ ym = assume_fftw_aligned(y_main_cpx_);
        fftwf_complex *__restrict__ ys = assume_fftw_aligned(y_shadow_cpx_);
        float *__restrict__ px = px_scratch_.data();
        std::memset(ym, 0, sizeof(fftwf_complex) * kBins);
        std::memset(ys, 0, sizeof(fftwf_complex) * kBins);
        std::fill_n(px, kBins, 0.0f);

        for (uint32_t ch = 0; ch < n_ch; ++ch) {
            for (uint32_t p = 0; p < n_parts; ++p) {
                const uint32_t slot = (hist_head_ - kStride * p) & (kHistLen - 1);
                const float *__restrict__ x = &x_hist_[(ch * kHistLen + slot) * kBins * 2];
                const size_t w_base = static_cast<size_t>(ch * n_parts + p) * kBins * 2;
                const float *__restrict__ wm = &w_main_[w_base];
                const float *__restrict__ ws = &w_shadow_[w_base];

                for (uint32_t k = 0; k < kBins; ++k) {
                    const float xr = x[2 * k];
                    const float xi = x[2 * k + 1];
                    px[k] += xr * xr + xi * xi;

                    const float wmr = wm[2 * k];
                    const float wmi = wm[2 * k + 1];
                    ym[k][0] += xr * wmr - xi * wmi;
                    ym[k][1] += xr * wmi + xi * wmr;

                    const float wsr = ws[2 * k];
                    const float wsi = ws[2 * k + 1];
                    ys[k][0] += xr * wsr - xi * wsi;
                    ys[k][1] += xr * wsi + xi * wsr;
                }
            }
        }

        float *__restrict__ s_x = s_x_.data();
        for (uint32_t k = 0; k < kBins; ++k) {
            s_x[k] = 0.85f * s_x[k] + 0.15f * px[k];
            mean_sx += s_x[k];
        }
    }
    mean_sx /= static_cast<float>(kBins);

    // 3. IFFT both Main and Shadow echo signals (overlap-save: last kHop samples are linear conv)
    fftwf_execute_dft_c2r(plan_inv_, y_main_cpx_, y_main_time_);
    fftwf_execute_dft_c2r(plan_inv_, y_shadow_cpx_, y_shadow_time_);

    float hop_d = 0.0f;
    float hop_m = 0.0f;
    float hop_s = 0.0f;
    const uint32_t valid_off = kFftSize - kHop; // 768

    {
        const float *__restrict__ ym_time = assume_fftw_aligned(y_main_time_);
        const float *__restrict__ ys_time = assume_fftw_aligned(y_shadow_time_);
        float *__restrict__ em_buf = e_main_.data();
        float *__restrict__ es_buf = e_shadow_.data();
        for (uint32_t i = 0; i < kHop; ++i) {
            const float d = rec_hop[i];
            const float ym = ym_time[valid_off + i] * kInvFft;
            const float ys = ys_time[valid_off + i] * kInvFft;

            const float em = d - ym;
            const float es = d - ys;

            em_buf[i] = em;
            es_buf[i] = es;

            hop_d += d * d;
            hop_m += em * em;
            hop_s += es * es;
        }
    }

    hop_d /= static_cast<float>(kHop);
    hop_m /= static_cast<float>(kHop);
    hop_s /= static_cast<float>(kHop);

    p_d_ = 0.9f * p_d_ + 0.1f * hop_d;
    p_m_ = 0.9f * p_m_ + 0.1f * hop_m;
    p_s_ = 0.9f * p_s_ + 0.1f * hop_s;

    // 4. Shadow filter update (only when playback reference is active)
    if (p_x_ > 1e-9f) {
        std::memset(fft_real_, 0, sizeof(float) * valid_off);
        std::memcpy(fft_real_ + valid_off, e_shadow_.data(), sizeof(float) * kHop);
        fftwf_execute_dft_r2c(plan_fwd_, fft_real_, err_cpx_);

        // Adaptive step size with proportional regularization, computed once per hop
        const float delta = std::max(1e-7f, 0.02f * mean_sx);
        const float mu = 0.45f;
        for (uint32_t k = 0; k < kBins; ++k) {
            step_scratch_[k] = mu / (s_x_[k] + delta);
        }

        for (uint32_t ch = 0; ch < n_ch; ++ch) {
            for (uint32_t p = 0; p < n_parts; ++p) {
                const uint32_t slot = (hist_head_ - kStride * p) & (kHistLen - 1);
                {
                    const float *__restrict__ x = &x_hist_[(ch * kHistLen + slot) * kBins * 2];
                    const fftwf_complex *__restrict__ err = assume_fftw_aligned(err_cpx_);
                    fftwf_complex *__restrict__ grad = assume_fftw_aligned(grad_cpx_);
                    const float *__restrict__ step = step_scratch_.data();
                    for (uint32_t k = 0; k < kBins; ++k) {
                        const float xr = x[2 * k];
                        const float xi = x[2 * k + 1];
                        const float er = err[k][0];
                        const float ei = err[k][1];

                        // conj(X) * E * step
                        grad[k][0] = (xr * er + xi * ei) * step[k];
                        grad[k][1] = (xr * ei - xi * er) * step[k];
                    }
                }

                // Gradient constraint: IFFT -> zero [kPartLen .. kFftSize-1] -> FFT
                fftwf_execute_dft_c2r(plan_inv_, grad_cpx_, grad_time_);
                {
                    const float *__restrict__ grad_time = assume_fftw_aligned(grad_time_);
                    float *__restrict__ fft_real = assume_fftw_aligned(fft_real_);
                    for (uint32_t i = 0; i < kPartLen; ++i) {
                        fft_real[i] = grad_time[i] * kInvFft;
                    }
                    std::memset(fft_real + kPartLen, 0, sizeof(float) * (kFftSize - kPartLen));
                }
                fftwf_execute_dft_r2c(plan_fwd_, fft_real_, grad_cpx_);

                {
                    const fftwf_complex *__restrict__ grad = assume_fftw_aligned(grad_cpx_);
                    float *__restrict__ w = &w_shadow_[static_cast<size_t>(ch * n_parts + p) * kBins * 2];
                    for (uint32_t k = 0; k < kBins; ++k) {
                        // Slight leakage prevents unbounded drift on silent bins
                        w[2 * k] = w[2 * k] * 0.9999f + grad[k][0];
                        w[2 * k + 1] = w[2 * k + 1] * 0.9999f + grad[k][1];
                    }
                }
            }
        }
    }

    // 5. Two-path shadow <-> main transfer & divergence guard.
    // Evaluate emitted-hop safety against the pre-transfer main power because
    // e_main_ and y_main_time_ were produced by the pre-transfer main filter.
    const float p_m_emit = p_m_;
    bool main_promoted = false;
    if (p_s_ > 1.8f * std::max(p_d_, p_m_) && p_s_ > 1e-8f) {
        // Shadow diverged (e.g. loud double-talk impulse): restore from main
        std::memcpy(w_shadow_.data(), w_main_.data(), sizeof(float) * w_main_.size());
        p_s_ = p_m_;
        shadow_better_count_ = 0;
    } else if (p_x_ > 1e-8f && p_s_ < 0.85f * p_m_ && p_s_ < 0.92f * p_d_) {
        if (++shadow_better_count_ >= 3) {
            std::memcpy(w_main_.data(), w_shadow_.data(), sizeof(float) * w_main_.size());
            p_m_ = p_s_;
            shadow_better_count_ = 0;
            main_promoted = true;
        }
    } else if (p_x_ > 1e-8f && p_m_ >= p_d_ * 0.99f && p_s_ < 0.95f * p_d_) {
        // Initial fast catch-up when main hasn't converged yet
        std::memcpy(w_main_.data(), w_shadow_.data(), sizeof(float) * w_main_.size());
        p_m_ = p_s_;
        main_promoted = true;
    } else {
        shadow_better_count_ = 0;
    }

    // 6. Emit safe main error (if pre-transfer main filter exceeded raw mic power, clamp safely)
    const bool main_safe = (p_m_emit <= 1.25f * p_d_ + 1e-9f);
    if (main_safe) {
        std::ranges::copy(e_main_, out_hop.begin());
    } else {
        std::ranges::copy(rec_hop, out_hop.begin());
        if (!main_promoted) {
            std::fill(w_main_.begin(), w_main_.end(), 0.0f);
            p_m_ = p_d_;
        }
    }

    // 7. Compute sqrt-Hann(512)-windowed linear echo PSD for joint OM-LSA post-filter
    std::memmove(y_main_hist_.data(), y_main_hist_.data() + kHop, sizeof(float) * kHop);
    if (main_safe) {
        const float *__restrict__ ym_time = assume_fftw_aligned(y_main_time_);
        float *__restrict__ ym_hist = y_main_hist_.data() + kHop;
        for (uint32_t i = 0; i < kHop; ++i) {
            ym_hist[i] = ym_time[valid_off + i] * kInvFft;
        }
    } else {
        std::memset(y_main_hist_.data() + kHop, 0, sizeof(float) * kHop);
    }

    if (echo_psd_out) {
        {
            float *__restrict__ fft_real = assume_fftw_aligned(fft_real_);
            const float *__restrict__ ym_hist = y_main_hist_.data();
            const float *__restrict__ win = win_hann_sqrt_.data();
            for (uint32_t i = 0; i < kPartLen; ++i) {
                fft_real[i] = ym_hist[i] * win[i];
            }
            std::memset(fft_real + kPartLen, 0, sizeof(float) * (kFftSize - kPartLen));
        }
        fftwf_execute_dft_r2c(plan_fwd_, fft_real_, fft_cpx_);
        {
            const fftwf_complex *__restrict__ fft_cpx = assume_fftw_aligned(fft_cpx_);
            float *__restrict__ echo_psd = echo_psd_out;
            for (uint32_t k = 0; k < kBins; ++k) {
                const float re = fft_cpx[k][0];
                const float im = fft_cpx[k][1];
                echo_psd[k] = re * re + im * im;
            }
        }
    }

    hist_head_ = (hist_head_ + 1) & (kHistLen - 1);
}

} // namespace rmic
