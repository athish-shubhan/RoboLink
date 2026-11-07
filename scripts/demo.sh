#!/usr/bin/env bash
# Native demo (no Docker): builds if needed, then runs the terminal dashboard.
set -euo pipefail
cd "$(dirname "$0")/.."
make build >/dev/null
exec build/rl_demo
