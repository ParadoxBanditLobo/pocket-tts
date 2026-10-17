#!/bin/sh
# Devuan-first developer launcher. Does not install packages or modify Python.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="$ROOT/.build/desktop"
if [ "${1:-}" != "--run-only" ]; then
    cmake -S "$ROOT/desktop" -B "$BUILDDIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILDDIR" --parallel 2
fi
if [ ! -x "$BUILDDIR/pocket-tts-desktop" ]; then
    echo "Desktop binary missing. Build first with: sh desktop/run-devuan.sh" >&2
    exit 1
fi
cd "$ROOT"
# Prevent model downloads and telemetry during synthesis.
export HF_HUB_OFFLINE=1
export HF_HUB_DISABLE_TELEMETRY=1
export DO_NOT_TRACK=1
export PYTHONPATH="$ROOT${PYTHONPATH:+:$PYTHONPATH}"
exec "$BUILDDIR/pocket-tts-desktop"
