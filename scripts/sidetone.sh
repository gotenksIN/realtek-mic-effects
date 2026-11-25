#!/usr/bin/env bash
# Toggle permanent live monitoring (sidetone) of the Realtek Mic Effects source.
set -euo pipefail

UNIT="realtek-mic-sidetone.service"
CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
ENV_DIR="${CONFIG_HOME}/realtek-mic-effects"
USER_ENV_FILE="${ENV_DIR}/sidetone.env"
SINK_ENV_FILE="${ENV_DIR}/sidetone-sink.env"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--sink <name>|--sink default] [on|off|toggle|status]

  on       Start sidetone now and at every login.
  off      Stop sidetone and disable it at login.
  toggle   Switch between on and off.
  status   Show whether sidetone is on and which sink it uses.

  --sink <name>    Play sidetone to this sink (see: pactl list short sinks).
  --sink default   Clear the sink pin. A -P in SIDETONE_ARGS (sidetone.env) still applies.
USAGE
}

is_active() {
    systemctl --user is-active --quiet "${UNIT}"
}

# The script owns sidetone-sink.env and never edits sidetone.env, which holds
# the user's own SIDETONE_ARGS.
write_sink() {
    local sink="$1"
    if [[ "${sink}" == "default" ]]; then
        rm -f "${SINK_ENV_FILE}"
        echo "Cleared the managed sink pin."
        echo "Sidetone uses the default sink unless SIDETONE_ARGS in ${USER_ENV_FILE} sets -P."
    else
        if [[ ! "${sink}" =~ ^[A-Za-z0-9._:-]+$ ]]; then
            echo "Invalid sink name: ${sink}" >&2
            exit 2
        fi
        mkdir -p "${ENV_DIR}"
        local tmp
        tmp="$(mktemp "${SINK_ENV_FILE}.XXXXXX")"
        printf 'SIDETONE_SINK_ARGS="-P %s"\n' "${sink}" > "${tmp}"
        mv "${tmp}" "${SINK_ENV_FILE}"
        echo "Sidetone uses sink ${sink}."
    fi
}

# Print the value of the last VAR= line in an env file, stripping matching outer quotes.
env_value() {
    local file="$1" var="$2" line val
    [[ -r "${file}" ]] || return 0
    line="$(grep "^${var}=" "${file}" | tail -n 1)" || return 0
    val="${line#"${var}"=}"
    if [[ ( "${val}" == \"*\" && "${val}" == *\" ) || ( "${val}" == \'*\' && "${val}" == *\' ) ]]; then
        val="${val:1:${#val}-2}"
    fi
    printf '%s' "${val}"
}

show_status() {
    local enabled active
    enabled="$(systemctl --user is-enabled "${UNIT}" 2>/dev/null || true)"
    active="$(systemctl --user is-active "${UNIT}" 2>/dev/null || true)"
    echo "Sidetone: ${active:-unknown} (${enabled:-not installed} at login)"
    local managed user
    managed="$(env_value "${SINK_ENV_FILE}" SIDETONE_SINK_ARGS)"
    user="$(env_value "${USER_ENV_FILE}" SIDETONE_ARGS)"
    if [[ -n "${managed}" ]]; then
        echo "Sink: ${managed#-P } (pinned with --sink)"
    else
        echo "Sink: not pinned with --sink"
    fi
    if [[ -n "${user}" ]]; then
        echo "SIDETONE_ARGS: ${user} (from ${USER_ENV_FILE})"
    fi
}

sink=""
command=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sink)
            [[ $# -ge 2 && -n "$2" ]] || { usage >&2; exit 2; }
            sink="$2"
            shift 2
            ;;
        --sink=*)
            sink="${1#--sink=}"
            [[ -n "${sink}" ]] || { usage >&2; exit 2; }
            shift
            ;;
        on|off|toggle|status)
            if [[ -n "${command}" ]]; then
                echo "Specify at most one command (got '${command}' and '$1')." >&2
                usage >&2
                exit 2
            fi
            command="$1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${sink}" && -z "${command}" ]]; then
    usage >&2
    exit 2
fi

if ! systemctl --user show-environment >/dev/null 2>&1; then
    echo "No systemd user session found." >&2
    exit 1
fi

if ! systemctl --user cat "${UNIT}" >/dev/null 2>&1; then
    echo "${UNIT} is not installed. Run scripts/install.sh first." >&2
    exit 1
fi

sink_updated=false
if [[ -n "${sink}" ]]; then
    write_sink "${sink}"
    sink_updated=true
fi

if [[ "${command}" == "toggle" ]]; then
    if is_active; then command="off"; else command="on"; fi
fi

case "${command}" in
    on)
        if "${sink_updated}" && is_active; then
            systemctl --user enable "${UNIT}"
            systemctl --user restart "${UNIT}"
        else
            systemctl --user enable --now "${UNIT}"
        fi
        echo "Sidetone is on."
        ;;
    off)
        systemctl --user disable --now "${UNIT}"
        echo "Sidetone is off."
        ;;
    status)
        if "${sink_updated}" && is_active; then
            systemctl --user restart "${UNIT}"
        fi
        show_status
        ;;
    "")
        if "${sink_updated}" && is_active; then
            systemctl --user restart "${UNIT}"
        fi
        ;;
esac
