#pragma once

#include <array>
#include <cstdint>
#include <fftw3.h>
#include <span>
#include <vector>

namespace rmic {

// Bark-band OM-LSA (Optimally-Modified Log-Spectral Amplitude) post-filter
// with Unbiased MMSE Noise PSD Estimation (Gerkmann & Hendriks) and
// joint residual-echo suppression.
class OmLsaPostFilter {
public:
    static constexpr uint32_t kHop = 256;
    static constexpr uint32_t kWinLen = 512;
    static constexpr uint32_t kFftSize = 1024;
    static constexpr uint32_t kBins = kFftSize / 2 + 1; // 513
    static constexpr uint32_t kNumBarkBands = 32;

    OmLsaPostFilter() = default;
    ~OmLsaPostFilter();

    OmLsaPostFilter(const OmLsaPostFilter &) = delete;
    OmLsaPostFilter &operator=(const OmLsaPostFilter &) = delete;

    [[nodiscard]] bool init(uint32_t sample_rate, float ns_floor_db, float echo_floor_db);
    void reset();
    void set_floors(float ns_floor_db, float echo_floor_db);

    // Process one 256-sample hop.
    // - in_hop: linear-AEC output (or HPF mic if AEC disabled)
    // - echo_psd: optional [513] sqrt-Hann(512)-windowed linear echo PSD, or nullptr
    // - out_hop: denoised output (latency = 256 samples from STFT overlap-add)
    void process_hop(std::span<const float, kHop> in_hop,
                     const float *echo_psd,
                     std::span<float, kHop> out_hop);

private:
    void build_bark_map();

    uint32_t sample_rate_ = 48000;
    float g_min_noise_ = 0.12589f; // -18 dB
    float g_min_echo_ = 0.01995f;  // -34 dB
    float log_g_min_noise_ = -2.0723f; // log(g_min_noise_)

    std::array<float, kWinLen> win_sqrt_{};
    std::vector<float> in_hist_;   // [kWinLen]
    std::vector<float> ola_buf_;   // [kWinLen]

    std::vector<uint32_t> bin_to_band_;
    std::vector<uint32_t> band_start_;
    std::vector<uint32_t> band_end_;

    std::vector<float> noise_psd_;
    std::vector<float> echo_psd_smooth_;
    std::vector<float> gain_prev_;
    std::vector<float> gamma_prev_;
    std::vector<float> xi_local_;
    std::vector<float> xi_band_;
    std::vector<float> gain_smooth_;
    std::array<float, kBins> g_min_bin_{};
    std::vector<uint32_t> speech_stagnation_;

    float xi_frame_ = 0.0f;
    uint32_t frame_count_ = 0;

    float *fft_real_ = nullptr;
    fftwf_complex *fft_cpx_ = nullptr;
    float *ifft_real_ = nullptr;
    fftwf_plan plan_fwd_ = nullptr;
    fftwf_plan plan_inv_ = nullptr;
};

} // namespace rmic
