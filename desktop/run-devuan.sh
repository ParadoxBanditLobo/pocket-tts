#!/bin/sh
# Devuan test runner: no system installs or changes to Python environments.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILDDIR="$ROOT/.build/desktop"
if [ -n "${POCKET_TTS_PYTHON:-}" ]; then
    PYTHON="$POCKET_TTS_PYTHON"
elif [ -n "${VIRTUAL_ENV:-}" ] && [ -x "$VIRTUAL_ENV/bin/python" ]; then
    PYTHON="$VIRTUAL_ENV/bin/python"
elif [ -x "$ROOT/.venv/bin/python" ]; then
    PYTHON="$ROOT/.venv/bin/python"
else
    PYTHON=python3
fi
export PYTHONPATH="$ROOT${PYTHONPATH:+:$PYTHONPATH}"
if ! "$PYTHON" -c "from pocket_tts import TTSModel; from pocket_tts.data.audio import stream_audio_chunks" >/dev/null; then
    echo "The selected Python lacks Pocket TTS runtime dependencies." >&2
    echo "Activate your working Pocket TTS Python environment or run:" >&2
    echo "  POCKET_TTS_PYTHON=/absolute/path/to/your/python sh desktop/run-devuan.sh" >&2
    echo "No packages were installed or changed." >&2
    exit 1
fi
if [ "${1:-}" != "--run-only" ]; then
    cmake -S "$ROOT/desktop" -B "$BUILDDIR" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILDDIR" --parallel 2
fi
if [ ! -x "$BUILDDIR/pocket-tts-desktop" ]; then
    echo "Desktop binary missing. Build first with: sh desktop/run-devuan.sh" >&2
    exit 1
fi
cd "$ROOT"
export POCKET_TTS_PYTHON="$PYTHON"
export HF_HUB_OFFLINE=1
export HF_HUB_DISABLE_TELEMETRY=1
export DO_NOT_TRACK=1
exec "$BUILDDIR/pocket-tts-desktop"
