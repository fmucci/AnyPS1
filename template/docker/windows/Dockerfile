# Windows verification image (queue P5-5): MinGW-w64 cross-compiles the whole game and Wine runs the result, inside a Linux container. amd64 on purpose (Wine is x86);
# on Apple Silicon Docker runs it through emulation, so it is slow. No game data is copied into the image.
FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential python3 gcc-mingw-w64-x86-64 wine64 ca-certificates curl xz-utils && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/root/.wine
WORKDIR /work
