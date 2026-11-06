#!/usr/bin/env bash
# Shared steps for the jobs that run on the self-hosted Linux GPU runner
# (`gpu-linux-3080`: labels self-hosted, X64, gpu, Linux, rtx3080).
#
# The runner has NO sudo by design (the repository is public: a sudo runner would
# be root for any job that slips through the fork guard), so nothing here
# installs system packages. `check-tools` instead fails closed and prints exactly
# what is missing, so the person who administers the machine can install it once.
#
#   tests/ci/linux_gpu_runner.sh check-tools [--build-only] [--need-glslang] [--need-pwsh]
#   tests/ci/linux_gpu_runner.sh venv [--with-test-data]
#   tests/ci/linux_gpu_runner.sh stage-binary <built-binary> <out-dir>
#   tests/ci/linux_gpu_runner.sh install-binary <artifact-dir>
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
#   `--build-only` drops the Vulkan-device and DISPLAY checks: a job that only
#   compiles (the CPU build jobs that hand the GPU jobs a binary) renders
#   nothing, so it must not demand a GPU and must be able to run on a runner
#   without one.
#
# `stage-binary` / `install-binary` are the two halves of the build -> GPU-test
# handoff. The build job compiles on a runner WITHOUT the `gpu` label and
# uploads the binary as an artifact; the GPU job downloads it and only runs
# tests. The handoff is pinned to the commit: `stage-binary` refuses unless the
# checkout it built is exactly $GITHUB_SHA, and writes build-provenance.txt
# (commit, run id, binary name, sha256) next to the copy. `install-binary`
# refuses unless that provenance names this job's own $GITHUB_SHA and
# $GITHUB_RUN_ID, this job's checkout is that same commit, the artifact holds
# nothing else, and the sha256 matches; only then does it place the binary at
# bin/<name>, where the jobs' unchanged "Resolve Godot binary" step finds it.
# Every mismatch fails closed: a binary built from another commit is never
# tested as this one.
#
# `venv` creates a per-job virtualenv from the version-and-hash-locked
# tests/ci/requirements-scons.txt and prepends its bin/ to PATH for every later
# step through GITHUB_PATH, so a later `python -m SCons` runs inside it (GAP-403).
# `--with-test-data` also installs the hash-locked tests/ci/requirements-test-data.txt
# (numpy) into the same venv, for jobs that run tests/ci/run_module_tests.py: its
# required guard phase runs the S1b quality harness unit tests, which import numpy.
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
  local need_glslang=0 need_pwsh=0 build_only=0 arg
  for arg in "$@"; do
    case "$arg" in
      --build-only) build_only=1 ;;
      --need-glslang) need_glslang=1 ;;
      --need-pwsh) need_pwsh=1 ;;
      *) echo "check-tools: unknown argument $arg" >&2; return 2 ;;
    esac
  done

  local -a missing=()
  local py
  if py="$(resolve_python311)"; then
    echo "ok       python (>= 3.11): $py ($("$py" --version 2>&1))"
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

  if [ "$build_only" = 1 ]; then
    echo "skipped  vulkaninfo and DISPLAY (--build-only: this job compiles and renders nothing)"
  elif ! command -v vulkaninfo >/dev/null 2>&1; then
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

  if [ "$build_only" = 1 ]; then
    :
  elif [ -z "${DISPLAY:-}" ]; then
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
  local with_test_data=0 arg
  for arg in "$@"; do
    case "$arg" in
      --with-test-data) with_test_data=1 ;;
      *) echo "venv: unknown argument $arg" >&2; return 2 ;;
    esac
  done
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
  if [ "$with_test_data" = 1 ]; then
    python -m pip install --require-hashes -r tests/ci/requirements-test-data.txt \
      || { echo "::error::numpy install (requirements-test-data.txt) failed"; return 1; }
    python -c 'import numpy; print("numpy", numpy.__version__)' \
      || { echo "::error::numpy is not importable from the venv"; return 1; }
  fi
  if [ -n "${GITHUB_PATH:-}" ]; then
    echo "$venv/bin" >> "$GITHUB_PATH"
  fi
  python --version
  python -m SCons --version | head -3
}

# The only file names the jobs' "Resolve Godot binary" step accepts.
HANDOFF_BINARY_NAMES=(godot.linuxbsd.editor.dev.x86_64 godot.linuxbsd.editor.x86_64)
HANDOFF_PROVENANCE=build-provenance.txt

is_handoff_binary_name() {
  local name="$1" allowed
  for allowed in "${HANDOFF_BINARY_NAMES[@]}"; do
    [ "$name" = "$allowed" ] && return 0
  done
  return 1
}

# Value of KEY in a provenance file; fails unless KEY occurs exactly once.
provenance_value() {
  local file="$1" key="$2" count
  count="$(grep -c "^${key}=" "$file" || true)"
  if [ "$count" != 1 ]; then
    echo "::error::$file has $count '${key}=' lines; expected exactly 1" >&2
    return 1
  fi
  sed -n "s/^${key}=//p" "$file"
}

stage_binary() {
  if [ "$#" -ne 2 ] || [ -z "$1" ] || [ -z "$2" ]; then
    echo "usage: $0 stage-binary <built-binary> <out-dir>" >&2
    return 2
  fi
  local binary="$1" out="$2" name head digest
  : "${GITHUB_SHA:?GITHUB_SHA is not set; this runs inside a GitHub Actions job}"
  : "${GITHUB_RUN_ID:?GITHUB_RUN_ID is not set; this runs inside a GitHub Actions job}"
  if [ ! -f "$binary" ] || [ ! -x "$binary" ]; then
    echo "::error::stage-binary: $binary is not an executable file"
    return 1
  fi
  name="$(basename "$binary")"
  if ! is_handoff_binary_name "$name"; then
    echo "::error::stage-binary: unexpected binary name '$name' (allowed: ${HANDOFF_BINARY_NAMES[*]})"
    return 1
  fi
  head="$(git rev-parse HEAD)" || { echo "::error::stage-binary: git rev-parse HEAD failed"; return 1; }
  if [ "$head" != "$GITHUB_SHA" ]; then
    echo "::error::stage-binary: the checkout that was built is $head, not GITHUB_SHA $GITHUB_SHA; refusing to publish it as this commit's binary"
    return 1
  fi
  if [ -e "$out" ] && [ -n "$(ls -A "$out")" ]; then
    echo "::error::stage-binary: $out already exists and is not empty"
    return 1
  fi
  mkdir -p "$out" || return 1
  cp -- "$binary" "$out/$name" || { echo "::error::stage-binary: copy failed"; return 1; }
  digest="$(sha256sum "$out/$name" | cut -d' ' -f1)" || return 1
  {
    echo "commit=$head"
    echo "run_id=$GITHUB_RUN_ID"
    echo "binary=$name"
    echo "sha256=$digest"
  } > "$out/$HANDOFF_PROVENANCE" || return 1
  echo "Staged $name for commit $head:"
  cat "$out/$HANDOFF_PROVENANCE"
}

install_binary() {
  if [ "$#" -ne 1 ] || [ -z "$1" ]; then
    echo "usage: $0 install-binary <artifact-dir>" >&2
    return 2
  fi
  local dir="$1" provenance commit run_id name digest head actual dest existing listing expected
  : "${GITHUB_SHA:?GITHUB_SHA is not set; this runs inside a GitHub Actions job}"
  : "${GITHUB_RUN_ID:?GITHUB_RUN_ID is not set; this runs inside a GitHub Actions job}"
  provenance="$dir/$HANDOFF_PROVENANCE"
  if [ ! -f "$provenance" ]; then
    echo "::error::install-binary: $provenance is missing; the artifact carries no build provenance"
    return 1
  fi
  commit="$(provenance_value "$provenance" commit)" || return 1
  run_id="$(provenance_value "$provenance" run_id)" || return 1
  name="$(provenance_value "$provenance" binary)" || return 1
  digest="$(provenance_value "$provenance" sha256)" || return 1
  if [ "$commit" != "$GITHUB_SHA" ]; then
    echo "::error::install-binary: the artifact was built from $commit, but this job tests GITHUB_SHA $GITHUB_SHA"
    return 1
  fi
  if [ "$run_id" != "$GITHUB_RUN_ID" ]; then
    echo "::error::install-binary: the artifact comes from run $run_id, not this run $GITHUB_RUN_ID"
    return 1
  fi
  head="$(git rev-parse HEAD)" || { echo "::error::install-binary: git rev-parse HEAD failed"; return 1; }
  if [ "$head" != "$commit" ]; then
    echo "::error::install-binary: this job's checkout is $head but the binary was built from $commit"
    return 1
  fi
  if ! is_handoff_binary_name "$name"; then
    echo "::error::install-binary: unexpected binary name '$name' (allowed: ${HANDOFF_BINARY_NAMES[*]})"
    return 1
  fi
  listing="$(ls -A "$dir" | LC_ALL=C sort | tr '\n' ' ')"
  expected="$(printf '%s\n' "$HANDOFF_PROVENANCE" "$name" | LC_ALL=C sort | tr '\n' ' ')"
  if [ "$listing" != "$expected" ]; then
    echo "::error::install-binary: $dir must hold exactly '$expected'; found '$listing'"
    return 1
  fi
  actual="$(sha256sum "$dir/$name" | cut -d' ' -f1)" || return 1
  if [ "$actual" != "$digest" ]; then
    echo "::error::install-binary: sha256 of $name is $actual, provenance says $digest"
    return 1
  fi
  # A binary already in bin/ is what the resolver would pick up. The checkout's
  # clean removes it, so its presence is a fault, never something to overwrite.
  for existing in "${HANDOFF_BINARY_NAMES[@]}"; do
    if [ -e "bin/$existing" ]; then
      echo "::error::install-binary: bin/$existing already exists before install; refusing to test a binary of unknown origin"
      return 1
    fi
  done
  dest="bin/$name"
  mkdir -p bin || return 1
  cp -- "$dir/$name" "$dest" || { echo "::error::install-binary: copy failed"; return 1; }
  chmod 0755 "$dest" || return 1
  actual="$(sha256sum "$dest" | cut -d' ' -f1)" || return 1
  if [ "$actual" != "$digest" ]; then
    echo "::error::install-binary: installed $dest hashes to $actual, expected $digest"
    return 1
  fi
  echo "Installed $dest, built from commit $commit in run $run_id (sha256 $digest)"
}

cmd="${1:-}"
shift || true
case "$cmd" in
  check-tools) check_tools "$@" ;;
  venv) make_venv "$@" ;;
  stage-binary) stage_binary "$@" ;;
  install-binary) install_binary "$@" ;;
  *) echo "usage: $0 {check-tools [--build-only] [--need-glslang] [--need-pwsh] | venv [--with-test-data] | stage-binary <built-binary> <out-dir> | install-binary <artifact-dir>}" >&2; exit 2 ;;
esac
