#!/usr/bin/env bash
# Full build on macOS and Linux (POSIX counterpart of build.bat): checks the game files, builds the translator, translates the game
# (cached), builds everything, runs the tests, stages dist/ and copies it into UC_INSTALL_DIR.
#   scripts/build.sh [--no-install] [--translate-only] [--no-tests]
# Settings come from the shell or local.env (local.env.example): UC_INSTALL_DIR (required), UC_SLANGC or VULKAN_SDK, UC_THINLTO (default ON).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
GEN="$ROOT/generated"
. "$ROOT/scripts/local.sh"
NO_INSTALL=0 TRANSLATE_ONLY=0 NO_TESTS=0
for arg in "$@"; do
  case "$arg" in
    --no-install) NO_INSTALL=1 ;;
    --translate-only) TRANSLATE_ONLY=1 ;;
    --no-tests) NO_TESTS=1 ;;
    *) echo "usage: $0 [--no-install] [--translate-only] [--no-tests]" >&2; exit 2 ;;
  esac
done
[ -n "${UC_INSTALL_DIR:-}" ] || { echo "ERROR: UC_INSTALL_DIR is not set. Copy local.env.example to local.env and set it to the folder with game/DATA/sys/main.dol (docs/building-macos.md)." >&2; exit 1; }
GAME_DOL="$UC_INSTALL_DIR/game/DATA/sys/main.dol"

echo "[1/6] Checking game files"
[ -d "$UC_INSTALL_DIR" ] || { echo "ERROR: install directory not found: $UC_INSTALL_DIR" >&2; exit 1; }
[ -f "$GAME_DOL" ] || { echo "ERROR: main.dol not found: $GAME_DOL" >&2; exit 1; }
for tool in cmake ninja python3; do command -v "$tool" >/dev/null || { echo "ERROR: $tool not found (brew install cmake ninja python)" >&2; exit 1; }; done
if [ -z "${UC_SLANGC:-}" ]; then
  for candidate in "${VULKAN_SDK:-/nonexistent}/bin/slangc" "$(command -v slangc || true)"; do
    [ -n "$candidate" ] && [ -x "$candidate" ] && { UC_SLANGC="$candidate"; break; }
  done
fi
[ -n "${UC_SLANGC:-}" ] && [ -x "$UC_SLANGC" ] || { echo "ERROR: slangc not found: set UC_SLANGC or VULKAN_SDK (docs/building-macos.md)" >&2; exit 1; }
mkdir -p "$BUILD"
JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )"
configure() {
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DUC_SLANGC="$UC_SLANGC" -DUC_THINLTO="${UC_THINLTO:-ON}" > "$BUILD/configure.log" 2>&1 \
    || { echo "ERROR: configure failed (see $BUILD/configure.log)" >&2; tail -20 "$BUILD/configure.log" >&2; exit 1; }
}

echo "[2/6] Configure and build the translator"
configure
cmake --build "$BUILD" --target translator > "$BUILD/translator_build.log" 2>&1 \
  || { echo "ERROR: translator build failed (see $BUILD/translator_build.log)" >&2; tail -20 "$BUILD/translator_build.log" >&2; exit 1; }

echo "[3/6] Translating the game (cached by DOL + translator + uc.cfg + hle.cfg hash)"
ROOT="$ROOT" BUILD="$BUILD" GEN="$GEN" GAME_DOL="$GAME_DOL" "$ROOT/scripts/translate.sh" || { echo "ERROR: translation step failed" >&2; exit 1; }
[ "$TRANSLATE_ONLY" = 1 ] && exit 0

echo "[4/6] Build everything and run tests"
configure   # again: game_sources.cmake now exists, so uc_game and uc are defined
cmake --build "$BUILD" || { echo "ERROR: build failed" >&2; exit 1; }
if [ "$NO_TESTS" = 0 ]; then
  # The GPU tests run on KosmicKrisp (Mesa's Vulkan on Metal) unless a driver is chosen already; MoltenVK has no logic ops.
  kk=/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json
  [ -n "${VK_DRIVER_FILES:-}${VK_ICD_FILENAMES:-}" ] || [ ! -f "$kk" ] || export VK_DRIVER_FILES="$kk"
  echo "Running tests with up to $JOBS parallel jobs"
  ctest --test-dir "$BUILD" --output-on-failure -j "$JOBS" || { echo "ERROR: tests failed" >&2; exit 1; }
fi

echo "[5/6] Staging dist"
mkdir -p "$DIST"
cmake --install "$BUILD" --prefix "$DIST" > "$BUILD/install.log" 2>&1 || { echo "ERROR: install to dist failed (see $BUILD/install.log)" >&2; exit 1; }

if [ "$NO_INSTALL" = 1 ]; then echo "[6/6] Skipping copy (--no-install)"; exit 0; fi
echo "[6/6] Copying dist to $UC_INSTALL_DIR"
rsync -a "$DIST/" "$UC_INSTALL_DIR/" || { echo "ERROR: copy failed" >&2; exit 1; }
echo "Done."
