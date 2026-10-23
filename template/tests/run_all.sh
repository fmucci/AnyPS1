#!/usr/bin/env bash
# Thin wrapper: the portable driver is tests/run_all.py (macOS, Linux, Windows).
#   ./tests/run_all.sh            quick: unit tests + 1500 differential cases + mutation check (about 1 minute)
#   ./tests/run_all.sh --long     30000 differential cases over 5 seeds
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 -I tests/run_all.py "$@"
