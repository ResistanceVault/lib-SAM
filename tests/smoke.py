from __future__ import annotations

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(
    repo: Path,
    *args: str,
    expect_ok: bool = True,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        list(args),
        cwd=repo,
        capture_output=True,
        text=True,
        input=input_text,
        check=False,
    )
    if expect_ok and completed.returncode != 0:
        raise AssertionError(
            f"command failed: {' '.join(args)}\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    if not expect_ok and completed.returncode == 0:
        raise AssertionError(f"command unexpectedly succeeded: {' '.join(args)}")
    return completed


def assert_wav_header(path: Path) -> None:
    data = path.read_bytes()
    assert data[:4] == b"RIFF"
    assert data[8:12] == b"WAVE"
    assert data[12:16] == b"fmt "
    assert struct.unpack_from("<H", data, 22)[0] == 1
    assert struct.unpack_from("<I", data, 24)[0] == 44100
    assert struct.unpack_from("<H", data, 34)[0] == 16


def assert_aiff_header(path: Path) -> None:
    data = path.read_bytes()
    assert data[:4] == b"FORM"
    assert data[8:12] == b"AIFF"
    assert data[12:16] == b"COMM"
    assert data[38:42] == b"SSND"


def main() -> int:
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd().resolve()
    tts = repo / "bin" / "tts.exe"
    lua = repo / "bin" / "lua" / "lua.exe"
    say = repo / "bin" / "lua" / "say.dll"

    assert tts.exists(), tts
    assert lua.exists(), lua
    assert say.exists(), say

    with tempfile.TemporaryDirectory() as temp_dir:
        temp = Path(temp_dir)
        wav_path = temp / "smoke.wav"
        raw_path = temp / "smoke.raw"
        aiff_path = temp / "smoke.aiff"

        run(repo, str(tts), "Stand by for incoming transmission.", "-o", str(wav_path))
        run(repo, str(tts), "--phonemes", "DHIHS IHZ AH TEH5ST.", "-o", str(raw_path))
        run(repo, str(tts), "Phone line test", "-o", str(aiff_path), "--phone", "--gain", "2.0")

        assert wav_path.stat().st_size > 44
        assert raw_path.stat().st_size > 0
        assert aiff_path.stat().st_size > 54
        assert_wav_header(wav_path)
        assert_aiff_header(aiff_path)

        debug = run(repo, str(tts), "Debug me", "--debug-report", "-", "--dry-run")
        assert "Chunk 1" in debug.stdout
        assert "Selected output format: raw" in debug.stdout

        numbers = {
            "10": "ten",
            "21": "twenty one",
            "105": "one hundred five",
            "2026": "two thousand twenty six",
            "9999": "nine thousand nine hundred ninety nine",
        }
        for raw, expanded in numbers.items():
            report = run(repo, str(tts), raw, "--debug-report", "-", "--dry-run")
            assert f"Normalized text: {expanded}" in report.stdout

        long_text = " ".join(["This is a chunking regression test sentence."] * 40)
        long_debug = run(repo, str(tts), long_text, "--debug-report", "-", "--dry-run")
        assert "Chunk 2" in long_debug.stdout

        invalid_flag = run(repo, str(tts), "hello", "--amiga", "-o", str(wav_path), expect_ok=False)
        assert "unsupported by the SAM backend" in invalid_flag.stderr

        invalid_range = run(repo, str(tts), "hello", "--pitch", "999", "-o", str(wav_path), expect_ok=False)
        assert "pitch must be between 0 and 255" in invalid_range.stderr

        lua_script = r"""
local loader, err = package.loadlib('.\\bin\\lua\\say.dll', 'luaopen_say')
assert(loader, err)
local say = loader()
local blob, info = say.synthesize('Library test', { format = 'wav', phone = true, gain = 1.25, speed = 80 })
assert(type(blob:GetData()) == 'number' and blob:GetData() ~= 0)
assert(blob:GetSize() == info.byte_count)
assert(info.language == 'en')
assert(info.format == 'wav')
assert(info.sample_rate == 44100)
assert(info.channels == 1)
assert(info.bits_per_sample == 16)
local report = say.debug_report('Debug library', { frame_ms = 7 })
assert(report:find('Chunk 1', 1, true))
assert(say.debug_report('2026'):find('Normalized text: two thousand twenty six', 1, true))
local defaults = say.default_options()
assert(defaults.language == 'en')
assert(defaults.sample_rate == 44100)
assert(defaults.format == 'raw')
"""
        run(repo, str(lua), "-", input_text=lua_script)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
