#!/usr/bin/env bash
# Generates project/hle.cfg and translates the game when the inputs changed (POSIX counterpart of translate.bat).
# Expects ROOT, BUILD, GEN, GAME_DOL in the environment and the translator already built. Returns 1 on failure.
set -euo pipefail
export CFG="$ROOT/project/uc.cfg"
export HLECFG="$ROOT/project/hle.cfg"
export TRANSLATOR="$BUILD/tools/translator/translator"
export GAME_DOL
python3 -I "$ROOT/tools/hle_cfg.py" "$ROOT/project" || { echo "ERROR: hle.cfg generation failed" >&2; exit 1; }
KEY="$(python3 -I "$ROOT/tools/translate_key.py")" || true
[ -n "$KEY" ] || { echo "ERROR: could not hash inputs for the translate cache" >&2; exit 1; }
if [ -f "$GEN/game_sources.cmake" ] && grep -qxF "# translation-input: $KEY" "$GEN/game_sources.cmake"; then
  echo "generated/ is up to date"
else
  # the translator keeps every earlier generation (~50 MB each); only the one in game_sources.cmake is used
  if ! "$TRANSLATOR" --dol "$GAME_DOL" --cfg "$CFG" --cfg "$HLECFG" --out "$GEN" --shard 150 --cache-key "$KEY" > "$BUILD/translate.log" 2>&1; then
    echo "ERROR: translation failed" >&2
    cat "$BUILD/translate.log" >&2
    exit 1
  fi
  cat "$BUILD/translate.log"
fi
python3 -I "$ROOT/tools/prune_generations.py" "$GEN"
