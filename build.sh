#!/usr/bin/env bash
# build.sh — fetch FS-UAE, apply the fsuae_rpc patch, and build.
#
# Idempotent: safe to re-run.  Existing source tree is reset to the
# target tag and the patch re-applied.
#
# Defaults:
#   FSUAE_SRC=/tmp/fsuae-src   where to clone fs-uae
#   FSUAE_TAG=v3.2.35          tag to build (matches stable 3.2 release)
#   FSUAE_URL=https://github.com/FrodeSolheim/fs-uae.git
#   JOBS=<nproc>
#
# On success, prints the path of the built binary.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FSUAE_SRC="${FSUAE_SRC:-/tmp/fsuae-src}"
FSUAE_TAG="${FSUAE_TAG:-v3.2.35}"
FSUAE_URL="${FSUAE_URL:-https://github.com/FrodeSolheim/fs-uae.git}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

REQUIRED_BREW_PKGS=(
    autoconf automake libtool pkg-config gettext
    glib libpng libmpeg2 openal-soft sdl2 zlib
)

REQUIRED_APT_PKGS=(
    build-essential autoconf automake libtool pkg-config gettext
    libglib2.0-dev libpng-dev libmpeg2-4-dev libopenal-dev
    libsdl2-dev zlib1g-dev
)

step()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
fail()  { printf '\033[1;31m!! %s\033[0m\n' "$*" >&2; exit 1; }

# -- 1. OS sanity --
case "$(uname -s)" in
    Darwin) PLATFORM=macos ;;
    Linux)  PLATFORM=linux ;;
    *)      fail "Unsupported OS: $(uname -s).  Patch works wherever FS-UAE itself builds." ;;
esac

# -- 2. Install build deps --
if [[ "$PLATFORM" == "macos" ]]; then
    command -v brew >/dev/null || fail "Homebrew not installed.  See https://brew.sh"
    step "Checking Homebrew dependencies"
    MISSING=()
    for pkg in "${REQUIRED_BREW_PKGS[@]}"; do
        if brew list --formula "$pkg" >/dev/null 2>&1; then
            info "ok: $pkg"
        else
            MISSING+=("$pkg")
        fi
    done
    if (( ${#MISSING[@]} > 0 )); then
        info "Installing: ${MISSING[*]}"
        brew install "${MISSING[@]}"
    fi
    # openal-soft is keg-only — expose its pkg-config
    export PKG_CONFIG_PATH="$(brew --prefix openal-soft)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
else
    step "On Linux — assuming you've installed deps via:"
    info "sudo apt install ${REQUIRED_APT_PKGS[*]}"
    info "(or the equivalent for your distro)"
fi

# -- 3. Clone or update source --
step "Fetching FS-UAE $FSUAE_TAG into $FSUAE_SRC"
if [[ ! -d "$FSUAE_SRC/.git" ]]; then
    git clone "$FSUAE_URL" "$FSUAE_SRC"
else
    info "Reusing existing clone"
    git -C "$FSUAE_SRC" fetch --tags origin
fi

# Reset any prior patch attempts; clean -e preserves our drop-in if present
git -C "$FSUAE_SRC" reset --hard >/dev/null
git -C "$FSUAE_SRC" clean -fdx --quiet -e fsuae_rpc.cpp -e src/fsuae_rpc.cpp

# The FrodeSolheim mirror sometimes defaults to sparse-checkout on
# first clone, which omits od-fs/ and other build deps.
if git -C "$FSUAE_SRC" sparse-checkout list >/dev/null 2>&1; then
    git -C "$FSUAE_SRC" sparse-checkout disable >/dev/null 2>&1 || true
fi

git -C "$FSUAE_SRC" checkout "$FSUAE_TAG" -- . 2>/dev/null || git -C "$FSUAE_SRC" checkout "$FSUAE_TAG"

# -- 4. Drop in fsuae_rpc.cpp + apply patch --
step "Applying fsuae_rpc patch"
cp -f "$HERE/fsuae_rpc.cpp" "$FSUAE_SRC/src/fsuae_rpc.cpp"
info "Copied fsuae_rpc.cpp -> src/"

if git -C "$FSUAE_SRC" apply --check "$HERE/patches/0001-fsuae-rpc-hook.patch" 2>/dev/null; then
    git -C "$FSUAE_SRC" apply "$HERE/patches/0001-fsuae-rpc-hook.patch"
    info "Applied patches/0001-fsuae-rpc-hook.patch"
else
    fail "Patch does not apply cleanly to $FSUAE_TAG.  Regenerate the patch."
fi

# -- 5. Configure + build --
step "Bootstrap"
( cd "$FSUAE_SRC" && ./bootstrap )

step "Configure"
( cd "$FSUAE_SRC" && ./configure )

step "Build (make -j$JOBS)"
( cd "$FSUAE_SRC" && make -j"$JOBS" )

# -- 6. Smoke test --
step "Smoke-testing built binary"
"$FSUAE_SRC/fs-uae" --version | head -1

step "Done"
echo "Binary: $FSUAE_SRC/fs-uae"
echo
echo "Launch with RPC enabled:"
echo "  FSUAE_RPC_PORT=8765 $FSUAE_SRC/fs-uae <config.fs-uae>"
echo
echo "Then drive it from another shell:"
echo "  curl -s http://127.0.0.1:8765/v1/ping"
echo "  curl -sX POST http://127.0.0.1:8765/v1/pause"
echo "  curl -s http://127.0.0.1:8765/v1/cpu"
echo "  curl -s 'http://127.0.0.1:8765/v1/mem?addr=0xC0&len=64'"
echo "  curl -sX POST http://127.0.0.1:8765/v1/resume"
