#!/usr/bin/env bash

set -eu

sudo apt-get update
sudo apt-get install -y \
    libavcodec-dev \
    libavutil-dev \
    libdrm-dev \
    libffmpeg-nvenc-dev \
    libgbm-dev \
    libgbm-dev \
    liblz4-dev \
    libswscale-dev \
    libva-dev \
    libweston-9-dev \
    libx264-dev \
    libzstd-dev \
    meson \
    scdoc \
    systemtap-sdt-dev \
    weston
