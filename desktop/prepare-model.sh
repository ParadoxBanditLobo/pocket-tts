#!/bin/sh
# Download the selected default model ONCE with explicit user approval.
# Normal GUI inference remains offline; no system Python modifications.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
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
if ! "$PYTHON" -c "from pocket_tts import TTSModel" >/dev/null; then
    echo "Selected Python cannot import Pocket TTS; set POCKET_TTS_PYTHON or activate your working environment." >&2
    exit 1
fi
echo "First-run preparation downloads the original Kyutai default English model/tokenizer to your Hugging Face cache."
echo "The current fork defaults to english_2026-09_2, which may differ from the model cached by an older pip installation."
echo "Model repositories may require accepting terms on Hugging Face and authenticating with the HF CLI."
printf "Allow this one-time model download? [y/N] "
read -r answer || answer=n
case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing downloaded."; exit 0 ;; esac
cd "$ROOT"
# Explicit opt-in step only. The GUI worker always forces offline mode.
export HF_HUB_OFFLINE=0
export HF_HUB_DISABLE_TELEMETRY=1
export DO_NOT_TRACK=1
"$PYTHON" - <<'PY'
from pocket_tts import TTSModel
m = TTSModel.load_model()
if not m.has_voice_cloning:
    raise SystemExit("Model loaded without voice cloning. To use local audio references, accept the original model access terms at https://huggingface.co/kyutai/pocket-tts and authenticate with the Hugging Face CLI, then retry.")
print("Default English voice-cloning model and tokenizer are ready for offline generation.")
PY
