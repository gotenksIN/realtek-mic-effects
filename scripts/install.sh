#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

cd "${ROOT_DIR}"
cmake --preset release
cmake --build --preset release -j"$(nproc)"

SPA_PLUGIN_DIR="$(pkg-config --variable=plugindir libspa-0.2)"
if [[ -z "${SPA_PLUGIN_DIR}" ]]; then
    echo "Failed to resolve libspa-0.2 plugindir via pkg-config." >&2
    exit 1
fi
CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
SPA_SYS_DIR="${SPA_PLUGIN_DIR}/aec"
LV2_USER_DIR="${HOME}/.lv2"
PW_CONF_DIR="${CONFIG_HOME}/pipewire/pipewire.conf.d"
PW_CONF_SRC="${ROOT_DIR}/config/60-realtek-mic-effects.conf"
PW_CONF_DST="${PW_CONF_DIR}/60-realtek-mic-effects.conf"
SYSTEMD_USER_DIR="${CONFIG_HOME}/systemd/user"

echo "Installing LV2 bundle to ${LV2_USER_DIR}/rmic_ns.lv2"
mkdir -p "${LV2_USER_DIR}"
rm -rf "${LV2_USER_DIR}/rmic_ns.lv2"
cp -r "${BUILD_DIR}/rmic_ns.lv2" "${LV2_USER_DIR}/"

if [[ ! -e "${PW_CONF_DST}" ]]; then
    echo "Installing PipeWire config to ${PW_CONF_DST}"
    install -Dm644 "${PW_CONF_SRC}" "${PW_CONF_DST}"
elif cmp -s "${PW_CONF_SRC}" "${PW_CONF_DST}"; then
    echo "PipeWire config is up to date: ${PW_CONF_DST}"
else
    echo "Preserving existing PipeWire config: ${PW_CONF_DST}"
    echo "Compare against ${PW_CONF_SRC} to pick up new defaults."
fi

echo "Installing sidetone service to ${SYSTEMD_USER_DIR}/realtek-mic-sidetone.service"
install -Dm644 "${ROOT_DIR}/config/realtek-mic-sidetone.service" \
    "${SYSTEMD_USER_DIR}/realtek-mic-sidetone.service"
if systemctl --user show-environment >/dev/null 2>&1; then
    systemctl --user daemon-reload
else
    echo "No systemd user session found. Run 'systemctl --user daemon-reload' after you log in."
fi

SPA_SRC="${BUILD_DIR}/libspa-aec-realtek.so"
SPA_DST="${SPA_SYS_DIR}/libspa-aec-realtek.so"
if [[ -w "${SPA_SYS_DIR}" || ( ! -e "${SPA_SYS_DIR}" && -w "${SPA_PLUGIN_DIR}" ) ]]; then
    install -Dm755 "${SPA_SRC}" "${SPA_DST}"
    echo "Installed SPA plugin to ${SPA_DST}"
else
    printf 'System SPA dir (%s) requires root. Run:\n' "${SPA_SYS_DIR}"
    printf '  sudo install -Dm755 %q %q\n' "${SPA_SRC}" "${SPA_DST}"
fi

echo "Done. Restart PipeWire with: systemctl --user restart pipewire pipewire-pulse"
echo "Toggle live mic monitoring with: ${ROOT_DIR}/scripts/sidetone.sh toggle"
