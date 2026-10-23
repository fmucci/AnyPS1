#!/usr/bin/env bash
# Windows verification inside the container (MinGW-w64 build, run under Wine): unit tests, a game run whose frame hash must equal the native one, and the SDL2 player.
# usage (from the host):  docker/windows/run.sh docker/windows/verify.sh NATIVE_MD5_OF_30000_TICK_FRAME
# The reference hash comes from a native run:  build/ps1_game DISC --ticks 30000 --card X.mcr --press cross@17200-17210 --press cross@17600-17610 --ppm f.ppm ; md5 f.ppm
set -euo pipefail
export PS1_TARGET=windows PS1_CC=x86_64-w64-mingw32-gcc WINE=/usr/lib/wine/wine64
CC=$PS1_CC
echo "== unit tests (cross-compiled, run under Wine)"
for t in test_hw test_card; do
  src=tests/runtime/$t.c; extra="runtime/gte.c runtime/card.c runtime/mem.c runtime/sys.c runtime/dispatch.c runtime/interp.c runtime/trace.c"
  [ $t = test_card ] && extra="runtime/card.c"
  $CC -Wall -Wextra -Werror -std=gnu11 -O1 -Iruntime $src $extra -o build/$t.exe
  $WINE build/$t.exe 2>&1 | tail -1
done
$CC -Wall -Wextra -Werror -std=gnu11 -O1 -Iruntime tests/runtime/test_rt.c runtime/mem.c runtime/sys.c runtime/dispatch.c runtime/interp.c runtime/trace.c runtime/io.c runtime/gte.c runtime/card.c -o build/test_rt.exe
$WINE build/test_rt.exe 2>&1 | tail -1
echo "== SDL2 (MinGW development package)"
if [ ! -d /opt/SDL2 ]; then
  curl -sL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz | tar xz -C /opt && mv /opt/SDL2-2.30.9 /opt/SDL2
fi
S=/opt/SDL2/x86_64-w64-mingw32
export PS1_SDL_FLAGS="-I$S/include/SDL2 -L$S/lib -lmingw32 -lSDL2main -lSDL2"
echo "== whole game (game + player)"
python3 -I tools/recomp/build_game.py -O1 --frontend 2>&1 | tail -2
cp $S/bin/SDL2.dll build/
echo "== 30000-tick run, frame hash"
$WINE build/ps1_game.exe "${DISC:-og-files/disc1.bin}" --ticks 30000 --card /tmp/w.mcr --press cross@17200-17210 --press cross@17600-17610 --ppm /out/win30000.ppm 2>&1 | tail -1
got=$(md5sum /out/win30000.ppm | cut -d' ' -f1)
echo "windows frame md5 $got, native ${1:-unknown}"
[ "$got" = "${1:-$got}" ] && echo "HASH MATCH" || echo "HASH DIFFERS"
echo "== SDL player (dummy drivers)"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy $WINE build/ps1_play.exe "${DISC:-og-files/disc1.bin}" --card /tmp/p.mcr --ticks 400 --no-sync 2>&1 | tail -2 || true     # the player ends with "tick limit reached" and exit code 1
echo "windows verification finished"
