# SAM-Backed `lib-say`-Compatible TTS

This repository wraps the classic SAM synthesizer in a `lib-say`-style surface:

- CLI executable: `tts`
- Lua module: `say`
- Public audio contract: mono, 16-bit PCM, 44100 Hz
- Output formats: `raw`, `wav`, `aiff`
- Extra post-processing: `phone`, `gain`

The compatibility target is the API and integration model, not acoustic parity with the original `lib-say` backend.

## Build

The project now builds with CMake and includes a bundled Lua 5.4 runtime at the repo root.

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Expected outputs:

- `bin/tts.exe`
- `bin/lua/say.dll`
- `bin/lua/lua54.dll`
- `bin/lua/lua.exe`

The top-level `Makefile` is a thin wrapper around the same CMake flow.

## CLI

```text
tts <text-or-input-file> -o <output.{raw|aiff|wav}> [--lang en] [--rate 44100]
tts --phonemes "<sam-phoneme-string>" -o out.wav
tts "Stand by for incoming transmission." -o out.wav --phone --gain 2.0
tts input.txt -o narrator.wav --speed 80 --pitch 56 --mouth 140 --throat 110
tts "Debug me" --debug-report report.txt --dry-run
```

Supported flags:

- `-o`, `--output <path>`
- `--lang <en>`
- `--rate <44100>`
- `--frame-ms <5-10>`
- `--phonemes`
- `--debug-report <path|->`
- `--dry-run`
- `--gain <number>`
- `--phone`
- `--speed <0-255>`
- `--pitch <0-255>`
- `--mouth <0-255>`
- `--throat <0-255>`
- `--sing`
- `-h`, `--help`

Unsupported legacy flags fail explicitly:

- `--centralize`
- `--articulate`
- `--voice-formants`
- `--voice-pitch`
- `--amiga`

Input behavior:

- Without `--phonemes`, a single positional argument that resolves to an existing file is read as text input.
- Otherwise positional arguments are treated as literal input text.
- With `--phonemes`, the positional input is always treated as literal SAM phoneme text.

## Lua

```lua
local say = package.loadlib("bin/lua/say.dll", "luaopen_say")()

local blob, info = say.synthesize("Library test", {
    format = "wav",
    gain = 1.25,
    phone = true,
    speed = 80,
})

print(blob:GetData(), blob:GetSize())
print(info.format, info.sample_rate, info.byte_count)

local report = say.debug_report("Debug me", { frame_ms = 7 })
local defaults = say.default_options()
```

Exported module functions:

- `say.synthesize(input, options?) -> blob, info`
- `say.debug_report(input, options?) -> report`
- `say.default_options() -> table`

Exported constants:

- `say.LANG_EN`
- `say.FORMAT_RAW`
- `say.FORMAT_AIFF`
- `say.FORMAT_WAV`

Blob methods:

- `blob:GetData()`
- `blob:GetSize()`

## Options Mapping

Supported Lua/engine options:

- `lang` or `language`: `"en"` only
- `sample_rate` or `rate`: `44100` only
- `frame_ms`: `5..10`
- `phonemes`: boolean
- `format`: `"raw"`, `"aiff"`, `"wav"`
- `gain`: number greater than `0`
- `phone`: boolean
- `speed`: integer `0..255`
- `pitch`: integer `0..255`
- `mouth`: integer `0..255`
- `throat`: integer `0..255`
- `sing`: boolean

Engine behavior:

- SAM renders internally at 22050 Hz 8-bit mono.
- The wrapper converts to signed 16-bit PCM and upsamples 2x to 44100 Hz.
- Long text is chunked and concatenated with a short silence gap.
- `phone` is applied before `gain`.

## Phoneme Mode

`--phonemes` and `phonemes = true` use SAM phonemes and stress markers, not the `lib-say` phoneme set.

Examples of accepted symbols:

- Vowels: `IY`, `IH`, `EH`, `AE`, `AA`, `AH`, `AO`, `OH`, `UH`, `UX`
- Diphthongs: `EY`, `AY`, `OY`, `AW`, `OW`, `UW`
- Consonants: `R`, `L`, `W`, `Y`, `M`, `N`, `NX`, `B`, `D`, `G`, `S`, `SH`, `TH`, `CH`, `/H`
- Stress digits: `1` through `8`

## Debug Reports

`--debug-report` and `say.debug_report(...)` produce a human-readable artifact that includes:

- original input
- whether the input was treated as file or literal text
- normalized text or phoneme input
- reciter output for text mode
- final SAM phoneme strings
- effective SAM parameters
- selected output format
- effective sample rate
- post-processing flags
- chunking summary

## Legal Note

The bundled SAM code is based on a reverse-engineered upstream that does not ship with a standard open-source redistribution license. Treat this repository as an implementation and integration experiment unless you have cleared the redistribution model separately.
