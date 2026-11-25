#include "rmicdsp.h"

#include "aec.hpp"
#include "delay_est.hpp"
#include "fftw_util.hpp"
#include "hpf.hpp"
#include "omlsa_ns.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <vector>

namespace rmic {

bool ensure_fftw_planner_thread_safe() {
    static std::mutex init_mutex;
    static bool initialized = false;
    std::scoped_lock lock(init_mutex);
    if (initialized) return true;

    // Keep libfftw3f_threads resident once its planner lock callbacks are
    // registered in libfftw3f, even if a host later unloads this plugin.
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&fftwf_make_planner_thread_safe), &info) == 0 ||
        info.dli_fname == nullptr) {
        return false;
    }
    if (dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE) == nullptr) {
        return false;
    }
    fftwf_make_planner_thread_safe();
    initialized = true;
    return true;
}

} // namespace rmic

struct rmic_pipeline {
    static constexpr uint32_t kHop = rmic::TwoPathAec::kHop; // 256
    static constexpr uint32_t kFifoCap = 32768;
    static_assert((kFifoCap & (kFifoCap - 1)) == 0, "FIFO indexing needs a power of 2");

    rmic_config_t cfg{};

    rmic::BiquadHpf rec_hpf;
    std::array<rmic::BiquadHpf, 2> play_hpf;
    rmic::BulkDelayEstimator delay_est;
    rmic::TwoPathAec aec;
    rmic::OmLsaPostFilter ns;

    // Preallocated input/output circular FIFOs (real-time safe)
    std::vector<float> rec_fifo;
    std::array<std::vector<float>, 2> play_fifo;
    std::vector<float> out_fifo;

    uint32_t in_read = 0;
    uint32_t in_write = 0;
    uint32_t in_count = 0;

    uint32_t out_read = 0;
    uint32_t out_write = 0;
    uint32_t out_count = 0;

    // Preallocated scratch hop buffers
    std::array<float, kHop> rec_hop{};
    std::array<std::array<float, kHop>, 2> play_hops{};
    std::array<float, kHop> play_mono_hop{};
    std::array<float, kHop> aec_out_hop{};
    std::array<float, kHop> ns_out_hop{};
    std::array<float, rmic::TwoPathAec::kBins> echo_psd{};

    float output_gain_lin = 1.0f;
};

namespace {
inline float db_to_lin(float db) {
    return std::pow(10.0f, db / 20.0f);
}

// Smooth soft clipper above 0.92 FS
inline float soft_limit(float x) {
    const float ax = std::abs(x);
    if (ax <= 0.92f) return x;
    const float excess = ax - 0.92f;
    const float y = 0.92f + 0.08f * std::tanh(excess / 0.08f);
    return x >= 0.0f ? y : -y;
}
} // namespace

extern "C" {

void rmic_config_default(rmic_config_t *cfg) {
    if (!cfg) return;
    cfg->sample_rate = 48000;
    cfg->ref_channels = 2;
    cfg->enable_hpf = true;
    cfg->hpf_cutoff_hz = 100.0f;
    cfg->enable_aec = true;
    cfg->enable_delay_est = true;
    cfg->echo_tail_ms = 170.0f;
    cfg->enable_ns = true;
    cfg->ns_floor_db = -18.0f;
    cfg->echo_floor_db = -34.0f;
    cfg->output_gain_db = 0.0f;
}

rmic_pipeline_t *rmic_pipeline_create(const rmic_config_t *cfg) {
    try {
        rmic_config_t local_cfg;
        if (cfg) {
            local_cfg = *cfg;
        } else {
            rmic_config_default(&local_cfg);
        }
        if (local_cfg.sample_rate == 0) local_cfg.sample_rate = 48000;
        local_cfg.ref_channels = std::clamp(local_cfg.ref_channels, 1u, 2u);
        local_cfg.echo_tail_ms = rmic::clamp_finite(local_cfg.echo_tail_ms, 20.0f, 256.0f, 170.0f);
        local_cfg.ns_floor_db = rmic::clamp_finite(local_cfg.ns_floor_db, -48.0f, 0.0f, -18.0f);
        local_cfg.echo_floor_db = rmic::clamp_finite(local_cfg.echo_floor_db, -60.0f, 0.0f, -34.0f);
        local_cfg.output_gain_db = rmic::clamp_finite(local_cfg.output_gain_db, -24.0f, 30.0f, 0.0f);

        auto p = std::make_unique<rmic_pipeline>();
        p->cfg = local_cfg;
        p->output_gain_lin = db_to_lin(local_cfg.output_gain_db);

        p->rec_hpf.init(local_cfg.sample_rate, local_cfg.hpf_cutoff_hz);
        for (uint32_t ch = 0; ch < 2; ++ch) {
            p->play_hpf[ch].init(local_cfg.sample_rate, 80.0f);
        }
        if (!p->delay_est.init(local_cfg.sample_rate, 512) ||
            !p->aec.init(local_cfg.sample_rate, local_cfg.ref_channels, local_cfg.echo_tail_ms) ||
            !p->ns.init(local_cfg.sample_rate, local_cfg.ns_floor_db, local_cfg.echo_floor_db)) {
            return nullptr;
        }

        p->rec_fifo.assign(rmic_pipeline::kFifoCap, 0.0f);
        p->play_fifo[0].assign(rmic_pipeline::kFifoCap, 0.0f);
        p->play_fifo[1].assign(rmic_pipeline::kFifoCap, 0.0f);
        p->out_fifo.assign(rmic_pipeline::kFifoCap, 0.0f);

        rmic_pipeline_reset(p.get());
        return p.release();
    } catch (...) {
        return nullptr;
    }
}

void rmic_pipeline_destroy(rmic_pipeline_t *p) {
    delete p;
}

void rmic_pipeline_reset(rmic_pipeline_t *p) {
    if (!p) return;
    p->rec_hpf.reset();
    p->play_hpf[0].reset();
    p->play_hpf[1].reset();
    p->delay_est.reset();
    p->aec.reset();
    p->ns.reset();

    std::fill(p->rec_fifo.begin(), p->rec_fifo.end(), 0.0f);
    std::fill(p->play_fifo[0].begin(), p->play_fifo[0].end(), 0.0f);
    std::fill(p->play_fifo[1].begin(), p->play_fifo[1].end(), 0.0f);
    std::fill(p->out_fifo.begin(), p->out_fifo.end(), 0.0f);

    p->in_read = 0;
    p->in_write = 0;
    p->in_count = 0;

    // Pre-fill output FIFO with 1 hop (256 samples) so arbitrary quantum sizes never underrun
    p->out_read = 0;
    p->out_write = rmic_pipeline::kHop;
    p->out_count = rmic_pipeline::kHop;
}

void rmic_pipeline_set_params(rmic_pipeline_t *p,
                              bool enable_hpf,
                              bool enable_aec,
                              bool enable_ns,
                              float ns_floor_db,
                              float output_gain_db) {
    if (!p) return;
    const bool ns_reenabled = (!p->cfg.enable_ns && enable_ns);
    p->cfg.enable_hpf = enable_hpf;
    p->cfg.enable_aec = enable_aec;
    p->cfg.enable_ns = enable_ns;
    if (ns_reenabled) {
        // Clear stale STFT overlap-add history so bypassed audio is never replayed.
        p->ns.reset();
    }

    const float clamped_floor = rmic::clamp_finite(ns_floor_db, -48.0f, 0.0f, -18.0f);
    const float clamped_gain = rmic::clamp_finite(output_gain_db, -24.0f, 30.0f, 0.0f);
    if (clamped_gain != p->cfg.output_gain_db) {
        p->cfg.output_gain_db = clamped_gain;
        p->output_gain_lin = db_to_lin(clamped_gain);
    }
    if (clamped_floor != p->cfg.ns_floor_db) {
        p->cfg.ns_floor_db = clamped_floor;
        p->ns.set_floors(clamped_floor, p->cfg.echo_floor_db);
    }
}

uint32_t rmic_pipeline_latency_samples(const rmic_pipeline_t *p) {
    if (!p) return 0;
    // 256 samples FIFO alignment + 256 samples STFT overlap-add when NS is enabled
    return p->cfg.enable_ns ? (rmic_pipeline::kHop * 2) : rmic_pipeline::kHop;
}

int32_t rmic_pipeline_delay_samples(const rmic_pipeline_t *p) {
    if (!p) return 0;
    return p->delay_est.delay_samples();
}

void rmic_pipeline_process(rmic_pipeline_t *p,
                           const float *rec,
                           const float *const *play,
                           float *out,
                           uint32_t n_samples) {
    if (!p || !rec || !out || n_samples == 0) return;

    const uint32_t n_ref = p->cfg.ref_channels;
    [[assume(n_ref == 1 || n_ref == 2)]];

    uint32_t offset = 0;
    while (offset < n_samples) {
        const uint32_t used = p->in_count + p->out_count;
        const uint32_t space = used < rmic_pipeline::kFifoCap ? (rmic_pipeline::kFifoCap - used) : 1u;
        const uint32_t chunk = std::min(n_samples - offset, space);

        // 1. Push sanitized capture & reference into input FIFOs
        for (uint32_t i = 0; i < chunk; ++i) {
            const uint32_t w = (p->in_write + i) & (rmic_pipeline::kFifoCap - 1);
            const float rec_s = rec[offset + i];
            p->rec_fifo[w] = std::isfinite(rec_s) ? rec_s : 0.0f;
            for (uint32_t ch = 0; ch < n_ref; ++ch) {
                const float play_s = (play && play[ch]) ? play[ch][offset + i] : 0.0f;
                p->play_fifo[ch][w] = std::isfinite(play_s) ? play_s : 0.0f;
            }
        }
        p->in_write = (p->in_write + chunk) & (rmic_pipeline::kFifoCap - 1);
        p->in_count += chunk;

        // 2. Process complete 256-sample hops
        while (p->in_count >= rmic_pipeline::kHop) {
            for (uint32_t i = 0; i < rmic_pipeline::kHop; ++i) {
                const uint32_t r = (p->in_read + i) & (rmic_pipeline::kFifoCap - 1);
                p->rec_hop[i] = p->rec_fifo[r];
                float mono_sum = 0.0f;
                for (uint32_t ch = 0; ch < n_ref; ++ch) {
                    const float s = p->play_fifo[ch][r];
                    p->play_hops[ch][i] = s;
                    mono_sum += s;
                }
                p->play_mono_hop[i] = mono_sum / static_cast<float>(n_ref);
            }
            p->in_read = (p->in_read + rmic_pipeline::kHop) & (rmic_pipeline::kFifoCap - 1);
            p->in_count -= rmic_pipeline::kHop;

            // 2a. High-pass conditioning
            if (p->cfg.enable_hpf) {
                p->rec_hpf.process(p->rec_hop);
                if (p->cfg.enable_aec && play) {
                    for (uint32_t ch = 0; ch < n_ref; ++ch) {
                        p->play_hpf[ch].process(p->play_hops[ch]);
                    }
                    // Update mono ref after HPF
                    for (uint32_t i = 0; i < rmic_pipeline::kHop; ++i) {
                        float s = 0.0f;
                        for (uint32_t ch = 0; ch < n_ref; ++ch) s += p->play_hops[ch][i];
                        p->play_mono_hop[i] = s / static_cast<float>(n_ref);
                    }
                }
            }

            // 2b. Bulk delay tracking + Two-Path Linear AEC
            const float *echo_psd_ptr = nullptr;
            if (p->cfg.enable_aec && play) {
                if (p->cfg.enable_delay_est) {
                    p->delay_est.feed(p->rec_hop, p->play_mono_hop);
                    p->aec.set_bulk_delay(p->delay_est.delay_samples());
                }
                const float *play_ptrs[2] = {p->play_hops[0].data(), p->play_hops[1].data()};
                p->aec.process_hop(p->rec_hop, play_ptrs, p->aec_out_hop, p->echo_psd.data());
                echo_psd_ptr = p->echo_psd.data();
            } else {
                p->aec_out_hop = p->rec_hop;
            }

            // 2c. OM-LSA Residual Echo + Stationary Noise Suppression
            const float *final_hop = p->aec_out_hop.data();
            if (p->cfg.enable_ns) {
                p->ns.process_hop(p->aec_out_hop, echo_psd_ptr, p->ns_out_hop);
                final_hop = p->ns_out_hop.data();
            }

            // 2d. Makeup gain + soft limiter -> output FIFO
            for (uint32_t i = 0; i < rmic_pipeline::kHop; ++i) {
                const uint32_t w = (p->out_write + i) & (rmic_pipeline::kFifoCap - 1);
                p->out_fifo[w] = soft_limit(final_hop[i] * p->output_gain_lin);
            }
            p->out_write = (p->out_write + rmic_pipeline::kHop) & (rmic_pipeline::kFifoCap - 1);
            p->out_count += rmic_pipeline::kHop;
        }

        // 3. Pop chunk samples from output FIFO
        const uint32_t avail = std::min(chunk, p->out_count);
        for (uint32_t i = 0; i < avail; ++i) {
            const uint32_t r = (p->out_read + i) & (rmic_pipeline::kFifoCap - 1);
            out[offset + i] = p->out_fifo[r];
        }
        p->out_read = (p->out_read + avail) & (rmic_pipeline::kFifoCap - 1);
        p->out_count -= avail;
        for (uint32_t i = avail; i < chunk; ++i) {
            out[offset + i] = 0.0f;
        }

        offset += chunk;
    }
}

} // extern "C"
