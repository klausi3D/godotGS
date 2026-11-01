#!/usr/bin/env bash
# Shared steps for the jobs that run on the self-hosted Linux GPU runner
# (`gpu-linux-3080`: labels self-hosted, X64, gpu, Linux, rtx3080).
#
# The runner has NO sudo by design (the repository is public: a sudo runner would
# be root for any job that slips through the fork guard), so nothing here
# installs system packages. `check-tools` instead fails closed and prints exactly
# what is missing, so the person who administers the machine can install it once.
#
#   tests/ci/linux_gpu_runner.sh check-tools [--need-glslang] [--need-pwsh]
#   tests/ci/linux_gpu_runner.sh venv
#
# `check-tools` (all findings are printed before the exit status is decided):
#   * Python 3.11 resolvable (`uv python find 3.11`, else `python3.11`, else a
#     `python3` that is itself >= 3.11, announced as such)
#   * pkg-config entries a distro-linked Godot build expects: ADVISORY only. The
#     default `use_sowrap=yes` build dlopens X11/ALSA/Pulse/udev at runtime and
#     compiles against the headers bundled in thirdparty/linuxbsd_headers, so
#     none of them is needed to build or run; a missing one is reported, not fatal
#   * `vulkaninfo --summary` shows an NVIDIA device and no software rasteriser
#   * DISPLAY is set and an X server answers on it (Godot renders on the real
#     Xorg of the NVIDIA GPU; Xvfb has no NVIDIA Vulkan)
#   * glslangValidator or glslc            (--need-glslang)
#   * PowerShell 7 (`pwsh`)                (--need-pwsh)
#
# `venv` creates a per-job virtualenv from the version-and-hash-locked
# tests/ci/requirements-scons.txt and prepends its bin/ to PATH for every later
# step through GITHUB_PATH, so a later `python -m SCons` runs inside it (GAP-403).
set -uo pipefail

REQUIRED_PKG_CONFIG=(x11 xcursor xinerama xi xrandr gl glu alsa libpulse libudev vulkan)

resolve_python311() {
  local found=""
  if command -v uv >/dev/null 2>&1; then
    found="$(uv python find 3.11 2>/dev/null || true)"
  fi
  if [ -z "$found" ] && command -v python3.11 >/dev/null 2>&1; then
    found="$(command -v python3.11)"
  fi
  if [ -z "$found" ] && command -v python3 >/dev/null 2>&1 \
     && python3 -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 11) else 1)'; then
    found="$(command -v python3)"
    echo "note: no python3.11 via uv/PATH; using $found ($("$found" --version 2>&1))" >&2
  fi
  [ -n "$found" ] && [ -x "$found" ] && printf '%s\n' "$found"
}

check_tools() {
  local need_glslang=0 need_pwsh=0 arg
  for arg in "$@"; do
    case "$arg" in
      --need-glslang) need_glslang=1 ;;
      --need-pwsh) need_pwsh=1 ;;
      *) echo "check-tools: unknown argument $arg" >&2; return 2 ;;
    esac
  done

  local -a missing=()
  local py
  if py="$(resolve_python311)"; then
    echo "ok       python 3.11: $py ($("$py" --version 2>&1))"
  else
    missing+=("Python >= 3.11 (none of 'uv python find 3.11', 'python3.11' or a python3 >= 3.11 resolved)")
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    missing+=("python3 on PATH")
  fi

  if ! command -v pkg-config >/dev/null 2>&1; then
    missing+=("pkg-config")
  else
    local pkg
    for pkg in "${REQUIRED_PKG_CONFIG[@]}"; do
      if pkg-config --exists "$pkg"; then
        echo "ok       pkg-config $pkg"
      else
        echo "advisory pkg-config $pkg is absent (not needed for the default use_sowrap=yes build)"
      fi
    done
  fi

  if [ "$need_glslang" = 1 ]; then
    if command -v glslangValidator >/dev/null 2>&1 || command -v glslc >/dev/null 2>&1; then
      echo "ok       shader compiler: $(command -v glslc || command -v glslangValidator)"
    else
      missing+=("glslangValidator or glslc (package glslang-tools)")
    fi
  fi

  if [ "$need_pwsh" = 1 ]; then
    if command -v pwsh >/dev/null 2>&1; then
      echo "ok       pwsh: $(command -v pwsh)"
    else
      missing+=("pwsh (PowerShell 7; tests/ci/collect_production_evidence.ps1 is PowerShell)")
    fi
  fi

  if ! command -v vulkaninfo >/dev/null 2>&1; then
    missing+=("vulkaninfo (package vulkan-tools)")
  else
    local summary
    summary="$(vulkaninfo --summary 2>&1 || true)"
    if printf '%s\n' "$summary" | grep -Eq 'deviceName[[:space:]]*=.*NVIDIA' \
       && ! printf '%s\n' "$summary" | grep -Eqi 'llvmpipe|lavapipe|PHYSICAL_DEVICE_TYPE_CPU'; then
      echo "ok       vulkaninfo: $(printf '%s\n' "$summary" | grep -E 'deviceName' | head -1 | sed 's/^[[:space:]]*//')"
    else
      echo "--- vulkaninfo --summary (device lines) ---"
      printf '%s\n' "$summary" | grep -E 'deviceName|deviceType|driverName' || echo "(no device lines)"
      missing+=("a Vulkan device that is an NVIDIA GPU with no llvmpipe/CPU device (check VK_ICD_FILENAMES and the driver)")
    fi
  fi

  if [ -z "${DISPLAY:-}" ]; then
    missing+=("DISPLAY (the runner .env must export DISPLAY=:0)")
  elif command -v xdpyinfo >/dev/null 2>&1; then
    if xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
      echo "ok       X server answers on $DISPLAY"
    else
      missing+=("an X server answering on DISPLAY=$DISPLAY (systemd unit xorg-headless)")
    fi
  elif [ -S "/tmp/.X11-unix/X${DISPLAY#:}" ] && pgrep -f "[X]org ${DISPLAY}( |\$)" >/dev/null 2>&1; then
    # A socket file alone survives a dead server; require a live Xorg serving it.
    echo "ok       X socket present and an Xorg process serves $DISPLAY"
  else
    missing+=("a live Xorg serving DISPLAY=$DISPLAY (socket present AND process alive; systemd unit xorg-headless)")
  fi

  if [ "${#missing[@]}" -gt 0 ]; then
    echo
    echo "::error title=Linux GPU runner is missing required tools::${#missing[@]} item(s) missing; see the list in the log"
    echo "MISSING on $(hostname):"
    local item
    for item in "${missing[@]}"; do
      echo "  - $item"
    done
    echo
    echo "This runner has no sudo, so jobs do not install packages. Install the items above"
    echo "on the runner machine (Ubuntu 24.04: sudo apt-get install build-essential pkg-config"
    echo "vulkan-tools glslang-tools; Python 3.11 via uv; PowerShell 7 only for the"
    echo "production-evidence collector) and rerun the job."
    return 1
  fi
  echo "All required tools are present."
}

make_venv() {
  local py
  py="$(resolve_python311)" || { echo "::error::Python 3.11 not found (uv python find 3.11 / python3.11)"; return 1; }
  : "${RUNNER_TEMP:?RUNNER_TEMP is not set; this runs inside a GitHub Actions job}"
  local venv="$RUNNER_TEMP/gs-ci-venv"
  "$py" -m venv --clear "$venv" || { echo "::error::venv creation failed"; return 1; }
  # shellcheck disable=SC2123
  PATH="$venv/bin:$PATH"
  if [ "$(python -c 'import sys; print(sys.prefix != sys.base_prefix)')" != "True" ]; then
    echo "::error::the venv interpreter is not first on PATH"
    return 1
  fi
  python -m pip install --require-hashes -r tests/ci/requirements-scons.txt \
    || { echo "::error::pip install failed"; return 1; }
  if [ -n "${GITHUB_PATH:-}" ]; then
    echo "$venv/bin" >> "$GITHUB_PATH"
  fi
  python --version
  python -m SCons --version | head -3
}

cmd="${1:-}"
shift || true
case "$cmd" in
  check-tools) check_tools "$@" ;;
  venv) make_venv "$@" ;;
  *) echo "usage: $0 {check-tools [--need-glslang] [--need-pwsh] | venv}" >&2; exit 2 ;;
esac
