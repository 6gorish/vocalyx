# Vocalyx

Shift the pitch of a spoken-word recording without making the speaker sound like a chipmunk.

Raising pitch normally drags the formants up with it — the vocal tract resonances that carry a
speaker's identity. Vocalyx moves pitch and formants by separate amounts, so a voice can be
raised several semitones while the formants move only part of the way. The result keeps more of
the original speaker's character than a plain pitch shift does.

Vocalyx is a thin Python layer over the [Rubber Band](https://breakfastquay.com/rubberband/)
command-line tool, plus parametric equalization and an optional noise blend for breathiness.

## Requirements

- Python 3.9 or later
- The `rubberband` command-line binary, version 3.0 or later, on your `PATH`

Vocalyx invokes `rubberband` as a subprocess rather than linking against the library. Version 3
is required because Vocalyx passes the `-3` flag to select the R3 engine.

```bash
# macOS
brew install rubberband

# Debian / Ubuntu
sudo apt install rubberband-cli
```

Confirm it works before installing Vocalyx:

```bash
rubberband --version
```

## Installation

```bash
git clone https://github.com/bjameshaskins/vocalyx.git
cd vocalyx

# Core dependencies only
pip install -e .

# With development tools (pytest, ruff) and spectrogram support (matplotlib)
pip install -e ".[dev]"

# Spectrogram support only
pip install -e ".[viz]"
```

## Quick start

```bash
# Default transformation: pitch +4 semitones, formants +1.5 semitones
vocalyx input.wav output.wav

# Stronger shift
vocalyx input.wav output.wav -p 5 --formant-st 2.0

# Pitch shift with formants fully preserved
vocalyx input.wav output.wav -p 4 --formant-st 0

# Shift pitch and formants together (the chipmunk effect, for comparison)
vocalyx input.wav output.wav -p 4 --formant-st 4

# Downward shift
vocalyx input.wav output.wav -p -4 --formant-st -1.5

# With a before/after spectrogram written alongside the output
vocalyx input.wav output.wav --spectrogram
```

## Command-line reference

```
vocalyx INPUT OUTPUT [OPTIONS]

Pitch & Formant Control:
  --pitch-semitones, -p ST    Pitch shift in semitones (default: 4.0)
  --formant-st ST             Formant shift in semitones (default: 1.5)

Post-processing EQ:
  --eq Hz:dB:Q                Equalization band; repeatable
                              (default: 2800:-2:1.5 and 7000:1.5:1)
  --no-eq                     Disable all equalization bands

Breathiness:
  --breathiness, -b AMOUNT    High-frequency noise blend, 0.0-0.1 (default: 0.0)
  --breathiness-shelf HZ      Frequency above which noise ramps in (default: 3000)

Engine:
  --engine {r2,r3}            Rubber Band engine; r3 is higher quality,
                              r2 is faster (default: r3)

Output:
  --spectrogram, -s           Write a before/after spectrogram as PNG
  --quiet, -q                 Suppress progress output
```

The input may be any format `libsndfile` reads — WAV, FLAC, AIFF, and others. The output format
is inferred from the output filename's extension.

## Python API

```python
from vocalyx import VocalTransform, transform_file

# Defaults
result = transform_file("input.wav", "output.wav")

# Custom configuration
config = VocalTransform(
    pitch_semitones=5.0,
    formant_semitones=2.0,
    eq_bands=[(2800.0, -2.0, 1.5), (7000.0, 1.5, 1.0)],
    breathiness=0.03,
    breathiness_shelf_hz=3000.0,
    rb_engine="r3",
)
result = transform_file("input.wav", "output.wav", config)

print(result["duration_sec"], result["sample_rate"])
```

`transform_file` returns a dictionary with the input and output paths, the sample rate, the
source duration in seconds, and the full configuration as a dictionary.

Lower-level functions are available from `vocalyx.engine`: `pitch_shift_rb`, `apply_eq`,
`add_breathiness`, and `process_formants`.

## How the formant shift works

Rubber Band's command-line interface offers a `--formant` flag that preserves formants during a
pitch shift, which is a binary choice: formants either follow the pitch entirely or stay put
entirely. Vocalyx needs positions in between, so it chains two passes.

Given a target pitch shift of P semitones and a target formant shift of F semitones:

1. **Pass one** shifts pitch by `P − F` with `--formant`, so formants stay where they were.
2. **Pass two** shifts pitch by `F` without `--formant`, so formants move with it.

Net pitch is `(P − F) + F = P`. Net formant movement is `0 + F = F`.

Two special cases skip the second pass:

- `--formant-st 0` runs a single pass with `--formant`.
- `--formant-st` equal to `--pitch-semitones` runs a single pass without it.

Because the default settings (P = 4.0, F = 1.5) fall outside both special cases, the default
path resamples the audio twice. This compounds Rubber Band's artifacts. If you hear smearing on
transients, try `--formant-st 0` to force the single-pass path and see whether the difference is
worth the loss of formant control.

## Post-processing

After the pitch shift, two optional stages run:

**Equalization.** Cascaded biquad filters built from SciPy's `iirpeak` and `iirnotch`. Each band
is a center frequency in hertz, a gain in decibels, and a Q factor controlling bandwidth — higher
Q is narrower. The defaults cut 2 dB at 2800 Hz and add 1.5 dB at 7000 Hz. Bands with gain under
0.1 dB are skipped.

**Breathiness.** Gaussian noise, high-passed with a second-order Butterworth filter at
`--breathiness-shelf`, scaled to match the signal's root-mean-square level, then blended in at
the given amount. Off by default. Values above roughly 0.1 sound obviously synthetic.

The output is then peak-normalized to 0.95 before writing. This happens unconditionally, so the
output level will not match the input level.

## Parameter guide

| Pitch | Formant | Effect |
|---|---|---|
| +3 to +5 | +1.0 to +2.5 | Masculine to feminine, speaker identity largely intact |
| +4 | 0 | Pitch raised, original formants; can sound thin |
| +4 | +4 | Everything shifted; the chipmunk artifact |
| −3 to −5 | −1.0 to −2.5 | Feminine to masculine |
| ±2 | ±0.5 | Subtle disguise |

Spoken word tolerates less pitch movement than singing does. Shifts beyond about 6 semitones in
either direction tend to reveal the processing regardless of formant handling.

## Known limitations

- **Formant-only shifts do not work.** `transform_file` skips the Rubber Band stage entirely when
  `pitch_semitones` is zero, so `-p 0 --formant-st 2` applies equalization and nothing else. Call
  `vocalyx.engine.pitch_shift_rb` directly as a workaround.
- **Mono only.** Multi-channel input is reduced to its first channel and the rest is discarded.
- **The default path runs two Rubber Band passes,** as described above.
- **No streaming.** The whole file is loaded into memory and written through temporary WAV files
  between passes, so very long recordings will be slow and memory-hungry.

## Development

```bash
pip install -e ".[dev]"
pytest
ruff check .
```

The test suite generates its audio with NumPy and commits no fixture files. Four of the tests
shell out to `rubberband` and will fail if the binary is missing.

## Roadmap

- [ ] Fix formant-only shifting
- [ ] Named presets with a `--preset` flag
- [ ] Stereo support
- [ ] Single-pass partial formant shift via the Rubber Band library's `setFormantScale`
- [ ] Batch processing
- [ ] Graphical interface
- [ ] Real-time monitoring

## Acknowledgments

[Rubber Band](https://breakfastquay.com/rubberband/) by Chris Cannam at Breakfast Quay, which
does the pitch and formant work.
