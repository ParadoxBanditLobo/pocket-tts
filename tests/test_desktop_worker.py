"""Model-free protocol tests for the local desktop worker."""

from __future__ import annotations

import io
import tempfile
import unittest
from pathlib import Path

# Import the pure protocol module without importing pocket_tts/__init__.py
# (which deliberately imports torch for the normal public Python API).
import importlib.util

WORKER_FILE = Path(__file__).resolve().parents[1] / "pocket_tts" / "desktop" / "worker.py"
spec = importlib.util.spec_from_file_location("desktop_worker_under_test", WORKER_FILE)
assert spec and spec.loader
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)
serve = worker.serve


def gen(voice: str, text: str, dest: str) -> bytes:
    parts = [value.encode("utf-8") for value in (voice, text, dest)]
    return b"GEN " + b" ".join(str(len(part)).encode() for part in parts) + b"\n" + b"".join(parts)


class FakeModel:
    sample_rate = 24000

    def __init__(self) -> None:
        self.voices = 0
        self.generated = 0

    def get_state_for_audio_prompt(self, voice: Path, truncate: bool = True) -> str:
        assert truncate
        self.voices += 1
        return str(voice)

    def generate_audio_stream(self, state: str, text: str):
        self.generated += 1
        yield (state, text)


class DesktopWorkerTests(unittest.TestCase):
    def test_model_and_voice_are_reused(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            voice = Path(root) / "reference.wav"
            voice.write_bytes(b"fake audio")
            out_a, out_b = Path(root) / "a.wav", Path(root) / "b.wav"
            requests = gen(str(voice), "Hello", str(out_a)) + gen(
                str(voice), "Again", str(out_b)
            ) + b"QUIT\n"
            stream = io.BytesIO()
            created = []
            written = []

            def factory():
                model = FakeModel()
                created.append(model)
                return model

            def write(path, chunks, rate):
                self.assertEqual(rate, 24000)
                written.extend(chunks)
                path.write_bytes(b"WAV")

            serve(io.BytesIO(requests), stream, factory, write)
            self.assertEqual(stream.getvalue(), b"READY\nOK\nOK\n")
            self.assertEqual(len(created), 1)
            self.assertEqual(created[0].voices, 1)
            self.assertEqual(created[0].generated, 2)
            self.assertEqual(len(written), 2)
            self.assertEqual(out_a.read_bytes(), b"WAV")
            self.assertEqual(out_b.read_bytes(), b"WAV")

    def test_invalid_voice_rejected_before_model_loading(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            created = []
            stream = io.BytesIO()
            serve(
                io.BytesIO(gen("https://example.com/voice.wav", "Hello", str(Path(root) / "a.wav")) + b"QUIT\n"),
                stream,
                lambda: created.append(FakeModel()),
                lambda *_: None,
            )
            self.assertFalse(created)
            self.assertIn(b"ERR ", stream.getvalue())
            self.assertIn(b"absolute local", stream.getvalue())

    def test_truncated_ipc_request_is_error(self) -> None:
        stream = io.BytesIO()
        serve(io.BytesIO(b"GEN 10 20 30\nabc"), stream, FakeModel, lambda *_: None)
        self.assertTrue(stream.getvalue().startswith(b"READY\nERR "))

    def test_empty_text_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            voice = Path(root) / "reference.wav"
            voice.write_bytes(b"voice")
            stream = io.BytesIO()
            serve(
                io.BytesIO(gen(str(voice), " ", str(Path(root) / "a.wav")) + b"QUIT\n"),
                stream, FakeModel, lambda *_: None
            )
            self.assertIn(b"Enter text", stream.getvalue())

    def test_synthesis_error_does_not_publish_partial(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            voice = Path(root) / "reference.wav"
            voice.write_bytes(b"voice")
            out = Path(root) / "result.wav"
            stream = io.BytesIO()

            def broken_writer(path, chunks, rate):
                path.write_bytes(b"partial")
                raise RuntimeError("test exception")

            serve(
                io.BytesIO(gen(str(voice), "Hello", str(out)) + b"QUIT\n"),
                stream, FakeModel, broken_writer
            )
            self.assertIn(b"ERR ", stream.getvalue())
            self.assertFalse(out.exists())
            self.assertFalse((Path(root) / "result.part.wav").exists())


if __name__ == "__main__":
    unittest.main()
