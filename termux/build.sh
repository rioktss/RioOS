#!/usr/bin/env bash
# Delegasi ke build script utama (satu sumber kebenaran).
set -euo pipefail
exec bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../linux/build.sh" "$@"
