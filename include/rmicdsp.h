#ifndef RMICDSP_H
#define RMICDSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rmic_config {
    uint32_t sample_rate;       /* 44100 or 48000 (default 48000) */
    uint32_t ref_channels;      /* 1 or 2 playback reference channels (default 2) */
    bool enable_hpf;            /* 4th-order Butterworth HPF (default true) */
    float hpf_cutoff_hz;        /* default 100.0 Hz */
    bool enable_aec;            /* Two-path PBFDAF AEC (default true) */
    bool enable_delay_est;      /* GCC-PHAT bulk delay tracking (default true) */
    float echo_tail_ms;         /* Linear AEC tail length in ms (default 170.0 ms, max 256 ms) */
    bool enable_ns;             /* OM-LSA stationary + residual-echo suppressor (default true) */
    float ns_floor_db;          /* Min gain floor for noise in dB (default -18.0 dB) */
    float echo_floor_db;        /* Min gain floor during residual echo in dB (default -34.0 dB) */
    float output_gain_db;       /* Post-processing digital makeup gain in dB (default 0.0 dB) */
} rmic_config_t;

typedef struct rmic_pipeline rmic_pipeline_t;

/* Populate default configuration matching Realtek Rear Mic + upgrades. */
void rmic_config_default(rmic_config_t *cfg);

/* Create/destroy full pipeline (real-time safe during process). */
[[nodiscard]] rmic_pipeline_t *rmic_pipeline_create(const rmic_config_t *cfg);
void rmic_pipeline_destroy(rmic_pipeline_t *p);
void rmic_pipeline_reset(rmic_pipeline_t *p);

/* Update runtime parameters safely (no allocations). */
void rmic_pipeline_set_params(rmic_pipeline_t *p,
                              bool enable_hpf,
                              bool enable_aec,
                              bool enable_ns,
                              float ns_floor_db,
                              float output_gain_db);

/* Query fixed internal buffering + STFT latency in samples. */
uint32_t rmic_pipeline_latency_samples(const rmic_pipeline_t *p);

/* Query current estimated bulk delay in samples (for telemetry/tests). */
int32_t rmic_pipeline_delay_samples(const rmic_pipeline_t *p);

/*
 * Process arbitrary n_samples blocks (real-time safe, zero allocations).
 * - rec: mono capture input [n_samples]
 * - play: array of ref_channels pointers [ref_channels][n_samples], or nullptr if AEC off
 * - out: mono processed output [n_samples]
 */
void rmic_pipeline_process(rmic_pipeline_t *p,
                           const float *rec,
                           const float *const *play,
                           float *out,
                           uint32_t n_samples);

#ifdef __cplusplus
}
#endif

#endif /* RMICDSP_H */
