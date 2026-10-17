# Desktop fork: foundation and privacy audit (checkpoint A)

This fork aims to preserve the original Pocket TTS model's voice quality and
local reference-audio cloning, while adding an optional **raylib desktop
interface**. Python/PyTorch remain in the inference backend. No ONNX conversion
is needed. The original CLI and server remain intact.

## Current status

Checkpoint A provides a **persistent Python engine worker** at
\`pocket_tts/desktop/worker.py\` and model-free worker protocol tests. It does
**not yet add a GUI**: the raylib frontend is the next batch and will talk to
this worker through anonymous OS pipes, not through an HTTP server. The worker
is internal infrastructure, not a public scripting interface.

The worker uses unchanged upstream functionality:

- \`TTSModel.load_model()\` once, on the first generation request.
- \`get_state_for_audio_prompt(Path(...), truncate=True)\` for local references.
- \`generate_audio_stream(...)\` followed by the original
  \`stream_audio_chunks(...)\` 16-bit WAV writer.
- An in-memory LRU of at most three voice states (keyed by canonical local
  path, file size and nanosecond modification time).
- Temporary \`*.part.wav\` output followed by atomic replacement on success.

This makes fidelity comparisons with the original Pocket TTS straightforward.

## Offline and telemetry audit: first pass

Examined: \`pocket_tts/main.py\`, \`pocket_tts/utils/utils.py\`,
\`pocket_tts/models/tts_model.py\`, \`pocket_tts/data/audio.py\`,
\`pocket_tts/static/index.html\` and package dependencies in \`pyproject.toml\`.

**What was observed**

- The original model loader uses Hugging Face to retrieve models/tokenizers
  when they are missing. Built-in/URL-based voices may also download data.
  \`download_if_necessary()\` additionally supports HTTPS downloads via requests.
- The existing \`pocket-tts serve\` command launches FastAPI/Uvicorn and binds
  to localhost by default. Its API accepts optional remote voice URLs.
- The original browser page imports Tailwind from \`cdn.tailwindcss.com\`, a
  third-party network dependency in the *browser UI*, not in model inference.
- The inspected inference code has logging/profiling and cache functionality,
  but **no explicit analytics or telemetry sender was identified**.

**Desktop-specific behavior**

- The new worker sets \`HF_HUB_OFFLINE=1\`, \`HF_HUB_DISABLE_TELEMETRY=1\` and
  \`DO_NOT_TRACK=1\` **before importing the model stack**. The intention is
  strictly local operation after explicit model setup.
- Its voice input accepts **only an absolute path to an existing local file**;
  \`https://\` and \`hf://\` voice references are deliberately not supported
  inside the desktop worker. Users can still use them via the original CLI.
- The worker runs no HTTP listener and creates no sockets directly.
- No reference recording or generated audio is uploaded by this worker.
- This is an **application-level audit**, not a verified network sandbox or a
  comprehensive audit of all transitive packages. No "guaranteed zero
  connections under every dependency" claim is being made.

To enforce no network at the OS level, users can optionally run the
application under their preferred system firewall or network namespace. We
will not add a mandatory firewall dependency.

## Linux / Devuan setup (developer preview)

If you already have a working original Pocket TTS virtual environment, keep
it; **do not uninstall or replace it**. You can activate it and run the source
checkout directly if it has compatible dependencies.

For a separate editable environment on Devuan, use a project-local virtual
environment:

\`\`\`sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install torch --index-url https://download.pytorch.org/whl/cpu
python -m pip install -e ".[audio]"
\`\`\`

This is a **developer setup**, not the final unzip-and-run release. The optional
\`soundfile\` dependency supports non-16-bit WAV, MP3, FLAC and similar audio
formats subject to the installed libsndfile's codecs. M4A may require
conversion (it is not guaranteed to decode).

First-time model download, **explicitly initiated by the user**:

\`\`\`sh
python -c 'from pocket_tts import TTSModel; TTSModel.load_model()'
\`\`\`

This may contact Hugging Face, prompt for the acceptance of model terms, or
require an existing read-only Hugging Face token, depending on model and
access conditions. We do not store credentials in the repository or add
authentication to the desktop GUI. The worker then runs in HF offline mode
and fails if the needed model isn't already cached.

### Model-free tests

\`\`\`sh
python -m unittest discover -s tests -p test_desktop_worker.py -v
\`\`\`

Tests mock the model and WAV writer to verify the wire protocol, one-time model
initialization, per-voice cache, local-path validation, and safe handling of
failed generations. No downloads or PyTorch model weights are needed.

## Next batches

1. Add a small **raylib C/C++ GUI** with text field, local audio picker,
   generate, play, save WAV, and status. It will launch
   \`python -m pocket_tts.desktop.worker\` with pipes.
2. Verify that a WAV generated through the desktop worker sounds like the
   original Python server using the **same model, text and reference**.
3. Improve interface editing and optional voice library; add a user-friendly
   model preparation step only after working synthesis is established.
4. Extract a minimal re-usable desktop engine API suitable for later
   ZenoVox integration, without linking raylib into the synthesis core.

The older PocketTTS.cpp fork remains an experimental native port; **do not
merge its inference modifications into this fidelity-first fork**.

## Licensing and privacy precautions

Upstream code is MIT licensed (retain the original LICENSE and notices).
Model weights and individual reference recordings have their own provenance,
terms, and consent considerations. Do not add actual personal voice files,
Hugging Face credentials, caches, or generated audio to this public Git repo.


## Checkpoint B: optional raylib desktop GUI

A C++17 raylib interface is now available in `desktop/main.cpp`. It launches
`python -u -m pocket_tts.desktop.worker` with anonymous local pipes: **no
FastAPI server or browser**. Model sessions and up to three reference voice
states are held by the worker. Use local WAV/MP3/FLAC/OGG or exported
safetensors voice references. The original engine and its WAV writer are
unchanged.

On Devuan, install X11/OpenGL build dependencies, a C++ compiler, CMake >=3.25,
and optionally `zenity` for Browse and Save:

```sh
sudo apt-get install build-essential cmake git libx11-dev libxrandr-dev libxi-dev libxinerama-dev libxcursor-dev libgl1-mesa-dev libasound2-dev zenity
sh desktop/run-devuan.sh
```

CMake uses installed raylib 5.5 if present or downloads its 5.5 source. It does
not install packages or modify system configuration. The launcher chooses
`./.venv/bin/python` when present; otherwise `python3` on PATH. Set
`POCKET_TTS_PYTHON=/absolute/path/to/python` if your existing Pocket TTS
installation uses another virtual environment.

**Model preparation:** The worker is deliberately HF-offline. You must first
load the original model once using the existing Python installation while
online, following the upstream model's license/access requirements. The
original `pocket-tts generate` command can serve as a quality baseline.

**Known limitations:** The GUI implements a simple append-and-backspace editor
(Ctrl+A clears), drag/drop or optional Zenity Browse, WAV play/save and
status. Its text drawing uses raylib's default limited font; Unicode audio
text can be passed but glyph rendering may be incomplete. Only one generation
runs at a time. Model loading is lazy at the first Generate action. Full
Devuan compilation and actual speech playback require laptop acceptance tests.


## Resolving a missing model (LocalEntryNotFoundError)

The current fork's **default** English config is `english_2026-09_2`. A previously installed
Pocket TTS from an older pip release may have downloaded a different model,
so the old model cache alone is not sufficient. The GUI itself intentionally
disables Hugging Face downloads. To cache the expected default weights and
tokenizer before synthesizing, run from the checked-out repo:

```sh
sh desktop/prepare-model.sh
```

This command **asks permission** before contacting Hugging Face. It uses the
selected `POCKET_TTS_PYTHON` environment, or the active venv/local venv.
You may need to accept the gated voice-cloning model terms at
https://huggingface.co/kyutai/pocket-tts and sign in through the Hugging Face
CLI. The script fails clearly if only the model without voice cloning was
loaded. After preparation, launch as usual; no network is needed during
speech generation (assuming your selected local reference is readable).
