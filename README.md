# Realtek mic effects for PipeWire

A clean-room Linux port of the Realtek Audio Console microphone effects for PipeWire, tuned for rear-panel analog microphones (`ALC892`) at native 44.1 kHz or 48 kHz.

Components:

- **PipeWire SPA plugin** (`libspa-aec-realtek.so`): 100 Hz high-pass filter, GCC-PHAT delay estimator, two-path PBFDAF echo canceller (AEC), and Bark-band OM-LSA noise/residual-echo suppressor.
- **LV2 plugin** (`rmic_ns.lv2`): standalone OM-LSA noise suppressor with a switchable 100 Hz high-pass filter, without AEC.
- **Offline WAV CLI** (`rmic-process`): processes WAV files through the same DSP pipeline.
- **Sidetone service** (`realtek-mic-sidetone.service` + `scripts/sidetone.sh`): toggleable live mic monitoring, equivalent to Windows **Listen to this device**.

## Prerequisites

- Clang 23 (`clang-23`, `clang++-23`), LLD 23 (`ld.lld-23`), and LLVM 23 tools (`llvm-ar-23`, `llvm-ranlib-23`)
- CMake 3.31+
- PipeWire 1.6+ with development headers

Debian / Ubuntu:

```bash
sudo apt install clang-23 lld-23 llvm-23 cmake pkgconf libfftw3-dev libsndfile1-dev \
    libpipewire-0.3-dev libspa-0.2-dev lv2-dev pipewire-bin alsa-utils \
    pulseaudio-utils
```

If your distribution does not ship LLVM 23, install `clang-23`, `lld-23`, and `llvm-23` from [apt.llvm.org](https://apt.llvm.org).

Verify versions:

```bash
clang-23 --version
ld.lld-23 --version
cmake --version
pkg-config --modversion libpipewire-0.3
```

## Build

```bash
cmake --preset release
cmake --build --preset release -j"$(nproc)"
```

The `release` preset builds into `build/` using Clang 23, LLD 23, ThinLTO, CFI/CET hardening, and `-march=native`.
To build portable binaries for other x86-64 CPUs, pass `-DRMIC_NATIVE_ARCH=OFF` when configuring.
If `build/` was configured with a different toolchain, pass `--fresh` to `cmake`.

Outputs:

- `build/libspa-aec-realtek.so` — PipeWire `audio.aec` plugin
- `build/rmic_ns.lv2/` — LV2 plugin bundle
- `build/rmic-process` — offline WAV processor

## Offline WAV processing

```bash
./build/rmic-process --mic raw_mic.wav --ref speaker_loopback.wav --out clean_mic.wav
```

Omit `--ref` to run without echo cancellation.

| Option | Effect |
| --- | --- |
| `--no-aec` | Disable echo cancellation |
| `--no-ns` | Disable noise suppression |
| `--no-hpf` | Disable the 100 Hz high-pass filter |
| `--floor-db <dB>` | Set the noise suppression floor (default `-18`) |
| `--gain-db <dB>` | Set the digital output gain (default `0`) |

## Install

Run the install script to build and install the LV2 bundle, PipeWire drop-in, sidetone service, and SPA plugin:

```bash
./scripts/install.sh
```

The script installs user files into `~/.lv2/`, `${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/`, and `${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/` (preserving an existing `60-realtek-mic-effects.conf` if you modified it).
If your system SPA plugin directory requires root, run the `sudo install` command the script prints.

<details>
<summary>Manual install commands</summary>

```bash
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
sudo install -Dm755 build/libspa-aec-realtek.so \
    "$(pkg-config --variable=plugindir libspa-0.2)/aec/libspa-aec-realtek.so"
install -Dm644 config/60-realtek-mic-effects.conf \
    "${CONFIG_HOME}/pipewire/pipewire.conf.d/60-realtek-mic-effects.conf"
mkdir -p ~/.lv2 && rm -rf ~/.lv2/rmic_ns.lv2 && cp -r build/rmic_ns.lv2 ~/.lv2/
install -Dm644 config/realtek-mic-sidetone.service \
    "${CONFIG_HOME}/systemd/user/realtek-mic-sidetone.service"
systemctl --user daemon-reload
```

</details>

### Activate and configure mic boost (0–30 dB)

1. Restart PipeWire:

   ```bash
   systemctl --user restart pipewire pipewire-pulse
   ```

2. Select **Realtek Mic Effects (AEC + NS)** as your input device in desktop sound settings or your app.
3. Configure hardware rear microphone boost (`ALC892` **Rear Mic Boost** steps from `0` to `+30 dB` in `10 dB` increments; `+10 dB` matches the Realtek Audio Console default):
   1. Find your card number with `arecord -l`.
   2. Set the boost interactively or from the command line:
      - Interactively: open `alsamixer -c <card>`, press `F4` for the **Capture** view, select **Rear Mic Boost**, and adjust to `0`, `10`, `20`, or `30 dB` (shown in the top-left `dB gain` readout).
      - Command line (substitute `0dB`, `10dB`, `20dB`, or `30dB`):

        ```bash
        amixer -c <card> sset 'Rear Mic Boost' 10dB
        ```

   3. Persist the mixer state across reboots:

      ```bash
      sudo alsactl store
      ```

   PipeWire maps `100%` on the hardware mic slider to maximum ALSA gain, stacking `Capture` (`+30 dB`) on top of **Rear Mic Boost** (`+30 dB`) for `+60 dB` total and clipping the input.
   Lower the hardware mic slider in sound settings (or run `pactl set-source-volume <source> 40%`) before fine-tuning `Rear Mic Boost` and `Capture` in `alsamixer`; WirePlumber restores the PipeWire slider value on restart, which otherwise resets raw `amixer` changes back to `100%`.

4. If your system has multiple microphones (such as a webcam mic), pin the hardware mic in `capture.props` inside `${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/60-realtek-mic-effects.conf` and restart PipeWire:

   ```
   capture.props = {
       node.passive = true
       target.object = "<source_name>"   # see: pactl list short sources
   }
   ```

To tune DSP parameters, edit `aec.args` in `${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/60-realtek-mic-effects.conf` and restart PipeWire.
Use `realtek.gain_db` (`-24.0` to `+30.0 dB`, default `0.0`) in `aec.args` for fine digital makeup gain after DSP.

## Live mic monitoring (sidetone)

Sidetone routes the processed microphone signal to your headphones so you can hear your voice or check effect settings.
Always use headphones — speakers cause acoustic feedback.

For a quick one-off test (press Ctrl+C to stop):

```bash
pw-loopback -C realtek_mic_effects_source
```

### Permanent toggleable sidetone

`scripts/sidetone.sh` manages the `realtek-mic-sidetone` systemd user service, persisting across logins:

| Command | Effect |
| --- | --- |
| `./scripts/sidetone.sh on` | Start sidetone now and at login |
| `./scripts/sidetone.sh off` | Stop sidetone and disable it at login |
| `./scripts/sidetone.sh toggle` | Switch between on and off |
| `./scripts/sidetone.sh status` | Show current state and pinned sink |

Bind `<repo>/scripts/sidetone.sh toggle` to a desktop keyboard shortcut for one-key toggling.

To pin sidetone to a specific headphone sink instead of the default output:

```bash
pactl list short sinks
./scripts/sidetone.sh --sink <sink_name>
```

Clear the pin with `./scripts/sidetone.sh --sink default`.
Both loopback streams disable fallback (`node.dont-fallback=true`, `node.linger=true`), so sidetone waits if the processed source or pinned sink is absent rather than falling back to the raw mic or speakers.
For extra `pw-loopback` flags (such as `SIDETONE_ARGS="-l 20"`), create `${XDG_CONFIG_HOME:-$HOME/.config}/realtek-mic-effects/sidetone.env`; a sink pinned via `--sink` is stored separately in `sidetone-sink.env` and overrides `-P` in `SIDETONE_ARGS`.

### Sidetone and AEC interaction

The default PipeWire config sets `monitor.mode = true`, feeding the default output monitor into the AEC reference.
If sidetone also plays to that default output while `realtek.aec = true`, your own voice enters the AEC reference and gets cancelled.

Pick one of these setups when using sidetone:

- **Headphones only (simplest):** set `realtek.aec = false` in `aec.args` and restart PipeWire. Headphones do not bleed into the mic, so AEC is unnecessary.
- **Speakers + headphone sidetone (keep AEC on):**
  1. In `${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/60-realtek-mic-effects.conf`, set `monitor.mode = false` and add a dedicated reference sink next to `source.props`:

     ```
     sink.props = {
         node.name = "realtek_mic_effects_sink"
         node.description = "Realtek Mic Effects Reference"
     }
     ```

  2. Restart PipeWire and select **Realtek Mic Effects Reference** as your default playback output.
  3. Pin sidetone to your hardware headphone sink (`./scripts/sidetone.sh --sink <sink_name>`) so sidetone bypasses the AEC reference.
