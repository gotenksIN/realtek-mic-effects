#pragma once

#include <cstdint>
#include <span>

namespace rmic {

class BiquadHpf {
public:
    void init(uint32_t sample_rate, float cutoff_hz);
    void reset();
    // Filter the buffer in place.
    void process(std::span<float> buf);

private:
    struct Section {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
    };

    Section s1_{};
    Section s2_{};
    bool initialized_ = false;
};

} // namespace rmic
