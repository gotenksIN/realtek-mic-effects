#include "rmicdsp.h"

#include <lv2/core/lv2.h>
#include <math.h>
#include <stdlib.h>

#define RMIC_NS_URI "urn:realtek-mic-effects:ns"

enum {
    PORT_IN = 0,
    PORT_OUT = 1,
    PORT_ENABLE_HPF = 2,
    PORT_ENABLE_NS = 3,
    PORT_NS_FLOOR_DB = 4,
    PORT_GAIN_DB = 5,
    PORT_LATENCY = 6
};

typedef struct {
    const float *in;
    float *out;
    const float *enable_hpf;
    const float *enable_ns;
    const float *ns_floor_db;
    const float *gain_db;
    float *latency;

    rmic_pipeline_t *pipe;
} RmicNsLv2;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor,
                              double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
    RmicNsLv2 *self = (RmicNsLv2 *)calloc(1, sizeof(RmicNsLv2));
    if (!self) return nullptr;

    rmic_config_t cfg;
    rmic_config_default(&cfg);
    cfg.sample_rate = (isfinite(rate) && rate >= 8000.0 && rate <= 384000.0)
                    ? (uint32_t)lround(rate)
                    : 48000u;
    cfg.enable_aec = false;
    cfg.enable_delay_est = false;
    cfg.enable_hpf = true;
    cfg.enable_ns = true;

    self->pipe = rmic_pipeline_create(&cfg);
    if (!self->pipe) {
        free(self);
        return nullptr;
    }
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
    RmicNsLv2 *self = (RmicNsLv2 *)instance;
    switch (port) {
    case PORT_IN: self->in = (const float *)data; break;
    case PORT_OUT: self->out = (float *)data; break;
    case PORT_ENABLE_HPF: self->enable_hpf = (const float *)data; break;
    case PORT_ENABLE_NS: self->enable_ns = (const float *)data; break;
    case PORT_NS_FLOOR_DB: self->ns_floor_db = (const float *)data; break;
    case PORT_GAIN_DB: self->gain_db = (const float *)data; break;
    case PORT_LATENCY: self->latency = (float *)data; break;
    default: break;
    }
}

static void activate(LV2_Handle instance) {
    RmicNsLv2 *self = (RmicNsLv2 *)instance;
    if (self->pipe) {
        rmic_pipeline_reset(self->pipe);
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    RmicNsLv2 *self = (RmicNsLv2 *)instance;
    if (!self->pipe) return;

    const bool hpf = self->enable_hpf ? (*self->enable_hpf >= 0.5f) : true;
    const bool ns = self->enable_ns ? (*self->enable_ns >= 0.5f) : true;
    const float floor_db = self->ns_floor_db ? *self->ns_floor_db : -18.0f;
    const float gain_db = self->gain_db ? *self->gain_db : 0.0f;

    rmic_pipeline_set_params(self->pipe, hpf, false, ns, floor_db, gain_db);

    if (n_samples > 0 && self->in && self->out) {
        rmic_pipeline_process(self->pipe, self->in, nullptr, self->out, n_samples);
    }

    if (self->latency) {
        *self->latency = (float)rmic_pipeline_latency_samples(self->pipe);
    }
}

static void deactivate(LV2_Handle instance) {
    (void)instance;
}

static void cleanup(LV2_Handle instance) {
    RmicNsLv2 *self = (RmicNsLv2 *)instance;
    if (self->pipe) {
        rmic_pipeline_destroy(self->pipe);
    }
    free(self);
}

static const void *extension_data(const char *uri) {
    (void)uri;
    return nullptr;
}

static const LV2_Descriptor descriptor = {
    RMIC_NS_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
