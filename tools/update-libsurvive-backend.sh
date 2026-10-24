#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Track a local libsurvive llfa-main without modifying its working tree.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
backend_patch="$script_dir/../patches/libsurvive/0002-fix-global-scene-solver-shutdown.patch"
if [ "$#" -lt 2 ]; then
    echo "Usage: $0 LOCAL_LIBSURVIVE_REPOSITORY DEPENDENCY_DIRECTORY [CMAKE_OPTIONS...]" >&2
    exit 2
fi
repository=$(realpath "$1")
mkdir -p "$2"
dependencies=$(realpath "$2")
shift 2
checkout="$dependencies/libsurvive-llfa-main-src"
build="$dependencies/libsurvive-llfa-main-build"
prefix="$dependencies/libsurvive-llfa-main-install"
if [ ! -e "$checkout" ]; then
    git clone --no-hardlinks --single-branch --branch llfa-main "$repository" "$checkout"
else
    if [ "$(git -C "$checkout" remote get-url origin)" != "$repository" ] ||
       [ "$(git -C "$checkout" branch --show-current)" != llfa-main ]; then
        echo "Existing checkout must track llfa-main from $repository" >&2
        exit 1
    fi
    git -C "$checkout" diff --cached --quiet
    # Remove only our managed patch before updating the pristine branch.
    # Preserve any unrelated local changes and refuse to update over them.
    if ! git -C "$checkout" diff --quiet; then
        git -C "$checkout" apply --reverse --check "$backend_patch"
        git -C "$checkout" apply --reverse "$backend_patch"
        if ! git -C "$checkout" diff --quiet; then
            git -C "$checkout" apply "$backend_patch"
            echo "Unrelated local changes in $checkout; refusing to update." >&2
            exit 1
        fi
    fi
    git -C "$checkout" fetch origin llfa-main
    git -C "$checkout" merge --ff-only origin/llfa-main
fi
if git -C "$checkout" apply --check "$backend_patch"; then
    git -C "$checkout" apply "$backend_patch"
elif git -C "$checkout" apply --reverse --check "$backend_patch"; then
    echo "Shutdown fix is already included in the source revision."
else
    echo "Cannot apply libsurvive shutdown fix; review the upstream changes." >&2
    exit 1
fi
cmake -S "$checkout" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_STATIC=OFF -DBUILD_APPLICATIONS=OFF -DENABLE_TESTS=OFF \
    -DDOTNET=OFF -DPYTHON_GENERATED_DIR= -DUSE_OPENBLAS=OFF "$@"
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --install "$build"
git -C "$checkout" rev-parse HEAD > "$dependencies/libsurvive-llfa-main-revision.txt"
sha256sum "$backend_patch" > "$dependencies/libsurvive-llfa-main-patches.txt"
echo "libsurvive llfa-main revision: $(cat "$dependencies/libsurvive-llfa-main-revision.txt")"
echo "Configure WiVRn with PKG_CONFIG_PATH=$prefix/lib/pkgconfig"
# Clear cached pkg-config paths if the WiVRn build previously used another installation.
echo "For an existing WiVRn build, clear SURVIVE_*, LH_SURVIVE_*, __pkg_config_checked_*SURVIVE and pkgcfg_lib_*SURVIVE* with cmake -U."
