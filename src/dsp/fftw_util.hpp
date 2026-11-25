#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fftw3.h>

namespace rmic {

// fftwf_malloc returns memory aligned for the SIMD instructions FFTW was
// built with. Every x86-64 SIMD build guarantees at least 16 bytes.
inline constexpr std::size_t kFftwAlign = 16;

// Tell the compiler that a pointer from fftwf_alloc_* is SIMD-aligned.
template <typename T>
[[nodiscard]] inline T *assume_fftw_aligned(T *p) {
    return static_cast<T *>(__builtin_assume_aligned(p, kFftwAlign));
}

// Enable FFTW's process-wide planner lock once so concurrent create/destroy
// across separate shared plugins cannot race on FFTW's global planner state.
// Pins libfftw3f_threads resident for the process lifetime so callbacks installed
// in libfftw3f never dangle if a host unloads a plugin.
// Call only during pipeline initialization, never on the audio thread.
[[nodiscard]] bool ensure_fftw_planner_thread_safe();

// Clamp finite values to [lo, hi]; replace NaN/Inf with fallback.
[[nodiscard]] inline float clamp_finite(float x, float lo, float hi, float fallback) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}

} // namespace rmic
