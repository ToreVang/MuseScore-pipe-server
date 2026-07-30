#!/usr/bin/env bash
# build_musescore.sh
# Builds MuseScore from the local repo with --pipe-server support.
# Run once; takes ~30-60 min. After that, the binary lives at:
#   /home/tore/Documents/OMR_repos/MuseScore/build.release/src/app/mscore
#
# Prerequisites (run ONCE as sudo, then re-run this script):
#   sudo apt-get install -y libgl-dev libopengl-dev libasound2-dev libfontconfig1-dev libfreetype-dev
#
# Usage:
#   bash build_musescore.sh

set -e
set -o pipefail

# ── Logging setup ────────────────────────────────────────────────────────
LOG_FILE="$HOME/musescore_build.log"
exec > >(tee -a "$LOG_FILE") 2>&1   # all stdout+stderr to terminal AND log file
echo ""
echo "Build log: $LOG_FILE"
echo "Started at: $(date)"
echo ""

# Print the exact command + line number when something fails
trap 'echo ""; echo "FAILED at line $LINENO: $BASH_COMMAND"; echo "Full log: $LOG_FILE"' ERR

REPO=/home/tore/Documents/OMR_repos/MuseScore
QT_DIR=$HOME/Qt/6.10.2/gcc_64
JOBS=$(nproc)

echo "======================================"
echo " MuseScore pipe-server build"
echo " Repo  : $REPO"
echo " Qt    : $QT_DIR"
echo " Cores : $JOBS"
echo "======================================"

# ── 0. Check prerequisites ───────────────────────────────────────────────
if [ ! -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    echo ""
    echo "ERROR: Qt 6.10.2 not found at $QT_DIR"
    echo "Install it with:"
    echo "  pip3 install --user --break-system-packages aqtinstall"
    echo "  ~/.local/bin/aqt install-qt linux desktop 6.10.2 linux_gcc_64 --outputdir ~/Qt"
    exit 1
fi

if [ ! -f /usr/include/GL/gl.h ]; then
    echo ""
    echo "ERROR: OpenGL headers missing. Run once:"
    echo "  sudo apt-get install -y libgl-dev libopengl-dev libasound2-dev libfontconfig1-dev"
    exit 1
fi

# ── 1. Submodules ────────────────────────────────────────────────────────
echo ""
echo "[1/3] Checking submodules..."
if [ ! -f "$REPO/muse/CMakeLists.txt" ]; then
    echo "  Initializing submodules (downloads ~200MB, a few minutes)..."
    git -C "$REPO" submodule update --init --recursive
else
    echo "  Submodules already initialized."
fi

# ── 2. CMake configure ──────────────────────────────────────────────────
echo ""
echo "[2/3] Configuring..."
export PATH=$HOME/.local/bin:$PATH  # cmake/ninja installed via pip

mkdir -p "$REPO/build.release"
cd "$REPO/build.release"

cmake "$REPO" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR/lib/cmake" \
    -DCMAKE_TOOLCHAIN_FILE="$QT_DIR/lib/cmake/Qt6/qt.toolchain.cmake" \
    -DMUSESCORE_BUILD_CONFIGURATION=app \
    -DMUSE_APP_BUILD_MODE=dev \
    -DCMAKE_BUILD_NUMBER=1 \
    -DMUSESCORE_REVISION=local \
    -DMUE_RUN_LRELEASE=ON \
    -DMUE_DOWNLOAD_SOUNDFONT=OFF \
    -DMUSE_ENABLE_UNIT_TESTS=OFF \
    -DMUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT=OFF \
    -DMUSE_MODULE_VST=OFF \
    -DMUSE_MODULE_NETWORK_WEBSOCKET=OFF \
    -DMUSE_MODULE_AUDIO_PIPEWIRE=OFF \
    -DMUSE_COMPILE_USE_UNITY=OFF

# ── 3. Build ─────────────────────────────────────────────────────────────
echo ""
echo "[3/3] Building with $JOBS cores (this takes 30-60 min)..."
echo "      Progress is shown below. Full output in: $LOG_FILE"
echo ""

# -k0 = keep going on errors so we see ALL failures, not just the first
ninja -j$JOBS -k0 || {
    echo ""
    echo "======================================"
    echo " BUILD FAILED — last 50 error lines:"
    echo "======================================"
    grep -E "error:|FAILED:" "$LOG_FILE" | tail -50
    echo ""
    echo "Full log: $LOG_FILE"
    exit 1
}

BINARY="$REPO/build.release/src/app/mscore"
if [ -f "$BINARY" ]; then
    echo ""
    echo "======================================"
    echo " BUILD SUCCESSFUL"
    echo " Binary: $BINARY"
    echo "======================================"

    # Update the SheetDigitizationTool config to point to new binary
    CFG=$HOME/.sheetdigitizationtool/user_config.json
    if [ -f "$CFG" ]; then
        python3 - "$BINARY" "$CFG" <<'PYEOF'
import json, sys
binary, cfg_path = sys.argv[1], sys.argv[2]
with open(cfg_path) as f:
    cfg = json.load(f)
old = cfg.get("MuseScorePath", "")
cfg["MuseScorePath"] = binary
with open(cfg_path, "w") as f:
    json.dump(cfg, f, indent=2)
print(f"  Config updated: {old} → {binary}")
PYEOF
    fi

    echo ""
    echo " Test it:"
    echo "  QT_QPA_PLATFORM=offscreen $BINARY --pipe-server"
    echo "  (type a JSON request, press Enter, should reply {\"status\":\"ok\"})"
    echo ""
    echo " Run benchmark:"
    echo "  python3 /home/tore/Documents/OMR_repos/SheetDigitizationTool/benchmark_musescore.py --mscore $BINARY"
else
    echo "ERROR: Build failed — binary not found at $BINARY"
    exit 1
fi
