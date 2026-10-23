# Linux verification image (queue P0-7 / P5-4): toolchain only; the repository is copied in at run time and the disc is mounted read-only, so no game data
# is ever baked into an image or committed. Build/run through docker/linux/run.sh.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential python3 perl rsync libsdl2-dev ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /work
