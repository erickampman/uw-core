#!/usr/bin/env bash
#
# Initialise every dependency submodule uw-core needs, at the pinned commits.
#
# Run this once after a fresh `git clone`, and again after a `git pull` that
# moves, adds or re-points a submodule.
#
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> Submodule URLs (after a pull that changed one)"
git submodule sync --recursive

echo "==> Submodules: the CLAP SDK"
git submodule update --init

echo "==> Done. Configure with: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
