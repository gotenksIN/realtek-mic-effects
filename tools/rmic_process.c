#include "rmicdsp.h"

#include <fcntl.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s --mic <mic.wav> --out <out.wav> [--ref <ref.wav>] [--no-aec] [--no-ns] [--no-hpf] [--floor-db -18] [--gain-db 0]\n",
            prog);
}

static bool parse_float_arg(const char *s, float *out) {
    char *end = nullptr;
    const float v = strtof(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool same_inode(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static SNDFILE *open_input_wav(const char *path, SF_INFO *info, struct stat *st_out) {
    const bool is_stdin = (strcmp(path, "-") == 0);
    const int fd = is_stdin ? STDIN_FILENO : open(path, O_RDONLY);
    if (fd < 0) return nullptr;
    if (fstat(fd, st_out) != 0) {
        if (!is_stdin) close(fd);
        return nullptr;
    }
    // Passing SF_TRUE transfers descriptor ownership to libsndfile, which also
    // closes fd if sf_open_fd fails.
    return sf_open_fd(fd, SFM_READ, info, is_stdin ? SF_FALSE : SF_TRUE);
}

static SNDFILE *open_output_wav(const char *path,
                                SF_INFO *info,
                                const struct stat *mic_st,
                                const struct stat *ref_st,
                                bool *overwrite_rejected) {
    *overwrite_rejected = false;
    const bool is_stdout = (strcmp(path, "-") == 0);
    const int fd = is_stdout ? STDOUT_FILENO : open(path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return nullptr;

    struct stat out_st = {};
    if (fstat(fd, &out_st) != 0) {
        if (!is_stdout) close(fd);
        return nullptr;
    }
    if (same_inode(mic_st, &out_st) || (ref_st && same_inode(ref_st, &out_st))) {
        *overwrite_rejected = true;
        if (!is_stdout) close(fd);
        return nullptr;
    }
    if (!is_stdout && S_ISREG(out_st.st_mode) && ftruncate(fd, 0) != 0) {
        close(fd);
        return nullptr;
    }

    // Passing SF_TRUE transfers descriptor ownership to libsndfile, which also
    // closes fd if sf_open_fd fails.
    return sf_open_fd(fd, SFM_WRITE, info, is_stdout ? SF_FALSE : SF_TRUE);
}

static bool write_trimmed(SNDFILE *out_sf, const float *buf, sf_count_t n, uint32_t *skip) {
    sf_count_t offset = 0;
    if (*skip > 0) {
        const sf_count_t drop = ((sf_count_t)*skip < n) ? (sf_count_t)*skip : n;
        offset = drop;
        *skip -= (uint32_t)drop;
    }
    const sf_count_t to_write = n - offset;
    if (to_write <= 0) return true;
    return sf_writef_float(out_sf, buf + offset, to_write) == to_write;
}

int main(int argc, char **argv) {
    const char *mic_path = nullptr;
    const char *ref_path = nullptr;
    const char *out_path = nullptr;

    rmic_config_t cfg;
    rmic_config_default(&cfg);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mic") == 0 && i + 1 < argc) {
            mic_path = argv[++i];
        } else if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) {
            ref_path = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--no-aec") == 0) {
            cfg.enable_aec = false;
        } else if (strcmp(argv[i], "--no-ns") == 0) {
            cfg.enable_ns = false;
        } else if (strcmp(argv[i], "--no-hpf") == 0) {
            cfg.enable_hpf = false;
        } else if (strcmp(argv[i], "--floor-db") == 0 && i + 1 < argc) {
            if (!parse_float_arg(argv[++i], &cfg.ns_floor_db)) {
                fprintf(stderr, "Invalid --floor-db value: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--gain-db") == 0 && i + 1 < argc) {
            if (!parse_float_arg(argv[++i], &cfg.output_gain_db)) {
                fprintf(stderr, "Invalid --gain-db value: %s\n", argv[i]);
                return 1;
            }
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!mic_path || !out_path) {
        print_usage(argv[0]);
        return 1;
    }

    SF_INFO mic_info = {};
    struct stat mic_st = {};
    SNDFILE *mic_sf = open_input_wav(mic_path, &mic_info, &mic_st);
    if (!mic_sf) {
        fprintf(stderr, "Failed to open mic wav: %s\n", mic_path);
        return 1;
    }
    if (mic_info.channels <= 0 || mic_info.samplerate <= 0) {
        fprintf(stderr, "Invalid mic wav format: channels=%d samplerate=%d\n",
                mic_info.channels, mic_info.samplerate);
        sf_close(mic_sf);
        return 1;
    }

    SF_INFO ref_info = {};
    struct stat ref_st = {};
    SNDFILE *ref_sf = nullptr;
    if (ref_path) {
        ref_sf = open_input_wav(ref_path, &ref_info, &ref_st);
        if (!ref_sf) {
            fprintf(stderr, "Failed to open ref wav: %s\n", ref_path);
            sf_close(mic_sf);
            return 1;
        }
        if (ref_info.channels <= 0 || ref_info.samplerate <= 0) {
            fprintf(stderr, "Invalid ref wav format: channels=%d samplerate=%d\n",
                    ref_info.channels, ref_info.samplerate);
            sf_close(mic_sf);
            sf_close(ref_sf);
            return 1;
        }
        if (ref_info.samplerate != mic_info.samplerate) {
            fprintf(stderr, "Sample rate mismatch: mic=%d ref=%d\n", mic_info.samplerate, ref_info.samplerate);
            sf_close(mic_sf);
            sf_close(ref_sf);
            return 1;
        }
        cfg.ref_channels = ref_info.channels >= 2 ? 2u : 1u;
    } else {
        cfg.enable_aec = false;
        cfg.enable_delay_est = false;
    }

    cfg.sample_rate = (uint32_t)mic_info.samplerate;

    SF_INFO out_info = mic_info;
    out_info.channels = 1;
    bool overwrite_rejected = false;
    SNDFILE *out_sf = open_output_wav(out_path, &out_info, &mic_st,
                                      ref_sf ? &ref_st : nullptr, &overwrite_rejected);
    if (!out_sf) {
        if (overwrite_rejected) {
            fprintf(stderr, "Output path must not overwrite an input file: %s\n", out_path);
        } else {
            fprintf(stderr, "Failed to open output wav: %s\n", out_path);
        }
        sf_close(mic_sf);
        if (ref_sf) sf_close(ref_sf);
        return 1;
    }

    rmic_pipeline_t *pipe = rmic_pipeline_create(&cfg);
    if (!pipe) {
        fprintf(stderr, "Failed to create DSP pipeline\n");
        sf_close(mic_sf);
        if (ref_sf) sf_close(ref_sf);
        sf_close(out_sf);
        return 1;
    }

    constexpr sf_count_t BLOCK = 480;
    float *mic_interleaved = calloc((size_t)BLOCK * (size_t)mic_info.channels, sizeof(float));
    float *ref_interleaved = ref_sf ? calloc((size_t)BLOCK * (size_t)ref_info.channels, sizeof(float)) : nullptr;
    if (!mic_interleaved || (ref_sf && !ref_interleaved)) {
        fprintf(stderr, "Out of memory\n");
        rmic_pipeline_destroy(pipe);
        free(mic_interleaved);
        free(ref_interleaved);
        sf_close(mic_sf);
        if (ref_sf) sf_close(ref_sf);
        sf_close(out_sf);
        return 1;
    }
    float mic_mono[BLOCK];
    float ref_ch0[BLOCK];
    float ref_ch1[BLOCK];
    float out_mono[BLOCK];

    const uint32_t latency = rmic_pipeline_latency_samples(pipe);
    uint32_t skip = latency;
    int exit_code = 0;

    for (;;) {
        sf_count_t n = sf_readf_float(mic_sf, mic_interleaved, BLOCK);
        if (n < BLOCK && sf_error(mic_sf) != SF_ERR_NO_ERROR) {
            fprintf(stderr, "Failed to read mic wav: %s\n", sf_strerror(mic_sf));
            exit_code = 1;
            break;
        }
        if (n <= 0) break;

        for (sf_count_t i = 0; i < n; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < mic_info.channels; ++c) {
                sum += mic_interleaved[i * mic_info.channels + c];
            }
            mic_mono[i] = sum / (float)mic_info.channels;
        }

        const float *play_ptrs[2] = {nullptr, nullptr};
        if (ref_sf) {
            sf_count_t nr = sf_readf_float(ref_sf, ref_interleaved, n);
            if (nr < n && sf_error(ref_sf) != SF_ERR_NO_ERROR) {
                fprintf(stderr, "Failed to read ref wav: %s\n", sf_strerror(ref_sf));
                exit_code = 1;
                break;
            }
            for (sf_count_t i = 0; i < n; ++i) {
                if (i < nr) {
                    ref_ch0[i] = ref_interleaved[i * ref_info.channels];
                    if (ref_info.channels >= 2) {
                        ref_ch1[i] = ref_interleaved[i * ref_info.channels + 1];
                    }
                } else {
                    ref_ch0[i] = 0.0f;
                    ref_ch1[i] = 0.0f;
                }
            }
            play_ptrs[0] = ref_ch0;
            play_ptrs[1] = (ref_info.channels >= 2) ? ref_ch1 : nullptr;
        }

        rmic_pipeline_process(pipe, mic_mono, ref_sf ? play_ptrs : nullptr, out_mono, (uint32_t)n);
        if (!write_trimmed(out_sf, out_mono, n, &skip)) {
            fprintf(stderr, "Failed to write output wav: %s\n", out_path);
            exit_code = 1;
            break;
        }
    }

    // Drain pipeline latency with silence so the output matches the input length.
    if (exit_code == 0 && latency > 0) {
        memset(mic_mono, 0, sizeof(mic_mono));
        memset(ref_ch0, 0, sizeof(ref_ch0));
        memset(ref_ch1, 0, sizeof(ref_ch1));
        const float *zero_play[2] = {
            ref_ch0,
            (ref_sf && ref_info.channels >= 2) ? ref_ch1 : nullptr
        };
        uint32_t rem = latency;
        while (rem > 0) {
            const uint32_t n = (rem > (uint32_t)BLOCK) ? (uint32_t)BLOCK : rem;
            rmic_pipeline_process(pipe, mic_mono, ref_sf ? zero_play : nullptr, out_mono, n);
            if (!write_trimmed(out_sf, out_mono, (sf_count_t)n, &skip)) {
                fprintf(stderr, "Failed to write output wav: %s\n", out_path);
                exit_code = 1;
                break;
            }
            rem -= n;
        }
    }

    rmic_pipeline_destroy(pipe);
    free(mic_interleaved);
    free(ref_interleaved);
    sf_close(mic_sf);
    if (ref_sf) sf_close(ref_sf);
    if (sf_close(out_sf) != 0) {
        fprintf(stderr, "Failed to finalize output wav: %s\n", out_path);
        exit_code = 1;
    }
    return exit_code;
}
