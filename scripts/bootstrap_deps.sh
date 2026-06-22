#!/usr/bin/env bash
#
# Fetch heavy prebuilt third-party dependencies that we deliberately keep OUT of
# git (large binaries) but still want vendored under deps/ so the CMake build
# picks them up automatically. Run once after cloning (and again to update):
#
#   ./scripts/bootstrap_deps.sh            # fetch anything missing
#   ./scripts/bootstrap_deps.sh --force    # re-fetch even if present
#
# Currently fetches:
#   - Intel Open Image Denoise (OIDN) → deps/oidn/  (enables TRACEY_WITH_OIDN)
#
# The core build works WITHOUT running this — OIDN-gated code stubs out and
# TRACEY_WITH_OIDN defaults OFF when deps/oidn is absent. Run it to turn the
# denoiser on.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
DEPS="$ROOT/deps"

FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

log() { printf '\033[1;36m[bootstrap]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[bootstrap] error:\033[0m %s\n' "$*" >&2; exit 1; }

# ── Intel Open Image Denoise ────────────────────────────────────────────────
OIDN_VERSION="2.5.0"

fetch_oidn() {
    local dest="$DEPS/oidn"
    if [[ -f "$dest/include/OpenImageDenoise/oidn.h" && "$FORCE" -ne 1 ]]; then
        log "OIDN already present at deps/oidn (use --force to re-fetch) — skipping."
        return 0
    fi

    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"
    [[ "$os" == "Darwin" ]] || err "OIDN bootstrap currently supports macOS only (got '$os'). Fetch a prebuilt for your platform from https://github.com/RenderKit/oidn/releases and extract include/ + lib/ into deps/oidn/."
    case "$arch" in
        arm64)  arch="arm64" ;;
        x86_64) arch="x86_64" ;;
        *) err "unsupported arch '$arch'" ;;
    esac

    local pkg="oidn-${OIDN_VERSION}.${arch}.macos"
    local url="https://github.com/RenderKit/oidn/releases/download/v${OIDN_VERSION}/${pkg}.tar.gz"
    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    log "Downloading OIDN ${OIDN_VERSION} (${arch})…"
    curl -fsSL --retry 3 -o "$tmp/oidn.tar.gz" "$url" \
        || err "download failed: $url"

    log "Extracting…"
    tar -xzf "$tmp/oidn.tar.gz" -C "$tmp"
    local src="$tmp/$pkg"
    [[ -d "$src/include" && -d "$src/lib" ]] || err "unexpected archive layout in $src"

    rm -rf "$dest"
    mkdir -p "$dest"
    cp -R "$src/include" "$dest/"
    cp -R "$src/lib" "$dest/"
    log "OIDN vendored → deps/oidn ($(du -sh "$dest" | cut -f1)). CMake will auto-enable TRACEY_WITH_OIDN."
}

# ── MaterialX (Core + Format + standard libraries) ──────────────────────────
# Built from source — we use only the document/XML API (not the shader
# codegen), so render/viewer/python/tests/gen-* are disabled for a fast build.
MATERIALX_VERSION="v1.39.5"

fetch_materialx() {
    local dest="$DEPS/materialx"
    if [[ -f "$dest/include/MaterialXCore/Document.h" && "$FORCE" -ne 1 ]]; then
        log "MaterialX already present at deps/materialx (use --force to re-fetch) — skipping."
        return 0
    fi

    command -v cmake >/dev/null || err "cmake is required to build MaterialX."
    command -v git   >/dev/null || err "git is required to fetch MaterialX."

    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    log "Cloning MaterialX ${MATERIALX_VERSION}…"
    git clone --depth 1 --branch "$MATERIALX_VERSION" \
        https://github.com/AcademySoftwareFoundation/MaterialX.git "$tmp/src" \
        || err "git clone failed"

    log "Configuring (Core+Format+stdlib only)…"
    cmake -S "$tmp/src" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release \
        -DMATERIALX_BUILD_TESTS=OFF \
        -DMATERIALX_BUILD_GEN_GLSL=OFF -DMATERIALX_BUILD_GEN_MSL=OFF \
        -DMATERIALX_BUILD_GEN_OSL=OFF -DMATERIALX_BUILD_GEN_MDL=OFF \
        -DMATERIALX_BUILD_RENDER=OFF -DMATERIALX_BUILD_VIEWER=OFF \
        -DMATERIALX_BUILD_GRAPH_EDITOR=OFF -DMATERIALX_BUILD_PYTHON=OFF \
        -DMATERIALX_BUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="$dest" \
        || err "cmake configure failed"

    log "Building + installing…"
    rm -rf "$dest"
    cmake --build "$tmp/build" --target install \
        -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
        || err "build failed"

    log "MaterialX vendored → deps/materialx ($(du -sh "$dest" | cut -f1)). CMake will auto-enable TRACEY_WITH_MATERIALX."
}

# ── OpenUSD (core: tf / sdf / usd / usdGeom — no imaging, no Python) ─────────
# Built from source via USD's own build_usd.py orchestrator (which also fetches
# + builds its one core dependency, TBB). We deliberately skip imaging /
# usdview / Python / tests so this stays a USD-core build (the bulky OpenEXR /
# OpenImageIO / OpenSubdiv stack is only needed once we add the Hydra delegate).
# This is the single heaviest dependency — expect a long first build.
USD_VERSION="v25.08"

fetch_usd() {
    local dest="$DEPS/usd"
    if [[ -f "$dest/include/pxr/pxr.h" && "$FORCE" -ne 1 ]]; then
        log "OpenUSD already present at deps/usd (use --force to re-fetch) — skipping."
        return 0
    fi

    command -v cmake   >/dev/null || err "cmake is required to build OpenUSD."
    command -v git     >/dev/null || err "git is required to fetch OpenUSD."
    command -v python3 >/dev/null || err "python3 is required to run build_usd.py."

    local tmp
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    log "Cloning OpenUSD ${USD_VERSION}…"
    git clone --depth 1 --branch "$USD_VERSION" \
        https://github.com/PixarAnimationStudios/OpenUSD.git "$tmp/src" \
        || err "git clone failed"

    log "Building USD core (no imaging / python / tests) → deps/usd. This is slow…"
    rm -rf "$dest"
    # --no-materialx: skip USD's MaterialX file-format plugin (usdMtlx). It
    # clashes with the build_usd-provided TBB (a `split` symbol ambiguity +
    # legacy tbb::task API) on modern toolchains, and we don't need it — Tracey
    # has its own MaterialX importer. USD-native MaterialX is a later concern.
    python3 "$tmp/src/build_scripts/build_usd.py" \
        --no-python --no-imaging --no-usdview --no-materialx \
        --no-examples --no-tutorials --no-tests --no-docs --no-tools \
        "$dest" \
        || err "build_usd.py failed (see output above)"

    log "OpenUSD vendored → deps/usd ($(du -sh "$dest" | cut -f1)). CMake will auto-enable TRACEY_WITH_USD."
}

fetch_oidn
fetch_materialx
fetch_usd
log "Done. Re-run CMake configure so the build detects the new dependencies."
