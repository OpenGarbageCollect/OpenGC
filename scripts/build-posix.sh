#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake --build "${OPENGC_BUILD_TREE:-$source_dir/build}" "$@"
