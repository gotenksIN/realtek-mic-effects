# Agent instructions

Read `CONTEXT.md` before changing DSP design, plugin structure, or toolchain settings.

## Build and toolchain invariants

Configure and build with the `release` preset (`build/` is git-ignored):

```bash
cmake --preset release
cmake --build --preset release -j"$(nproc)"
```

- **Toolchain:** Clang 23 (`clang-23`, `clang++-23`), LLD 23, `llvm-ar-23`, `llvm-ranlib-23`, CMake 3.31+, and PipeWire 1.6+ (`libpipewire-0.3>=1.6`). `CMakeLists.txt` rejects other toolchains; do not add compatibility shims for older compilers, C/C++ standards, or PipeWire/SPA versions.
- **Language modes:** Strict C23 for wrappers and CLI; strict C++26 for `src/dsp/` (`CMAKE_*_EXTENSIONS OFF`). Keep all targets warning-free (`-Wall -Wextra`, plus `-Wpedantic` on `rmicdsp`).
- **Floating point:** Keep `-ffp-contract=fast -fno-math-errno -fno-trapping-math` and `RMIC_NATIVE_ARCH` (`-march=native`, default `ON`). Never add `-ffast-math` (breaks DSP numerical behavior and `std::isfinite` guards).
- **Hardening and LTO:** Keep ThinLTO (`-flto=thin`), `-fsanitize=cfi -fno-sanitize=cfi-icall`, `-fcf-protection=full`, `-ftrivial-auto-var-init=zero`, `-fstack-protector-strong`, `_FORTIFY_SOURCE=3` (Release), full RELRO, and non-executable stack.
  - Never enable `cfi-icall` (calls into non-CFI host function pointers like `spa_log` trap).
  - Do not link non-LTO objects, expose C++ classes with default visibility, or add assembly/prebuilt objects lacking CET (IBT + SHSTK) notes.
- **Exports:** All targets build with hidden visibility. Each plugin uses a linker version script (`*.map`), and `spa-aec-realtek` also passes `-Wl,-z,start-stop-visibility=hidden` to hide LLD's `__start_`/`__stop_spa_log_topic` symbols. To add an entry point, mark it with `SPA_EXPORT` or `LV2_SYMBOL_EXPORT` and add it to the `.map` file.

## Verification

Development runs in WSL without audio devices or a PipeWire session.
Do not load plugins into PipeWire/LV2 hosts, capture/play audio, or install/start the sidetone service.
Do not add tests, `enable_testing()`, or `ctest` unless explicitly asked.

Run only these checks:

1. Clean Release build with zero warnings:

   ```bash
   rm -rf build && cmake --preset release && cmake --build --preset release -j"$(nproc)"
   ```

2. Exported dynamic symbols:
   - `nm -D --defined-only build/libspa-aec-realtek.so` → only `spa_handle_factory_enum` and `spa_log_topic_enum`.
   - `nm -D --defined-only build/rmic_ns.lv2/rmic_ns.so` → only `lv2_descriptor`.
3. CLI check: `./build/rmic-process` with no arguments prints usage and exits with code 1. Use it on WAV inputs for offline DSP checks when available.
4. Hardening on `build/libspa-aec-realtek.so`, `build/rmic_ns.lv2/rmic_ns.so`, and `build/rmic-process`:
   - `readelf -n <file>` shows `x86 feature: IBT, SHSTK`.
   - `readelf -d <file>` shows `BIND_NOW`.
   - `readelf -lW <file>` shows `GNU_RELRO` and `GNU_STACK` without `E`.
5. When editing shell scripts: `shellcheck scripts/*.sh`.
6. When editing `config/realtek-mic-sidetone.service`: `systemd-analyze --user verify` (or `systemd-analyze verify` if no user bus exists) on a copy.

## Layout

- `include/rmicdsp.h`: sole C interface between wrappers and the DSP core.
- `src/dsp/`: C++26 DSP core (`rmicdsp` static PIC library).
- `src/spa/aec-realtek.c`, `src/spa/aec-realtek.map`: PipeWire SPA `audio.aec` plugin.
- `src/lv2/rmic_ns_lv2.c`, `src/lv2/rmic_ns.map`, `lv2/rmic_ns.lv2/`: LV2 noise suppression plugin.
- `tools/rmic_process.c`: offline WAV processor.
- `config/60-realtek-mic-effects.conf`: PipeWire `module-echo-cancel` drop-in.
- `config/realtek-mic-sidetone.service`: systemd user service for `pw-loopback` sidetone.
- `scripts/install.sh`, `scripts/sidetone.sh`: build/install and sidetone control scripts.

## Real-time safety and DSP hot loops (`src/dsp/`)

`rmic_pipeline_process`, `rmic_pipeline_set_params`, and everything they call run on the audio thread:

- Never allocate/free memory, lock mutexes, perform blocking I/O, log, throw exceptions, resize buffers, or create/destroy FFTW plans.
- Preallocate all buffers and FFTW plans in `rmic_pipeline_create` (enabling FFTW's process-wide planner lock via `ensure_fftw_planner_thread_safe()` during initialization), and keep processing bounded in time for any `n_samples`.
- Wrap FFTW buffers with `assume_fftw_aligned` (`src/dsp/fftw_util.hpp`, 16-byte minimum alignment on x86-64).
- Scope `__restrict__` pointers to inner blocks that do not call FFTW or alias the same buffer through another pointer.
- Use `[[assume(...)]]` only on local variables, never on expressions containing function calls.
- Keep offline `rmic-process` output within rounding error of the previous build on identical inputs.

## Plugin wrappers (`src/spa/`, `src/lv2/`, `tools/`)

- Keep all wrappers and the CLI in strict C23 (`nullptr`, built-in `bool`, `constexpr`, `{}` initializers); do not convert them to C++.
- Access the DSP core only through `include/rmicdsp.h`.
- Implement only `init2` in `spa_audio_aec_methods` (`SPA_VERSION_AUDIO_AEC_METHODS`); never add legacy `init`.
- Keep `_POSIX_C_SOURCE=200809L` on `spa-aec-realtek` so strict C23 exposes the POSIX locale API used by `<spa/utils/string.h>`.
- Register SPA log topics with `SPA_LOG_TOPIC_DEFINE_STATIC` and export them with `SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED`.
