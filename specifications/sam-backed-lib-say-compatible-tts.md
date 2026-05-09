# SAM-Backed lib-say-Compatible TTS

## Status

Draft specification.

## 1. Objective

Define a small Text-To-Speech library and command-line tool that keep the integration model of `lib-say` while replacing the synthesis backend with `SAM` (Software Automatic Mouth).

This project is intentionally:

- English only
- file-output oriented
- Lua-host friendly
- compatible with the current `lib-say` Lua protocol
- extended with a telephone filter and a post-synthesis gain stage

The main goal is integration compatibility, not acoustic identity with the current formant engine.

## 2. Upstream Basis

The implementation is based on the `s-macke/SAM` codebase and its core behavior:

- English text reciter
- English phonetic input mode
- classic SAM voice controls: `speed`, `pitch`, `mouth`, `throat`
- optional sing mode
- debug-oriented phoneme visibility

The wrapper must preserve the recognizable SAM character instead of trying to hide it behind heavy DSP.

## 3. Compatibility Targets

### 3.1 What must stay compatible with `lib-say`

- CLI shape: `tts <input> -o <output>`
- output formats: `raw`, `aiff`, `wav`
- Lua module name: `say`
- Lua call shape: `say.synthesize(input, options?) -> blob, info`
- Lua helper functions:
  - `say.debug_report(input, options?) -> report`
  - `say.default_options() -> table`
- blob interop methods:
  - `blob:GetData()`
  - `blob:GetSize()`
- post-processing options:
  - `gain`
  - `phone`

### 3.2 What is intentionally different

- language support is limited to English
- the synthesis backend is SAM, not the current `lib-say` formant engine
- SAM-native voice controls replace the formant-engine controls that do not map cleanly
- no Amiga synthesis path

### 3.3 Compatibility mapping

| `lib-say` surface | SAM-backed behavior |
| --- | --- |
| `--lang`, `lang`, `language` | only `en` is accepted |
| `--rate`, `sample_rate`, `rate` | fixed public output at `44100` |
| `--frame-ms`, `frame_ms` | accepted for protocol compatibility, metadata only |
| `--phonemes`, `phonemes` | switches to SAM phonetic mode |
| `--gain`, `gain` | preserved |
| `--phone`, `phone` | preserved |
| `--amiga`, `amiga` | unsupported |
| `--centralize`, `--articulate`, `--voice-formants`, `--voice-pitch` | removed |
| `--speed`, `--pitch`, `--mouth`, `--throat`, `--sing` | added as SAM-native controls |

## 4. Non-Goals

- French support
- neural synthesis
- acoustic parity with the existing `lib-say` engine
- real-time speaker streaming API in v1
- preserving unsupported engine-specific flags through silent no-op behavior

## 5. Public Audio Contract

### 5.1 Internal rendering

- SAM runs at its native 22050 Hz mono output rate.
- SAM's native 8-bit output is treated as an internal format only.

### 5.2 Public output

The public contract must match modern `lib-say` expectations:

- mono
- signed 16-bit PCM
- fixed effective sample rate: `44100 Hz`

The wrapper must therefore:

1. render with SAM at 22050 Hz
2. convert to signed PCM
3. upsample 2x to 44100 Hz
4. run optional post-effects
5. encode the requested container

### 5.3 Encoded formats

- `raw`: headerless `s16le`
- `wav`: PCM WAV, mono, 16-bit, 44100 Hz
- `aiff`: PCM AIFF, mono, 16-bit, 44100 Hz, big-endian payload

## 6. Engine Limits And Wrapper Responsibilities

The wrapper must not expose SAM's native implementation limits as user-visible product limits.

Known SAM constraints include:

- short fixed-size text buffers
- output buffer assumptions sized for short clips
- parameter storage based on 8-bit values

Therefore:

- text mode must support long inputs by chunking and concatenation
- the wrapper must allocate output dynamically
- out-of-range numeric controls must be rejected explicitly, not wrapped modulo 256

### 6.1 Text chunking

For plain text input:

- split on sentence punctuation first
- fall back to whitespace boundaries if needed
- keep each SAM segment comfortably below the native input limit
- concatenate rendered segments with a short silence gap of 20 to 40 ms

### 6.2 Phoneme mode limit

For phoneme input in v1:

- a single phoneme segment may be limited to one SAM-safe chunk
- overly long phoneme strings may be rejected with a clear error instead of being auto-segmented

This restriction is acceptable in v1 because phoneme mode is an expert path.

## 7. CLI Specification

### 7.1 Executable

The executable name remains `tts`.

### 7.2 Usage

```text
tts <text-or-input-file> -o <output.{raw|aiff|wav}> [--lang en] [--rate 44100]
tts --phonemes "<sam-phoneme-string>" -o out.wav
tts "Stand by for incoming transmission." -o out.wav --phone --gain 2.0
tts input.txt -o narrator.wav --speed 80 --pitch 56 --mouth 140 --throat 110
tts "Debug me" --debug-report report.txt --dry-run
```

### 7.3 Input behavior

- If `--phonemes` is absent and the first positional argument resolves to an existing file, the file contents are used as text input.
- Otherwise the first positional argument is treated as literal input text.
- In `--phonemes` mode the first positional argument is always treated as literal phoneme text.

### 7.4 Required flags

- `-o`, `--output <path>`

`--output` is optional only when `--dry-run` is active.

The output format is inferred from the file extension:

- `.raw`
- `.wav`
- `.aiff`

### 7.5 Supported flags

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

### 7.6 Flag semantics

- `--lang` accepts only `en`. Any other value is an error.
- `--rate` is kept for protocol compatibility and accepts only `44100`.
- `--frame-ms` is accepted for compatibility with `lib-say` and is echoed in metadata and debug reports, but it does not alter SAM's internal acoustic model.
- `--phonemes` switches input parsing from text reciter mode to SAM phonetic mode.
- `--debug-report` writes a human-readable report. `-` means stdout.
- `--dry-run` builds the debug path and validates synthesis inputs without writing audio.
- `--gain` applies post-synthesis linear gain with soft-knee limiting.
- `--phone` applies the telephone effect after synthesis and before gain.
- `--speed`, `--pitch`, `--mouth`, `--throat`, and `--sing` map directly to SAM-native controls.

### 7.7 Unsupported legacy flags

The following current `lib-say` CLI flags are not part of this product and must fail with a clear message:

- `--centralize`
- `--articulate`
- `--voice-formants`
- `--voice-pitch`
- `--amiga`

The error text should explicitly state that the flag is unsupported by the SAM backend.

## 8. Lua Specification

### 8.1 Module name

The Lua extension module name remains `say`.

### 8.2 Exported functions

```lua
blob, info = say.synthesize(input, options)
report = say.debug_report(input, options)
defaults = say.default_options()
```

### 8.3 Exported constants

- `say.LANG_EN`
- `say.FORMAT_RAW`
- `say.FORMAT_AIFF`
- `say.FORMAT_WAV`

`LANG_FR` is not exported.

### 8.4 `say.synthesize(input, options?) -> blob, info`

`blob` is a userdata that owns the encoded bytes.

`info` is a Lua table with the same structural contract as `lib-say`:

- `language`
- `format`
- `sample_rate`
- `frame_ms`
- `phonemes`
- `channels`
- `bits_per_sample`
- `sample_count`
- `byte_count`
- `duration_seconds`
- `pcm_encoding`

Expected fixed values:

- `language = "en"`
- `sample_rate = 44100`
- `channels = 1`
- `bits_per_sample = 16`

`pcm_encoding` rules:

- `aiff` -> `"s16be"`
- `raw` and `wav` -> `"s16le"`

### 8.5 Lua options

Supported option keys:

- `lang` or `language`: `"en"` only
- `sample_rate` or `rate`: `44100` only
- `frame_ms`: integer `5..10`, compatibility metadata only
- `phonemes`: boolean
- `format`: `"raw"`, `"aiff"`, or `"wav"`
- `gain`: number greater than `0`
- `phone`: boolean
- `speed`: integer `0..255`
- `pitch`: integer `0..255`
- `mouth`: integer `0..255`
- `throat`: integer `0..255`
- `sing`: boolean

Compatibility behavior:

- unknown keys are ignored
- `amiga = false` may be ignored for compatibility
- `amiga = true` must raise an error

### 8.6 `say.default_options()`

The function returns a Lua table with at least:

```lua
{
    language = "en",
    sample_rate = 44100,
    frame_ms = 5,
    phonemes = false,
    format = "raw",
    speed = 72,
    pitch = 64,
    mouth = 128,
    throat = 128,
    sing = false,
}
```

### 8.7 Blob methods

- `blob:GetData()` returns the data pointer as an integer
- `blob:GetSize()` returns the byte size

The lifetime contract matches `lib-say`: the pointer remains valid while the owning userdata remains alive.

## 9. SAM Phoneme Mode

`--phonemes` and `phonemes = true` use SAM's phonetic alphabet and stress markers, not the phoneme set currently used by `lib-say`.

The implementation must document the accepted SAM symbols, including:

- vowels such as `IY`, `IH`, `EH`, `AE`, `AA`, `AH`, `AO`, `OH`, `UH`, `UX`
- diphthongs such as `EY`, `AY`, `OY`, `AW`, `OW`, `UW`
- consonants such as `R`, `L`, `W`, `Y`, `M`, `N`, `NX`, `B`, `D`, `G`, `S`, `SH`, `TH`, `CH`, `/H`
- stress digits accepted by SAM

This phoneme mode is an intentional compatibility break at the linguistic layer, but not at the API layer.

## 10. Post-Processing

### 10.1 Gain

`gain` matches the current `lib-say` output-side behavior:

- linear gain on the 16-bit PCM buffer
- `1.0` means unchanged
- values below `1.0` attenuate linearly
- values above unity pass through a soft-knee limiter
- the limiter knee is fixed near `-3 dBFS`
- the limiter must avoid hard clipping under normal use

### 10.2 Telephone filter

`phone` matches the current `lib-say` effect concept:

- band-limited telephone sound
- approximately 500 to 2800 Hz passband
- steep skirts
- noticeable midrange presence boost
- light saturation or codec-like crunch

The effect must be applied to the 44100 Hz post-upsample PCM buffer.

### 10.3 Processing order

When both effects are enabled:

1. synthesize with SAM
2. convert and upsample
3. apply `phone`
4. apply `gain`
5. encode output

## 11. Debug Report

The debug report is a human-readable text artifact, not a stable machine protocol.

It should contain, when available:

- original input
- whether the input path was treated as file or literal text
- normalized text passed to SAM
- phoneme mode vs text mode
- reciter output for text mode
- final SAM phoneme string
- effective SAM parameters
- selected output format
- effective sample rate
- enabled post-processing flags
- chunking summary for long text

In `--dry-run` mode and in `say.debug_report(...)`, no audio file is written.

## 12. Error Handling

### 12.1 CLI

The CLI must exit non-zero on:

- missing input
- missing output path when not in dry-run mode
- unsupported language
- unsupported sample rate
- unsupported output extension
- invalid numeric range
- invalid phoneme input
- unsupported legacy flag
- I/O failure
- synthesis failure

Errors must be short, explicit, and actionable.

### 12.2 Lua

Lua failures must raise a regular Lua error with a plain-text message.

Examples:

- unsupported language
- `gain <= 0`
- `amiga = true`
- out-of-range SAM parameter
- encoding failure

## 13. Testing

Minimum test coverage:

- CLI smoke tests for `raw`, `wav`, and `aiff`
- Lua smoke tests for `say.synthesize`, `say.debug_report`, and `say.default_options`
- regression tests for:
  - default voice
  - `--phone`
  - `--gain`
  - phoneme mode
  - `--sing`
  - non-default `speed`, `pitch`, `mouth`, `throat`
- metadata tests for the `info` table
- pointer lifetime test for `blob`
- long-text chunking test
- invalid-flag and invalid-range tests

Audio regression may use byte snapshots, hashes, or tolerant waveform comparisons, as long as the method is deterministic on a fixed build target.

## 14. Packaging

Expected build outputs:

- `bin/tts.exe`
- `bin/lua/say.dll`
- `bin/lua/lua54.dll` when using the bundled Lua build

The Lua module must remain usable with the same pointer-and-size interop pattern as the current `lib-say` binding.

## 15. Legal And Provenance Constraint

This project cannot ignore the legal status of the chosen upstream.

The upstream `SAM` repository describes the code as reverse-engineered and does not provide a standard open-source redistribution license. That makes redistribution a product risk and potentially a release blocker.

This specification therefore requires one of the following before public distribution:

1. legal approval for the chosen use and distribution model
2. a user-supplied-source workflow that keeps the upstream out of distributed binaries
3. a clean-room or otherwise redistributable SAM-compatible backend

If none of these conditions is met, the implementation may still be useful for internal experimentation, but it must not be treated as safely redistributable software.
