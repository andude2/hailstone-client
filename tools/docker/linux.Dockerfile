# Linux release builds: Ubuntu 24.04 (GCC 13, glibc 2.39), so the client runs on 24.04 and newer (docs/ci.md).
FROM ubuntu:24.04
# SDL finds Wayland only through pkg-config, needing egl.pc (libegl-dev) as well; without it SDL was built X11-only up
# to 0.7.0-beta, without a word (cmake/Dependencies.cmake now stops instead). pkg-config is listed rather than left to
# arrive with another package.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates python3 file ccache pkg-config \
    ninja-build glslang-tools zlib1g-dev clang libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libdecor-0-dev libdrm-dev \
    libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev && rm -rf /var/lib/apt/lists/*
# CEF's libcef.so links against these (GitHub's ubuntu-24.04 runners have them preinstalled).
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libcups2t64 libxcomposite1 libxdamage1 libcairo2 \
    libpango-1.0-0 libxrandr2 libxkbcommon0 libgbm1 libdrm2 libasound2t64 libdbus-1-3 libexpat1 libxshmfence1 \
    libglib2.0-0t64 libx11-xcb1 && rm -rf /var/lib/apt/lists/*
