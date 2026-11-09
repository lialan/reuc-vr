#!/usr/bin/env bash
# Records the Quest's optimizer profile: project/pgo/uc-quest.profdata, which scripts/build-quest.sh then uses for every build.
#   UC_QUEST_PGO=generate scripts/build-quest.sh && cp quest/reuc/build/outputs/apk/debug/reuc-debug.apk build/reuc-pgo-gen.apk
#   scripts/quest_pgo.sh [build/reuc-pgo-gen.apk]
# Plays the deterministic fightvr replay (build/demos/fightvr.demo: boot, menus, chapter 1 up to the heavy train fight) with the
# instrumented app on the headset under quest3-lock; the app writes uc.profraw into its private run folder when the game exits
# (android_main.cpp), which is pulled and merged with the NDK's llvm-profdata. Re-record after big changes to the translator or runtime.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APK="${1:-$ROOT/build/reuc-pgo-gen.apk}"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.3.13750724}"
PROFDATA="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-profdata"
PKG=org.uc.umbrellachronicles.vr
OUT="$ROOT/build-android/quest-runs/pgo-$(date +%Y%m%d-%H%M%S)"
WINDOW="$ROOT/build/demos/fightvr.json"
STOP=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['to'])" "$WINDOW")
mkdir -p "$OUT"
GAME=(--test-server demo --fps-limit 0 --save-root saves --god --skip-tutorials --skip-intro --deterministic --demo-play fightvr.demo
      --stop-retrace "$STOP" --mute --game-hz 120)
ARGS=(); for a in "${GAME[@]}"; do ARGS+=("--arg=$a"); done
adb logcat -c
# run.py's non-interactive wait ends only when the app's process does, which outlives the game: end the session at the game's exit
quest3-lock python3 -I "$ROOT/scripts/quest/run.py" --apk "$APK" --interactive --guardian-pause --out "$OUT" \
  --file "$ROOT/build/demos/fightvr.demo=fightvr.demo" --saves "$HOME/workspace/reuc-install/saves" "${ARGS[@]}" > "$OUT/run.txt" 2>&1 &
RUN=$!
until adb logcat -d -s uc 2>/dev/null | grep -q "game exited"; do
  kill -0 $RUN 2>/dev/null || { echo "ERROR: the run ended before the game exited (see $OUT)" >&2; exit 1; }
  sleep 5
done
touch "$OUT/STOP"; wait $RUN || true
grep "PGO profile.*: 0$" "$OUT/uc.log" || { echo "ERROR: no profile written (an instrumented APK?)" >&2; exit 1; }
PRIVATE="files/.codex-xr-$(cat "$ROOT/build-android/quest-private-id")"
adb exec-out run-as $PKG cat "$PRIVATE/uc.profraw" > "$OUT/uc.profraw"
mkdir -p "$ROOT/project/pgo"
"$PROFDATA" merge -o "$ROOT/project/pgo/uc-quest.profdata" "$OUT/uc.profraw"
ls -la "$OUT/uc.profraw" "$ROOT/project/pgo/uc-quest.profdata"
