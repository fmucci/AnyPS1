#!/usr/bin/env bash
# Checks that template/ is healthy WITHOUT any game: copies it to a temp dir and runs its generic test suite (recompiler differential tests, mutation check, runtime and hardware unit tests,
# memory card tests, stub registry, generic checks). Run it before committing a change to template/.   usage: scripts/verify_template.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cp -R "$HERE/template/." "$T/"
cd "$T" && ./tests/run_all.sh 2>&1 | tail -5
