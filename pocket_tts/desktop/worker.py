"""Persistent local-only Pocket TTS engine for the optional raylib desktop.

Uses the upstream TTSModel and upstream WAV writer unchanged. No HTTP server,
voice upload, GUI library, or third-party IPC package is involved.

Wire protocol (stdin/stdout pipes, UTF-8):
  Worker -> READY\n
  GUI -> GEN <voice-bytes> <text-bytes> <output-bytes>\n<concatenated bytes>
  Worker -> OK\n   or   ERR <utf8-byte-count>\n<utf8 error bytes>
  GUI -> QUIT\n

All paths and text are length-prefixed, not shell-evaluated. Only absolute,
existing *local* audio files are accepted as reference voices. Model files
must be preloaded separately; synthesis runs with Hugging Face offline mode.
"""

from __future__ import annotations

import os
import sys
from collections import OrderedDict
from collections.abc import Callable
from pathlib import Path
from typing import Any, BinaryIO

# Set before importing huggingface_hub and the actual inference package.
os.environ["HF_HUB_OFFLINE"] = "1"
os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
os.environ["DO_NOT_TRACK"] = "1"

MAX_VOICE_BYTES = 4096
MAX_TEXT_BYTES = 12000
MAX_OUTPUT_BYTES = 4096
SUPPORTED_EXTENSIONS = {".wav", ".mp3", ".flac", ".ogg", ".safetensors"}


class ProtocolError(Exception):
    """A malformed or truncated local IPC message."""


def _read_exact(input_stream: BinaryIO, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        part = input_stream.read(size - len(data))
        if not part:
            raise ProtocolError("Unexpected end of local desktop request.")
        data.extend(part)
    return bytes(data)


def _read_request(input_stream: BinaryIO) -> tuple[str, str, str] | None:
    header = input_stream.readline(128)
    if not header:
        return None
    if header == b"QUIT\n":
        return None

    parts = header.strip().split(b" ")
    if len(parts) != 4 or parts[0] != b"GEN":
        raise ProtocolError("Unrecognized local desktop request.")
    try:
        lengths = [int(v) for v in parts[1:]]
    except ValueError as exc:
        raise ProtocolError("Invalid desktop request lengths.") from exc

    limits = (MAX_VOICE_BYTES, MAX_TEXT_BYTES, MAX_OUTPUT_BYTES)
    if any(n < 1 or n > limit for n, limit in zip(lengths, limits)):
        raise ProtocolError("Desktop request exceeded size limits.")

    try:
        return tuple(_read_exact(input_stream, n).decode("utf-8") for n in lengths)
    except UnicodeDecodeError as exc:
        raise ProtocolError("Desktop request contains invalid UTF-8.") from exc


def _reply(output_stream: BinaryIO, error: str | None = None) -> None:
    if error is None:
        output_stream.write(b"OK\n")
    else:
        message = error.encode("utf-8", errors="replace")[:8192]
        output_stream.write(f"ERR {len(message)}\n".encode("ascii"))
        output_stream.write(message)
    output_stream.flush()


def _validate_local_voice(voice: str) -> tuple[Path, tuple[str, int, int]]:
    path = Path(voice).expanduser()
    if not path.is_absolute():
        raise ValueError("Choose an absolute local audio file path.")
    if path.suffix.lower() not in SUPPORTED_EXTENSIONS:
        raise ValueError("Supported references: WAV, MP3, FLAC, OGG or safetensors.")
    if not path.is_file():
        raise ValueError("Reference audio file was not found.")
    stat = path.stat()
    if stat.st_size == 0:
        raise ValueError("Reference audio is empty.")
    path = path.resolve(strict=True)
    return path, (str(path), stat.st_size, stat.st_mtime_ns)


def serve(
    input_stream: BinaryIO,
    output_stream: BinaryIO,
    model_factory: Callable[[], Any],
    wav_writer: Callable[[Path, Any, int], None],
) -> None:
    """Process serial requests. Importable and testable without torch/model assets."""
    model: Any = None
    states: OrderedDict[tuple[str, int, int], Any] = OrderedDict()
    output_stream.write(b"READY\n")
    output_stream.flush()

    while True:
        try:
            request = _read_request(input_stream)
        except ProtocolError as exc:
            _reply(output_stream, str(exc))
            break  # Input stream is no longer safely aligned.
        if request is None:
            break

        voice, text, destination = request
        try:
            if not text.strip():
                raise ValueError("Enter text to generate.")
            ref, key = _validate_local_voice(voice)
            out = Path(destination)
            if not out.is_absolute() or out.suffix.lower() != ".wav":
                raise ValueError("Output must be an absolute WAV path.")
            if not out.parent.is_dir():
                raise ValueError("Output directory does not exist.")

            if model is None:
                model = model_factory()

            if key in states:
                state = states[key]
                states.move_to_end(key)
            else:
                state = model.get_state_for_audio_prompt(ref, truncate=True)
                states[key] = state
                if len(states) > 3:
                    states.popitem(last=False)

            # Generate through the original API, writing with the original
            # streaming WAV implementation used by the working web demo.
            partial = out.with_name(out.stem + ".part.wav")
            try:
                wav_writer(
                    partial, model.generate_audio_stream(state, text), model.sample_rate
                )
                os.replace(partial, out)
            finally:
                partial.unlink(missing_ok=True)
            _reply(output_stream)
        except Exception as exc:
            # Keep paths and credentials out of protocol errors by default.
            # Detailed tracebacks remain in the user's own terminal on stderr.
            print(f"Desktop synthesis error: {type(exc).__name__}: {exc}", file=sys.stderr)
            _reply(output_stream, f"{type(exc).__name__}: {exc}")


def main() -> None:
    # Reserve stdout exclusively for the framed protocol; library messages
    # using Python print() go to stderr rather than corrupting the pipe.
    output = sys.stdout.buffer
    sys.stdout = sys.stderr
    from pocket_tts.data.audio import stream_audio_chunks
    from pocket_tts.models.tts_model import TTSModel

    serve(
        sys.stdin.buffer,
        output,
        TTSModel.load_model,
        stream_audio_chunks,
    )


if __name__ == "__main__":
    main()
