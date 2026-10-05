#!/bin/sh

##  Install required dependencies for limine bootloader
## 1. Clone the limine bootloader repository (if not already present)
## 2. Build the limine host tool

echo "[Status] : Installing dependencies for limine bootloader..."

set -e
cd "$(dirname "$0")/.."

# Limine bootloader binaries (GitHub mirror; use codeberg.org/Limine/Limine if you prefer)
if [ ! -d limine ]; then
  git clone --depth=1 --branch=v11.x-binary \
    https://github.com/limine-bootloader/limine.git limine
fi

make -C limine            # builds the host 'limine' tool used to install the BIOS boot sector

# limine.h protocol header, vendored into the repo
if [ ! -f third_party/limine.h ]; then
  tmp=$(mktemp -d)
  git clone --depth=1 https://github.com/limine-bootloader/limine-protocol.git "$tmp"
  cp "$tmp/include/limine.h" third_party/limine.h
  rm -rf "$tmp"
fi

echo "[Status] : Dependencies for limine bootloader installed successfully."
