#!/bin/sh
# Play the game by hand at the heavy chapter 1 train fight: menus and a bot at speed 8 skip there, the game drops to speed 1 and the
# harness lets go (isolated saves, 1280x720 window, unlocked fps). Close the game window to end.
#   scripts/play.sh                 # the fight, god mode (health/ammo full)
#   scripts/play.sh --at start      # chapter 1 gameplay start instead
#   scripts/play.sh --mortal        # no god mode
#   scripts/play.sh --quest         # the Quest app (hold quest3-lock); Ctrl-C ends
# Other options go to `uctest run play` (e.g. --chapter 2 --from NAME, --game-arg=--render-scale --game-arg=2, --exe PATH).
cd "$(dirname "$0")/.." || exit 1
god=--god pick_exe=1 n=$#
while [ "$n" -gt 0 ]; do   # drop --mortal, keep the rest in order
  a=$1; shift; n=$((n - 1))
  case $a in --mortal) god=; continue ;; --exe|--exe=*|--quest) pick_exe= ;; esac
  set -- "$@" "$a"
done
[ -n "$god" ] && set -- "$god" "$@"
if [ -n "$pick_exe" ] && [ ! -x dist/uc ] && [ ! -f dist/uc.exe ]; then set -- --exe build/game/uc "$@"; fi   # a Mac dev build lives in build/game
cd scripts && exec python3 -m uctest run play "$@"
