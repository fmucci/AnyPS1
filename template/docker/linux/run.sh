#!/usr/bin/env bash
# Runs a command of this repository inside the Linux image, on a private copy of the sources (the host's build/ and work/ are never touched).
#   docker/linux/run.sh ./tests/run_all.sh
#   docker/linux/run.sh python3 -I tools/recomp/build_game.py -O1 --frontend
# The disc data (og-files/) is mounted read-only; SDL uses its dummy video/audio drivers; /out is the host's build/linux-out.
set -euo pipefail
cd "$(dirname "$0")/../.."
PLAT=${PS1_DOCKER_PLATFORM:+--platform $PS1_DOCKER_PLATFORM}      # e.g. PS1_DOCKER_PLATFORM=linux/amd64 for an x86-64 build (emulated on Apple Silicon: slow)
TAG=ps1-linux${PS1_DOCKER_PLATFORM:+-${PS1_DOCKER_PLATFORM#linux/}}
mkdir -p build/linux-out
docker build -q $PLAT -t $TAG docker/linux >/dev/null
docker run --rm $PLAT -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy \
  -v "$PWD":/src:ro -v "$PWD/og-files":/disc:ro -v "$PWD/build/linux-out":/out $TAG bash -c '
    set -euo pipefail
    rsync -a --exclude build --exclude work --exclude og-files --exclude .git /src/ /work/
    mkdir -p /work/build && ln -s /disc /work/og-files
    cd /work && "$@"' _ "$@"
