# Project decisions

Durable architectural and toolchain decisions for `realtek-mic-effects`.

## Clean-room classic DSP

Inspecting the Windows Realtek driver (`RltkAPOU64.dll` 11.0.6000.957, `HDX_AsrockExt_RTK.inf`) confirmed the rear-mic pipeline uses classic signal processing only (100 Hz HPF, delay estimator, frequency-domain AEC, OM-LSA post-filter), with no neural-network weights.

- **Decision:** Reimplement the same algorithm families from published DSP literature; never copy, link, or translate Realtek code or binaries.
- **Why:** Avoids proprietary licensing issues while keeping CPU use low, latency deterministic, and deployment free of model files.

## Native 44.1 / 48 kHz processing

- **Decision:** Process at the native PipeWire rate (44.1 kHz or 48 kHz) instead of resampling to 16 kHz like the Windows Realtek AEC path.
- **Why:** Preserves full 20–24 kHz voice bandwidth and removes a resampler stage.

## Dual plugin packaging over one C API

- **Decision:** Ship the full HPF + AEC + NS pipeline as a PipeWire SPA `audio.aec` plugin and standalone HPF + NS as an LV2 plugin, both calling a single static C++ DSP library (`rmicdsp`) through `include/rmicdsp.h`.
- **Why:** `libpipewire-module-echo-cancel` supplies synchronized capture and playback reference streams to `audio.aec` plugins via one drop-in file. Noise suppression needs no playback reference, so LV2 makes it usable in `filter-chain` and other hosts without duplicating DSP code.

## Two-path PBFDAF AEC with GCC-PHAT and Bark OM-LSA

- **Decision:** Use a GCC-PHAT bulk delay estimator, a two-path (main + shadow) partitioned block frequency-domain adaptive filter (PBFDAF), and a joint Bark-band OM-LSA post-filter.
- **Why:** Two-path adaptation avoids the single-path trade-off between fast convergence and double-talk stability. PBFDAF supports long echo tails at low hop latency and low CPU cost. Bulk delay alignment keeps the adaptive tail focused on the acoustic path, and feeding the AEC residual-echo estimate into OM-LSA suppresses stationary noise and residual echo together.

## Single toolchain and modern standards

- **Decision:** Require Clang 23, LLD 23, LLVM 23 archive tools (`llvm-ar-23`, `llvm-ranlib-23`), CMake 3.31+, and PipeWire 1.6+. Compile wrappers and CLI as strict C23 and the DSP core as strict C++26 (`CMAKE_*_EXTENSIONS OFF`). Implement only `spa_audio_aec_methods::init2` (no legacy `init`, no `aec.method` factory key).
- **Why:** One compiler, linker, and host ABI eliminates compatibility shims and gives predictable code generation and diagnostics.

## Native optimization, ThinLTO, and hardening

- **Decision:** Release builds enable `-march=native` (`RMIC_NATIVE_ARCH`, default `ON`), `-ffp-contract=fast`, `-fno-math-errno`, `-fno-trapping-math`, ThinLTO (`-flto=thin`), Clang CFI without `cfi-icall`, and `_FORTIFY_SOURCE=3`. All builds enable CET (`-fcf-protection=full`), zeroed stack locals, `-fstack-protector-strong`, full RELRO (`-z relro -z now`), and a non-executable stack. `-ffast-math` is prohibited.
- **Why:** Target binaries run on the build host (x86-64 with AVX2/FMA); safe FP flags allow FMA and vectorization without `-ffast-math` reassociation or finite-math assumptions that alter DSP output. Plugins run inside long-lived audio servers, so ThinLTO + CFI + CET/RELRO/fortification limit memory-safety impact. `cfi-icall` stays disabled because plugins invoke host function pointers (e.g., `spa_log`) compiled without CFI metadata.

## Minimal symbol surface

- **Decision:** Build all targets with hidden visibility and link each shared plugin with a version script (`*.map`) plus `-Wl,-z,start-stop-visibility=hidden` on the SPA plugin.
- **Why:** Prevents internal symbols, libstdc++ template instantiations, and linker-generated `spa_log_topic` section symbols from clashing inside PipeWire or LV2 hosts.

## Sidetone via `pw-loopback` service

- **Decision:** Implement permanent live monitoring as a `pw-loopback` systemd user service managed by `scripts/sidetone.sh`, outside the SPA plugin.
- **Why:** Keeps the SPA plugin a pure echo canceller, reuses PipeWire's own loopback tool, persists across logins, and supports desktop shortcut toggling.

## Verification without an automated test suite

- **Decision:** Do not maintain an automated test suite.
- **Why:** The WSL development environment lacks audio hardware and a running PipeWire session; verification relies on warning-free Release builds, exported-symbol and hardening checks, and offline WAV runs with `rmic-process`.
