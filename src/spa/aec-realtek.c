/*
 * PipeWire SPA Audio:AEC plugin implementing Realtek-style mic processing:
 * 4th-order 100 Hz HPF + GCC-PHAT bulk delay + Two-path PBFDAF AEC + Bark OM-LSA NS.
 */

#include "rmicdsp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/dict.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.aec.realtek");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define MAX_SCRATCH_FRAMES 16384u

struct impl {
    struct spa_handle handle;
    struct spa_audio_aec aec;

    struct spa_log *log;
    struct spa_hook_list hooks;

    rmic_config_t cfg;
    rmic_pipeline_t *pipe;

    uint32_t rec_channels;
    uint32_t play_channels;
    uint32_t out_channels;

    float mono_rec_scratch[MAX_SCRATCH_FRAMES];
    float mono_out_scratch[MAX_SCRATCH_FRAMES];
};

static void parse_props(struct impl *impl, const struct spa_dict *args, bool runtime_only) {
    if (!args) return;

    const char *v;
    float val = 0.0f;
    if ((v = spa_dict_lookup(args, "realtek.hpf")) != nullptr) {
        impl->cfg.enable_hpf = spa_atob(v);
    }
    if ((v = spa_dict_lookup(args, "realtek.aec")) != nullptr) {
        impl->cfg.enable_aec = spa_atob(v);
    }
    if ((v = spa_dict_lookup(args, "realtek.ns")) != nullptr) {
        impl->cfg.enable_ns = spa_atob(v);
    }
    if ((v = spa_dict_lookup(args, "realtek.ns_floor_db")) != nullptr) {
        if (spa_atof(v, &val) && val >= -48.0f && val <= 0.0f) {
            impl->cfg.ns_floor_db = val;
        }
    }
    if ((v = spa_dict_lookup(args, "realtek.gain_db")) != nullptr) {
        if (spa_atof(v, &val) && val >= -24.0f && val <= 30.0f) {
            impl->cfg.output_gain_db = val;
        }
    }

    if (runtime_only) return;

    if ((v = spa_dict_lookup(args, "realtek.hpf_hz")) != nullptr) {
        if (spa_atof(v, &val) && val >= 20.0f && val <= 400.0f) {
            impl->cfg.hpf_cutoff_hz = val;
        }
    }
    if ((v = spa_dict_lookup(args, "realtek.delay_est")) != nullptr) {
        impl->cfg.enable_delay_est = spa_atob(v);
    }
    if ((v = spa_dict_lookup(args, "realtek.echo_tail_ms")) != nullptr) {
        if (spa_atof(v, &val) && val >= 20.0f && val <= 256.0f) {
            impl->cfg.echo_tail_ms = val;
        }
    }
    if ((v = spa_dict_lookup(args, "realtek.echo_floor_db")) != nullptr) {
        if (spa_atof(v, &val) && val >= -60.0f && val <= 0.0f) {
            impl->cfg.echo_floor_db = val;
        }
    }
}

static int impl_add_listener(void *object,
                             struct spa_hook *listener,
                             const struct spa_audio_aec_events *events,
                             void *data) {
    struct impl *impl = object;
    spa_hook_list_append(&impl->hooks, listener, events, data);
    return 0;
}

/*
 * Note: spa/interfaces/audio/aec.h names the three info parameters
 * (play_info, rec_info, out_info), but PipeWire's module-echo-cancel
 * actually passes (&rec_info, &out_info, &play_info), matching aec-webrtc.
 */
static int impl_init2(void *object,
                      const struct spa_dict *args,
                      struct spa_audio_info_raw *rec_info,
                      struct spa_audio_info_raw *out_info,
                      struct spa_audio_info_raw *play_info) {
    struct impl *impl = object;

    rmic_config_default(&impl->cfg);
    parse_props(impl, args, false);

    const uint32_t rate = (rec_info && rec_info->rate > 0) ? rec_info->rate
                        : (play_info && play_info->rate > 0) ? play_info->rate
                        : 48000u;
    impl->rec_channels = (rec_info && rec_info->channels > 0) ? rec_info->channels : 1u;
    impl->play_channels = (play_info && play_info->channels > 0) ? play_info->channels : 2u;
    impl->out_channels = (out_info && out_info->channels > 0) ? out_info->channels : impl->rec_channels;

    impl->cfg.sample_rate = rate;
    impl->cfg.ref_channels = impl->play_channels >= 2 ? 2u : 1u;

    if (impl->pipe) {
        rmic_pipeline_destroy(impl->pipe);
        impl->pipe = nullptr;
    }
    impl->pipe = rmic_pipeline_create(&impl->cfg);
    if (!impl->pipe) {
        spa_log_error(impl->log, "failed to create rmicdsp pipeline");
        return -ENOMEM;
    }

    /*
     * Leave aec.latency unset: PipeWire's module-echo-cancel interprets it
     * as a required fixed blocksize, whereas rmicdsp handles arbitrary sizes.
     */
    impl->aec.latency = nullptr;

    spa_log_info(impl->log,
                 "init realtek aec: rate=%u rec_ch=%u play_ch=%u out_ch=%u hpf=%d aec=%d ns=%d tail=%.0fms floor=%.1fdB gain=%.1fdB",
                 rate, impl->rec_channels, impl->play_channels, impl->out_channels,
                 impl->cfg.enable_hpf, impl->cfg.enable_aec, impl->cfg.enable_ns,
                 impl->cfg.echo_tail_ms, impl->cfg.ns_floor_db, impl->cfg.output_gain_db);
    return 0;
}

static int impl_run(void *object,
                    const float *rec[],
                    const float *play[],
                    float *out[],
                    uint32_t n_samples) {
    struct impl *impl = object;
    if (!impl->pipe || !rec || !out || n_samples == 0) {
        return 0;
    }

    uint32_t offset = 0;
    while (offset < n_samples) {
        const uint32_t chunk = (n_samples - offset > MAX_SCRATCH_FRAMES)
                             ? MAX_SCRATCH_FRAMES
                             : (n_samples - offset);

        const float *rec_mono;
        if (impl->rec_channels > 1) {
            const float inv = 1.0f / (float)impl->rec_channels;
            for (uint32_t i = 0; i < chunk; ++i) {
                float sum = 0.0f;
                for (uint32_t ch = 0; ch < impl->rec_channels; ++ch) {
                    sum += rec[ch] ? rec[ch][offset + i] : 0.0f;
                }
                impl->mono_rec_scratch[i] = sum * inv;
            }
            rec_mono = impl->mono_rec_scratch;
        } else if (rec[0]) {
            rec_mono = rec[0] + offset;
        } else {
            memset(impl->mono_rec_scratch, 0, sizeof(float) * chunk);
            rec_mono = impl->mono_rec_scratch;
        }

        const float *play_ptrs[2] = {nullptr, nullptr};
        if (play) {
            play_ptrs[0] = play[0] ? (play[0] + offset) : nullptr;
            if (impl->play_channels >= 2 && play[1]) {
                play_ptrs[1] = play[1] + offset;
            }
        }

        float *out_target = (impl->out_channels == 1 && out[0])
                          ? (out[0] + offset)
                          : impl->mono_out_scratch;

        rmic_pipeline_process(impl->pipe,
                              rec_mono,
                              play ? play_ptrs : nullptr,
                              out_target,
                              chunk);

        if (impl->out_channels > 1 || out_target == impl->mono_out_scratch) {
            for (uint32_t ch = 0; ch < impl->out_channels; ++ch) {
                if (out[ch]) {
                    memcpy(out[ch] + offset, out_target, sizeof(float) * chunk);
                }
            }
        }

        offset += chunk;
    }

    return 0;
}

static int impl_set_props(void *object, const struct spa_dict *args) {
    struct impl *impl = object;
    parse_props(impl, args, true);
    if (impl->pipe) {
        rmic_pipeline_set_params(impl->pipe,
                                 impl->cfg.enable_hpf,
                                 impl->cfg.enable_aec,
                                 impl->cfg.enable_ns,
                                 impl->cfg.ns_floor_db,
                                 impl->cfg.output_gain_db);
    }
    return 0;
}

static int impl_activate(void *object) {
    struct impl *impl = object;
    if (impl->pipe) {
        rmic_pipeline_reset(impl->pipe);
    }
    return 0;
}

static int impl_deactivate(void *object) {
    struct impl *impl = object;
    if (impl->pipe) {
        rmic_pipeline_reset(impl->pipe);
    }
    return 0;
}

static const struct spa_audio_aec_methods impl_aec_methods = {
    SPA_VERSION_AUDIO_AEC_METHODS,
    .add_listener = impl_add_listener,
    .run = impl_run,
    .set_props = impl_set_props,
    .activate = impl_activate,
    .deactivate = impl_deactivate,
    .init2 = impl_init2,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **iface) {
    struct impl *impl;
    spa_return_val_if_fail(handle != nullptr, -EINVAL);
    spa_return_val_if_fail(iface != nullptr, -EINVAL);

    impl = (struct impl *)handle;
    if (spa_streq(type, SPA_TYPE_INTERFACE_AUDIO_AEC)) {
        *iface = &impl->aec;
        return 0;
    }
    return -ENOTSUP;
}

static int impl_clear(struct spa_handle *handle) {
    struct impl *impl = (struct impl *)handle;
    if (impl->pipe) {
        rmic_pipeline_destroy(impl->pipe);
        impl->pipe = nullptr;
    }
    spa_hook_list_clean(&impl->hooks);
    return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory, const struct spa_dict *params) {
    return sizeof(struct impl);
}

static int impl_factory_init(const struct spa_handle_factory *factory,
                             struct spa_handle *handle,
                             const struct spa_dict *info,
                             const struct spa_support *support,
                             uint32_t n_support) {
    struct impl *impl;
    spa_return_val_if_fail(factory != nullptr, -EINVAL);
    spa_return_val_if_fail(handle != nullptr, -EINVAL);

    impl = (struct impl *)handle;
    memset(impl, 0, sizeof(*impl));

    impl->handle.version = SPA_VERSION_HANDLE;
    impl->handle.get_interface = impl_get_interface;
    impl->handle.clear = impl_clear;

    impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
    spa_hook_list_init(&impl->hooks);

    rmic_config_default(&impl->cfg);

    impl->aec.iface = SPA_INTERFACE_INIT(
        SPA_TYPE_INTERFACE_AUDIO_AEC,
        SPA_VERSION_AUDIO_AEC,
        &impl_aec_methods, impl);
    impl->aec.name = "realtek";
    impl->aec.info = nullptr;
    impl->aec.latency = nullptr;

    return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
    { SPA_TYPE_INTERFACE_AUDIO_AEC },
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
                                    const struct spa_interface_info **info,
                                    uint32_t *index) {
    spa_return_val_if_fail(factory != nullptr, -EINVAL);
    spa_return_val_if_fail(info != nullptr, -EINVAL);
    spa_return_val_if_fail(index != nullptr, -EINVAL);

    if (*index >= SPA_N_ELEMENTS(impl_interfaces))
        return 0;
    *info = &impl_interfaces[(*index)++];
    return 1;
}

static const struct spa_dict_item impl_factory_info_items[] = {
    { SPA_KEY_FACTORY_AUTHOR, "realtek-mic-effects" },
    { SPA_KEY_FACTORY_DESCRIPTION, "Clean-room Realtek-style PBFDAF AEC + OM-LSA Noise Suppression" },
};

static const struct spa_dict impl_factory_info = SPA_DICT_INIT_ARRAY(impl_factory_info_items);

static const struct spa_handle_factory spa_aec_realtek_factory = {
    SPA_VERSION_HANDLE_FACTORY,
    .name = SPA_NAME_AEC,
    .info = &impl_factory_info,
    .get_size = impl_get_size,
    .init = impl_factory_init,
    .enum_interface_info = impl_enum_interface_info,
};

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index) {
    spa_return_val_if_fail(factory != nullptr, -EINVAL);
    spa_return_val_if_fail(index != nullptr, -EINVAL);

    switch (*index) {
    case 0:
        *factory = &spa_aec_realtek_factory;
        break;
    default:
        return 0;
    }
    (*index)++;
    return 1;
}
