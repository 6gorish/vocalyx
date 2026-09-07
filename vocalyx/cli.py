"""
vocalyx.cli
~~~~~~~~~~~

Command-line interface for Vocalyx vocal transformation.

Usage examples:

    # Default transformation (M→F, pitch +4st, formants +1.5st)
    vocalyx input.wav output.wav

    # Custom pitch and formant shift
    vocalyx input.wav output.wav -p 5 --formant-st 2.0

    # Pitch shift only, no formant shift (like raw rubberband --formant)
    vocalyx input.wav output.wav -p 4 --formant-st 0

    # With spectrogram visualization
    vocalyx input.wav output.wav --spectrogram

    # No EQ post-processing
    vocalyx input.wav output.wav --no-eq
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from vocalyx.engine import VocalTransform, transform_file


def parse_eq_band(s: str) -> tuple:
    """Parse an EQ band string like '3000:-2.0:1.5' → (3000.0, -2.0, 1.5)."""
    parts = s.split(":")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(
            f"EQ band must be center_hz:gain_db:Q, got '{s}'"
        )
    try:
        return (float(parts[0]), float(parts[1]), float(parts[2]))
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"EQ band values must be numeric, got '{s}'"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="vocalyx",
        description=(
            "Vocalyx — Voice transformation preserving speaker identity.\n\n"
            "Uses Rubber Band for high-quality pitch shifting with independent\n"
            "formant control. Shifts pitch and formants by different amounts\n"
            "for natural-sounding voice transformation."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  vocalyx input.wav output.wav                    # default M→F\n"
            "  vocalyx input.wav output.wav -p 5 --formant-st 2 # stronger shift\n"
            "  vocalyx input.wav output.wav -p 4 --formant-st 0 # pitch only\n"
            "  vocalyx in.wav out.wav --spectrogram             # with visualization\n"
        ),
    )

    parser.add_argument(
        "input",
        type=str,
        help="Input audio file (WAV, FLAC, AIFF, etc.)",
    )
    parser.add_argument(
        "output",
        type=str,
        help="Output audio file (WAV)",
    )

    # -- Pitch & Formants --
    pitch = parser.add_argument_group("Pitch & Formant Control")
    pitch.add_argument(
        "--pitch-semitones", "-p",
        type=float,
        default=4.0,
        metavar="ST",
        help="Pitch shift in semitones. +3 to +5 for M→F. (default: 4.0)",
    )
    pitch.add_argument(
        "--formant-st",
        type=float,
        default=1.5,
        metavar="ST",
        help=(
            "Independent formant shift in semitones. Controls how much "
            "formants move relative to pitch. 0 = preserve original formants. "
            "Same as pitch = shift everything (chipmunk). "
            "Typical M→F: 1.0–2.5. (default: 1.5)"
        ),
    )

    # -- Post-processing EQ --
    eq = parser.add_argument_group("Post-processing EQ")
    eq.add_argument(
        "--eq",
        type=parse_eq_band,
        action="append",
        metavar="Hz:dB:Q",
        help=(
            "EQ band as center_hz:gain_db:Q. "
            "Can be repeated. (default: 2800:-2:1.5, 7000:1.5:1)"
        ),
    )
    eq.add_argument(
        "--no-eq",
        action="store_true",
        help="Disable all EQ bands.",
    )

    # -- Breathiness --
    breath = parser.add_argument_group("Breathiness")
    breath.add_argument(
        "--breathiness", "-b",
        type=float,
        default=0.0,
        metavar="AMOUNT",
        help="HF noise blend (0.0–0.1). Very subtle. (default: 0.0)",
    )
    breath.add_argument(
        "--breathiness-shelf",
        type=float,
        default=3000.0,
        metavar="HZ",
        help="Frequency above which breathiness ramps in. (default: 3000)",
    )

    # -- Engine --
    eng = parser.add_argument_group("Engine")
    eng.add_argument(
        "--engine",
        choices=["r2", "r3"],
        default="r3",
        help="Rubber Band engine. r3 = higher quality, r2 = faster. (default: r3)",
    )

    # -- Output options --
    output = parser.add_argument_group("Output")
    output.add_argument(
        "--spectrogram", "-s",
        action="store_true",
        help="Generate before/after spectrogram comparison (PNG).",
    )
    output.add_argument(
        "--quiet", "-q",
        action="store_true",
        help="Suppress progress output.",
    )

    return parser


def print_config(config: VocalTransform) -> None:
    """Print the active configuration."""
    if config.pitch_semitones > 0:
        direction = "M→F"
    elif config.pitch_semitones < 0:
        direction = "F→M"
    else:
        direction = "neutral"

    engine_label = config.rb_engine.upper()
    print(f"\n  ┌─ Vocalyx Transform ({direction}) [Rubber Band {engine_label}]")
    print("  │")
    print(f"  ├─ Pitch shift:     {config.pitch_semitones:+.1f} st")
    print(f"  ├─ Formant shift:   {config.formant_semitones:+.1f} st")
    print("  │")
    if config.eq_bands:
        print("  ├─ EQ bands:")
        for center, gain, q in config.eq_bands:
            print(f"  │    {center:.0f} Hz  {gain:+.1f} dB  Q={q:.1f}")
    else:
        print("  ├─ EQ bands:        (none)")
    print("  │")
    if config.breathiness > 0.001:
        print(
            f"  ├─ Breathiness:     {config.breathiness:.3f} "
            f"above {config.breathiness_shelf_hz:.0f} Hz"
        )
    else:
        print("  ├─ Breathiness:     off")
    print("  │")
    print(f"  └─ Engine:          Rubber Band {engine_label}")
    print()


def generate_spectrogram(
    input_path: str,
    output_path: str,
    image_path: str,
) -> None:
    """Generate a side-by-side spectrogram comparison."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import soundfile as sf
    except ImportError:
        print("  ⚠  matplotlib required for spectrograms: pip install matplotlib")
        return

    x_in, fs_in = sf.read(input_path)
    x_out, fs_out = sf.read(output_path)

    if x_in.ndim > 1:
        x_in = x_in[:, 0]
    if x_out.ndim > 1:
        x_out = x_out[:, 0]

    fig, axes = plt.subplots(2, 1, figsize=(14, 8), sharex=True)
    fig.suptitle("Vocalyx — Spectral Comparison (STFT)", fontsize=14, fontweight="bold")

    for ax, signal, label in [
        (axes[0], x_in, "Original"),
        (axes[1], x_out, "Transformed"),
    ]:
        ax.specgram(signal, Fs=fs_in, NFFT=2048, noverlap=1536, cmap="magma")
        ax.set_ylabel("Frequency (Hz)")
        ax.set_title(label, loc="left", fontsize=11)
        ax.set_ylim(0, 8000)

    axes[1].set_xlabel("Time (s)")
    plt.tight_layout()
    plt.savefig(image_path, dpi=150)
    plt.close()
    print(f"  📊 Spectrogram saved: {image_path}")


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    # Validate input
    input_path = Path(args.input)
    if not input_path.exists():
        print(f"Error: input file not found: {input_path}", file=sys.stderr)
        return 1

    # Resolve EQ bands
    if args.no_eq:
        eq_bands = []
    elif args.eq:
        eq_bands = list(args.eq)
    else:
        eq_bands = None  # Use defaults from VocalTransform

    # Build config
    kwargs = dict(
        pitch_semitones=args.pitch_semitones,
        formant_semitones=args.formant_st,
        breathiness=args.breathiness,
        breathiness_shelf_hz=args.breathiness_shelf,
        rb_engine=args.engine,
    )
    if eq_bands is not None:
        kwargs["eq_bands"] = eq_bands

    config = VocalTransform(**kwargs)

    if not args.quiet:
        print_config(config)
        print(f"  ⏳ Processing: {input_path}")

    t0 = time.time()
    result = transform_file(str(input_path), args.output, config)
    elapsed = time.time() - t0

    if not args.quiet:
        dur = result["duration_sec"]
        ratio = dur / elapsed if elapsed > 0 else float("inf")
        print(f"  ✓  Done in {elapsed:.1f}s ({ratio:.1f}x realtime)")
        print(f"  📁 Output: {result['output']}")

    # Spectrogram
    if args.spectrogram:
        out = Path(args.output)
        img_path = str(out.with_suffix(".png"))
        generate_spectrogram(args.input, args.output, img_path)

    return 0


if __name__ == "__main__":
    sys.exit(main())
