# Cross-build environment for the GTA SA loader on goodluckOS (armhf).
FROM debian:bookworm
RUN dpkg --add-architecture armhf && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates make pkg-config file patchelf \
        crossbuild-essential-armhf \
        libsdl2-dev:armhf libopenal-dev:armhf libopenal1:armhf zlib1g-dev:armhf && \
    rm -rf /var/lib/apt/lists/*
