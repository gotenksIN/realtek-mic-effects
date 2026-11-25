#include "hpf.hpp"

#include <cmath>
#include <numbers>

namespace rmic {

namespace {
constexpr double kPi = std::numbers::pi;

void design_hpf_biquad(double fs, double fc, double q,
                       double &b0, double &b1, double &b2,
                       double &a1, double &a2) {
    const double w0 = 2.0 * kPi * fc / fs;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha = sin_w0 / (2.0 * q);
    const double a0 = 1.0 + alpha;

    b0 = ((1.0 + cos_w0) * 0.5) / a0;
    b1 = (-(1.0 + cos_w0)) / a0;
    b2 = ((1.0 + cos_w0) * 0.5) / a0;
    a1 = (-2.0 * cos_w0) / a0;
    a2 = (1.0 - alpha) / a0;
}
} // namespace

void BiquadHpf::init(uint32_t sample_rate, float cutoff_hz) {
    if (sample_rate == 0 || !std::isfinite(cutoff_hz) || cutoff_hz <= 0.0f ||
        cutoff_hz >= 0.45f * static_cast<float>(sample_rate)) {
        initialized_ = false;
        return;
    }
    const double fs = static_cast<double>(sample_rate);
    const double fc = static_cast<double>(cutoff_hz);
    // 4th-order Butterworth HPF: two cascaded 2nd-order sections
    constexpr double q1 = 0.5411961001461970;
    constexpr double q2 = 1.3065629648763766;
    design_hpf_biquad(fs, fc, q1, s1_.b0, s1_.b1, s1_.b2, s1_.a1, s1_.a2);
    design_hpf_biquad(fs, fc, q2, s2_.b0, s2_.b1, s2_.b2, s2_.a1, s2_.a2);
    reset();
    initialized_ = true;
}

void BiquadHpf::reset() {
    s1_.z1 = s1_.z2 = 0.0;
    s2_.z1 = s2_.z2 = 0.0;
}

void BiquadHpf::process(std::span<float> buf) {
    if (!initialized_) return;

    // Keep the filter state in registers for the whole block.
    Section s1 = s1_;
    Section s2 = s2_;
    for (float &sample : buf) {
        const double x = static_cast<double>(sample);
        // Direct Form II Transposed for numerical stability
        const double y1 = s1.b0 * x + s1.z1;
        s1.z1 = s1.b1 * x - s1.a1 * y1 + s1.z2;
        s1.z2 = s1.b2 * x - s1.a2 * y1;

        const double y2 = s2.b0 * y1 + s2.z1;
        s2.z1 = s2.b1 * y1 - s2.a1 * y2 + s2.z2;
        s2.z2 = s2.b2 * y1 - s2.a2 * y2;

        sample = static_cast<float>(y2);
    }
    // Flush subnormal state once per block for hosts without FTZ/DAZ.
    if (std::abs(s1.z1) < 1e-30) s1.z1 = 0.0;
    if (std::abs(s1.z2) < 1e-30) s1.z2 = 0.0;
    if (std::abs(s2.z1) < 1e-30) s2.z1 = 0.0;
    if (std::abs(s2.z2) < 1e-30) s2.z2 = 0.0;
    s1_ = s1;
    s2_ = s2;
}

} // namespace rmic
