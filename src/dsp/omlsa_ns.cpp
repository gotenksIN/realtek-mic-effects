#include "omlsa_ns.hpp"

#include "fftw_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace rmic {

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kInvFft = 1.0f / static_cast<float>(OmLsaPostFilter::kFftSize);

inline float hz_to_bark(float f) {
    return 13.0f * std::atan(0.00076f * f) + 3.5f * std::atan((f / 7500.0f) * (f / 7500.0f));
}

// Abramowitz & Stegun 5.1.53 / 5.1.56 approximation of E1(x) = int_x^inf (e^-t / t) dt
inline float fast_expint(float x) {
    if (x <= 1e-6f) {
        return 13.238f; // E1(1e-6)
    }
    if (x <= 1.0f) {
        const float poly = -0.57721566f +
                           x * (0.99999193f +
                           x * (-0.24991055f +
                           x * (0.05519968f +
                           x * (-0.00976004f +
                           x * 0.00107857f))));
        return -std::log(x) + poly;
    }
    if (x > 30.0f) {
        return 0.0f;
    }
    const float num = x * (x * (x * (x + 8.5733287401f) + 18.0590169730f) + 8.6347608925f) + 0.2677737343f;
    const float den = x * (x * (x * (x + 9.5733223454f) + 25.6329561486f) + 21.0996530827f) + 3.9584969228f;
    return (std::exp(-x) / x) * (num / den);
}

inline float db_to_amp(float db) {
    return std::pow(10.0f, db / 20.0f);
}
} // namespace

OmLsaPostFilter::~OmLsaPostFilter() {
    if (plan_fwd_) fftwf_destroy_plan(plan_fwd_);
    if (plan_inv_) fftwf_destroy_plan(plan_inv_);
    if (fft_real_) fftwf_free(fft_real_);
    if (fft_cpx_) fftwf_free(fft_cpx_);
    if (ifft_real_) fftwf_free(ifft_real_);
}

bool OmLsaPostFilter::init(uint32_t sample_rate, float ns_floor_db, float echo_floor_db) {
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    set_floors(ns_floor_db, echo_floor_db);

    for (uint32_t i = 0; i < kWinLen; ++i) {
        const float h = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(kWinLen)));
        win_sqrt_[i] = std::sqrt(h);
    }

    in_hist_.assign(kWinLen, 0.0f);
    ola_buf_.assign(kWinLen, 0.0f);

    noise_psd_.assign(kBins, 1e-8f);
    echo_psd_smooth_.assign(kBins, 0.0f);
    gain_prev_.assign(kBins, 1.0f);
    gamma_prev_.assign(kBins, 1.0f);
    xi_local_.assign(kBins, 0.0f);
    xi_band_.assign(kNumBarkBands, 0.0f);
    gain_smooth_.assign(kBins, 1.0f);
    speech_stagnation_.assign(kBins, 0);

    build_bark_map();

    if (!fft_real_) fft_real_ = fftwf_alloc_real(kFftSize);
    if (!fft_cpx_) fft_cpx_ = fftwf_alloc_complex(kBins);
    if (!ifft_real_) ifft_real_ = fftwf_alloc_real(kFftSize);

    if (!fft_real_ || !fft_cpx_ || !ifft_real_) {
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
            plan_inv_ = fftwf_plan_dft_c2r_1d(static_cast<int>(kFftSize), fft_cpx_, ifft_real_, FFTW_ESTIMATE);
        }
    }

    if (!plan_fwd_ || !plan_inv_) {
        return false;
    }

    reset();
    return true;
}

void OmLsaPostFilter::set_floors(float ns_floor_db, float echo_floor_db) {
    g_min_noise_ = db_to_amp(clamp_finite(ns_floor_db, -48.0f, 0.0f, -18.0f));
    g_min_echo_ = db_to_amp(clamp_finite(echo_floor_db, -60.0f, 0.0f, -34.0f));
    log_g_min_noise_ = std::log(g_min_noise_);
}

void OmLsaPostFilter::build_bark_map() {
    bin_to_band_.resize(kBins);
    band_start_.assign(kNumBarkBands, kBins);
    band_end_.assign(kNumBarkBands, 0);

    const float nyquist = 0.5f * static_cast<float>(sample_rate_);
    const float max_bark = hz_to_bark(nyquist);

    for (uint32_t k = 0; k < kBins; ++k) {
        const float hz = static_cast<float>(k) * static_cast<float>(sample_rate_) / static_cast<float>(kFftSize);
        const float b = hz_to_bark(hz) / max_bark;
        const uint32_t band = std::min(kNumBarkBands - 1, static_cast<uint32_t>(b * static_cast<float>(kNumBarkBands)));
        bin_to_band_[k] = band;
        band_start_[band] = std::min(band_start_[band], k);
        band_end_[band] = std::max(band_end_[band], k + 1);
    }
}

void OmLsaPostFilter::reset() {
    std::fill(in_hist_.begin(), in_hist_.end(), 0.0f);
    std::fill(ola_buf_.begin(), ola_buf_.end(), 0.0f);
    std::fill(noise_psd_.begin(), noise_psd_.end(), 1e-8f);
    std::fill(echo_psd_smooth_.begin(), echo_psd_smooth_.end(), 0.0f);
    std::fill(gain_prev_.begin(), gain_prev_.end(), 1.0f);
    std::fill(gamma_prev_.begin(), gamma_prev_.end(), 1.0f);
    std::fill(xi_local_.begin(), xi_local_.end(), 0.0f);
    std::fill(xi_band_.begin(), xi_band_.end(), 0.0f);
    std::fill(gain_smooth_.begin(), gain_smooth_.end(), 1.0f);
    std::fill(speech_stagnation_.begin(), speech_stagnation_.end(), 0);
    xi_frame_ = 0.0f;
    frame_count_ = 0;
}

void OmLsaPostFilter::process_hop(std::span<const float, kHop> in_hop,
                                  const float *echo_psd,
                                  std::span<float, kHop> out_hop) {
    // 1. Shift 512-sample analysis window by 256 samples, apply sqrt(Hann), zero-pad to 1024
    std::memmove(in_hist_.data(), in_hist_.data() + kHop, sizeof(float) * kHop);
    std::ranges::copy(in_hop, in_hist_.begin() + kHop);

    {
        float *__restrict__ fft_real = assume_fftw_aligned(fft_real_);
        const float *__restrict__ in_hist = in_hist_.data();
        const float *__restrict__ win = win_sqrt_.data();
        for (uint32_t i = 0; i < kWinLen; ++i) {
            fft_real[i] = in_hist[i] * win[i];
        }
        std::memset(fft_real + kWinLen, 0, sizeof(float) * (kFftSize - kWinLen));
    }
    fftwf_execute_dft_r2c(plan_fwd_, fft_real_, fft_cpx_);

    if (frame_count_ < 4) ++frame_count_;

    // 2. Unbiased MMSE Noise PSD Estimation (Gerkmann & Hendriks 2012) + Echo PSD
    // Fixed prior q=0.5, optimal SNR xi_H1 = 10^(15/10) = 31.62278
    constexpr float kXiOpt = 31.6227766f;
    constexpr float kOnePlusXiOpt = 1.0f + kXiOpt;
    constexpr float kXiOptRatio = kXiOpt / kOnePlusXiOpt;
    constexpr float kAlphaNoise = 0.88f;

    float frame_xi_sum = 0.0f;
    std::fill(xi_band_.begin(), xi_band_.end(), 0.0f);

    // First pass: update noise PSD, echo PSD, a posteriori SNR gamma, and decision-directed xi
    // Store per-bin xi and gamma in xi_local_ / gamma_prev_ for the second pass.
    {
        const fftwf_complex *__restrict__ fft_cpx = assume_fftw_aligned(fft_cpx_);
        for (uint32_t k = 0; k < kBins; ++k) {
            const float re = fft_cpx[k][0];
            const float im = fft_cpx[k][1];
            const float y2 = re * re + im * im;

            if (frame_count_ <= 3) {
                // Fast initial noise floor seeding on startup
                noise_psd_[k] = std::max(1e-10f, (noise_psd_[k] * (frame_count_ - 1) + y2) / static_cast<float>(frame_count_));
            } else {
                const float snr_post = std::min(y2 / (noise_psd_[k] + 1e-12f), 200.0f);
                // P(H1 | Y) with equal prior q = 0.5
                const float exponent = std::min(snr_post * kXiOptRatio, 40.0f);
                const float lr = kOnePlusXiOpt * std::exp(-exponent);
                float p_h1 = 1.0f / (1.0f + lr);
                p_h1 = std::min(p_h1, 0.99f);

                if (p_h1 > 0.985f) {
                    ++speech_stagnation_[k];
                } else {
                    speech_stagnation_[k] = 0;
                }
                // Prevent permanent freeze if stationary noise jumped upward
                if (speech_stagnation_[k] > 180) { // ~1 second
                    p_h1 = 0.5f;
                    speech_stagnation_[k] = 0;
                }

                const float e_n2 = (1.0f - p_h1) * y2 + p_h1 * noise_psd_[k];
                noise_psd_[k] = std::max(1e-11f, kAlphaNoise * noise_psd_[k] + (1.0f - kAlphaNoise) * e_n2);
            }

            // Residual echo PSD with late-reverb exponential decay
            float e_res = 0.0f;
            if (echo_psd) {
                const float raw_echo = echo_psd[k] * 0.20f;
                echo_psd_smooth_[k] = std::max(raw_echo, 0.72f * echo_psd_smooth_[k]);
                e_res = echo_psd_smooth_[k];
            } else {
                echo_psd_smooth_[k] = 0.0f;
            }

            const float total_interf = noise_psd_[k] + e_res + 1e-12f;
            const float gamma = std::min(y2 / total_interf, 1000.0f);

            // Decision-directed a priori SNR
            constexpr float kAlphaDd = 0.95f;
            const float prev_term = gain_prev_[k] * gain_prev_[k] * gamma_prev_[k];
            const float inst_term = std::max(gamma - 1.0f, 0.0f);
            const float xi = std::max(1e-4f, kAlphaDd * prev_term + (1.0f - kAlphaDd) * inst_term);

            gamma_prev_[k] = gamma;
            xi_local_[k] = xi;

            const uint32_t band = bin_to_band_[k];
            xi_band_[band] += xi;
            if (k >= 2 && k < kBins - 2) {
                frame_xi_sum += xi;
            }
        }
    }

    // Smooth bark bands and global frame xi
    for (uint32_t b = 0; b < kNumBarkBands; ++b) {
        const uint32_t count = band_end_[b] > band_start_[b] ? (band_end_[b] - band_start_[b]) : 1u;
        xi_band_[b] /= static_cast<float>(count);
    }
    const float frame_xi_mean = frame_xi_sum / static_cast<float>(kBins - 4);
    xi_frame_ = 0.8f * xi_frame_ + 0.2f * frame_xi_mean;

    // 3. Multi-scale Speech Presence Probability (Cohen 2001) + OM-LSA Gain
    const float p_frame = std::clamp((xi_frame_ - 0.10f) / 1.50f, 0.05f, 1.0f);

    for (uint32_t k = 0; k < kBins; ++k) {
        const float xi = xi_local_[k];
        const float gamma = gamma_prev_[k];

        // Local 3-bin average
        const uint32_t k0 = k > 0 ? k - 1 : 0;
        const uint32_t k2 = k + 1 < kBins ? k + 1 : kBins - 1;
        const float xi_loc = 0.25f * xi_local_[k0] + 0.5f * xi + 0.25f * xi_local_[k2];
        const float xi_b = xi_band_[bin_to_band_[k]];

        const float p_loc = std::clamp((xi_loc - 0.15f) / 2.0f, 0.0f, 1.0f);
        const float p_band = std::clamp((xi_b - 0.12f) / 1.8f, 0.0f, 1.0f);
        const float q_prior = std::clamp(1.0f - p_loc * p_band * p_frame, 0.02f, 0.98f);

        const float v = std::clamp(gamma * xi / (1.0f + xi), 1e-6f, 40.0f);
        // Posterior speech presence probability p(k)
        const float exp_v = std::exp(std::min(v, 35.0f));
        const float ratio = (q_prior / (1.0f - q_prior)) * (1.0f + xi) / exp_v;
        const float p_spp = 1.0f / (1.0f + ratio);

        // Ephraim-Malah MMSE-LSA gain G_H1(k)
        const float g_h1 = std::clamp((xi / (1.0f + xi)) * std::exp(0.5f * fast_expint(v)), 0.01f, 1.0f);

        // Dynamically blend floor between noise floor and deeper echo floor when echo dominates
        const float echo_frac = echo_psd_smooth_[k] / (noise_psd_[k] + echo_psd_smooth_[k] + 1e-12f);
        const float g_min = g_min_noise_ * (1.0f - echo_frac) + g_min_echo_ * echo_frac;
        g_min_bin_[k] = g_min;

        // OM-LSA gain: G_H1^p * G_min^(1-p) = exp(p*log(G_H1) + (1-p)*log(G_min)).
        // g_h1 >= 0.01 and g_min > 0, so both logs are finite. Without echo,
        // g_min equals g_min_noise_ exactly, so reuse its cached log.
        const float log_g_min = echo_frac > 0.0f ? std::log(g_min) : log_g_min_noise_;
        const float g_raw = std::max(std::exp(p_spp * std::log(g_h1) + (1.0f - p_spp) * log_g_min), g_min);
        gain_smooth_[k] = g_raw;
    }

    // 4. Inter-bin smoothing + asymmetric temporal smoothing (fast attack, smooth release)
    {
        fftwf_complex *__restrict__ fft_cpx = assume_fftw_aligned(fft_cpx_);
        const float *__restrict__ g_smooth = gain_smooth_.data();
        float *__restrict__ g_prev = gain_prev_.data();
        const float *__restrict__ g_floor = g_min_bin_.data();
        for (uint32_t k = 0; k < kBins; ++k) {
            const uint32_t k0 = k > 0 ? k - 1 : 0;
            const uint32_t k2 = k + 1 < kBins ? k + 1 : kBins - 1;
            const float g_freq = 0.15f * g_smooth[k0] + 0.70f * g_smooth[k] + 0.15f * g_smooth[k2];

            // Fast attack when speech arrives, mild release to suppress musical noise
            const float alpha_g = (g_freq > g_prev[k]) ? 0.10f : 0.35f;
            const float g = std::max(alpha_g * g_prev[k] + (1.0f - alpha_g) * g_freq, g_floor[k]);
            g_prev[k] = g;

            fft_cpx[k][0] *= g;
            fft_cpx[k][1] *= g;
        }
    }

    // 5. IFFT + sqrt(Hann) synthesis + overlap-add
    fftwf_execute_dft_c2r(plan_inv_, fft_cpx_, ifft_real_);

    {
        float *__restrict__ ola = ola_buf_.data();
        const float *__restrict__ ifft_real = assume_fftw_aligned(ifft_real_);
        const float *__restrict__ win = win_sqrt_.data();
        for (uint32_t i = 0; i < kWinLen; ++i) {
            ola[i] += ifft_real[i] * kInvFft * win[i];
        }
    }

    std::copy_n(ola_buf_.begin(), kHop, out_hop.begin());
    std::memmove(ola_buf_.data(), ola_buf_.data() + kHop, sizeof(float) * kHop);
    std::memset(ola_buf_.data() + kHop, 0, sizeof(float) * kHop);
}

} // namespace rmic
