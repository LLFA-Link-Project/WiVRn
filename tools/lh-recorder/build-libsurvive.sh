#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 NEW_SOURCE_DIRECTORY BUILD_DIRECTORY" >&2
    exit 2
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$1
build_dir=$2
if [ -e "$source_dir" ]; then
    echo "Source directory already exists; choose a new directory (no existing checkout is modified)." >&2
    exit 1
fi
git clone https://github.com/collabora/libsurvive.git "$source_dir"
git -C "$source_dir" checkout 23f03665070549f0c13a6e768dfaf7764f75c417
git -C "$source_dir" apply "$script_dir/../../patches/libsurvive/0001-usbmon-safe-passive-recording.patch"
cmake -S "$source_dir" -B "$build_dir" \
    -DDO_CORE_BUILD=ON -DBUILD_STATIC=OFF -DENABLE_survive-cli=ON \
    -DENABLE_driver_usbmon=ON -DENABLE_driver_vive=ON \
    -DENABLE_poser_mpfit=ON -DENABLE_disambiguator_statebased=ON \
    -DUSE_OPENBLAS=OFF
cmake --build "$build_dir" --target survive-cli -j4
