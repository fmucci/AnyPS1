#!/usr/bin/env bash
# Runs a command inside the Windows image on a private copy of the sources; the disc is mounted read-only and /out is the host's build/win.
# Wine is /usr/lib/wine/wine64 (the package installs no `wine` command).
#   docker/windows/run.sh bash -c 'PS1_TARGET=windows PS1_CC=x86_64-w64-mingw32-gcc python3 -I tools/recomp/build_game.py -O1 && /usr/lib/wine/wine64 build/ps1_game.exe ...'
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build -q -t ps1-windows docker/windows >/dev/null
mkdir -p build/win
docker run --rm -v "$PWD":/src:ro -v "$PWD/og-files":/disc:ro -v "$PWD/build/win":/out ps1-windows bash -c '
  set -euo pipefail
  rsync -a --exclude build --exclude work --exclude og-files --exclude .git /src/ /work/ 2>/dev/null || { mkdir -p /work && cd /src && tar cf - --exclude=./build --exclude=./work --exclude=./og-files --exclude=./.git . | (cd /work && tar xf -); }
  mkdir -p /work/build && ln -s /disc /work/og-files
  cd /work && "$@"' _ "$@"
