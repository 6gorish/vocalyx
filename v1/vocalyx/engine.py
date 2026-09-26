"""
vocalyx.engine
~~~~~~~~~~~~~~

Core vocal transformation engine.

Uses Rubber Band v3+ R3 engine for high-quality pitch shifting with
independent formant control. The R3 engine's setFormantScale API allows
shifting pitch and formants by *different* amounts — the key to natural
voice transformation that preserves speaker identity.

Pipeline:
    1. Rubber Band pitch shift with partial formant shift
    2. Optional lightweight EQ post-processing (no STFT envelope manipulation)
"""

from __future__ import annotations

import dataclasses
import os
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import iirnotch, iirpeak

# ---------------------------------------------------------------------------
# Transform configuration
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class VocalTransform:
    """
    Configuration for a vocal transformation pass.

    Defaults tuned for moderate male→female spoken-word transformation.
    """

    # Pitch
    pitch_semitones: float = 4.0
    """Pitch shift in semitones. +3 to +5 for M→F speech."""

    # Formant shift (independent of pitch)
    formant_semitones: float = 1.5
    """Formant shift in semitones. Less than pitch_semitones to avoid chipmunk.
    0 = fully preserve original formants. Same as pitch = shift everything.
    Typical M→F: 1.0–2.5 st (partial shift toward female formant positions)."""

    # Post-processing EQ: list of (center_hz, gain_db, Q)
    eq_bands: list = dataclasses.field(default_factory=lambda: [
        (2800.0, -2.0, 1.5),   # Gentle presence cut
        (7000.0, 1.5, 1.0),    # Subtle air
    ])
    """Post-processing EQ bands as (center_hz, gain_db, Q)."""

    # Breathiness (subtle noise addition)
    breathiness: float = 0.0
    """HF noise blend (0.0–0.1). 0 = off. Very subtle amounts only."""

    breathiness_shelf_hz: float = 3000.0
    """Frequency above which breathiness noise ramps in."""

    # Rubber Band engine settings
    rb_engine: str = "r3"
    """Rubber Band engine: 'r3' (higher quality) or 'r2' (faster)."""


# ---------------------------------------------------------------------------
# Rubber Band pitch shifting with independent formant control
# ---------------------------------------------------------------------------

def _find_rubberband() -> str:
    """Find the rubberband CLI binary."""
    for name in ["rubberband", "rubberband-r3"]:
        try:
            result = subprocess.run(
                [name, "--version"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                return name
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue
    raise RuntimeError(
        "rubberband CLI not found. Install via: brew install rubberband"
    )


def pitch_shift_rb(
    x: np.ndarray,
    fs: int,
    pitch_semitones: float,
    formant_semitones: float = 0.0,
    engine: str = "r3",
) -> np.ndarray:
    """
    Pitch shift using Rubber Band CLI with independent formant control.

    Parameters
    ----------
    x : np.ndarray
        Mono audio signal.
    fs : int
        Sample rate.
    pitch_semitones : float
        Pitch shift in semitones.
    formant_semitones : float
        Independent formant shift in semitones. When different from
        pitch_semitones, RB shifts formants separately from pitch.
        0.0 = preserve original formants entirely.
    engine : str
        'r3' for higher quality, 'r2' for faster processing.
    """
    if abs(pitch_semitones) < 0.01 and abs(formant_semitones) < 0.01:
        return x.copy()

    rb_bin = _find_rubberband()

    # Build command
    cmd = [rb_bin]

    # Engine selection
    if engine == "r3":
        cmd.append("-3")
    else:
        cmd.append("-2")

    # Pitch shift
    cmd.extend(["-p", str(pitch_semitones)])

    # Formant handling
    # --formant preserves formants (equivalent to formant_scale = 1/pitch_ratio)
    # To get partial formant shift, we use --formant (preserve) and then
    # would need setFormantScale via the library API.
    #
    # Via CLI, we can approximate partial formant shift by:
    # - Using --formant to preserve formants during pitch shift
    # - Then doing a second pass with a small pitch shift (formant_semitones)
    #   WITHOUT --formant, which shifts formants along with pitch
    # - Then undoing that second pitch shift with --formant
    #
    # But that's hacky. A cleaner approach for the CLI:
    # If formant_semitones == 0: use --formant (full preservation)
    # If formant_semitones == pitch_semitones: no --formant (shift everything)
    # Otherwise: we need two passes.
    #
    # For now, support three modes and implement partial via two-pass.

    if abs(formant_semitones) < 0.01:
        # Full formant preservation
        cmd.append("--formant")
    elif abs(formant_semitones - pitch_semitones) < 0.01:
        # No formant preservation (formants shift with pitch)
        pass  # default RB behavior
    else:
        # Partial formant shift — two-pass approach:
        # Pass 1: shift pitch by full amount with formant preservation
        # Pass 2: shift pitch by formant_semitones WITHOUT preservation,
        #          then shift back by formant_semitones WITH preservation
        # Net effect: pitch shifted by pitch_semitones, formants by formant_semitones
        #
        # Actually simpler:
        # Pass 1: pitch shift by pitch_semitones, preserve formants
        # Pass 2: pitch shift by formant_semitones (formants follow), no preserve
        # Pass 3: pitch shift by -formant_semitones, preserve formants
        # Net: pitch = pitch_semitones + formant_semitones - formant_semitones = pitch_semitones
        #       formants = 0 + formant_semitones + 0 = formant_semitones ✓
        #
        # But three passes degrades quality. Let's use two:
        # Pass 1: pitch shift by (pitch_semitones - formant_semitones), preserve formants
        # Pass 2: pitch shift by formant_semitones, NO preserve (formants follow)
        # Net pitch: (pitch - formant) + formant = pitch_semitones ✓
        # Net formants: 0 + formant_semitones = formant_semitones ✓

        # Do two-pass
        fd1, tmpfile1 = tempfile.mkstemp(suffix=".wav")
        os.close(fd1)
        fd2, tmpfile2 = tempfile.mkstemp(suffix=".wav")
        os.close(fd2)
        fd3, tmpfile3 = tempfile.mkstemp(suffix=".wav")
        os.close(fd3)

        try:
            sf.write(tmpfile1, x, fs)

            preserve_amount = pitch_semitones - formant_semitones

            # Pass 1: shift pitch by (pitch - formant) with formant preservation
            cmd1 = [rb_bin]
            if engine == "r3":
                cmd1.append("-3")
            cmd1.extend(["-p", str(preserve_amount), "--formant", tmpfile1, tmpfile2])

            subprocess.run(cmd1, capture_output=True, check=True, timeout=120)

            # Pass 2: shift pitch by formant_semitones WITHOUT preservation
            cmd2 = [rb_bin]
            if engine == "r3":
                cmd2.append("-3")
            cmd2.extend(["-p", str(formant_semitones), tmpfile2, tmpfile3])

            subprocess.run(cmd2, capture_output=True, check=True, timeout=120)

            result, _ = sf.read(tmpfile3)
            if result.ndim > 1:
                result = result[:, 0]
            return result.astype(np.float64)

        finally:
            for f in [tmpfile1, tmpfile2, tmpfile3]:
                try:
                    os.unlink(f)
                except OSError:
                    pass

    # Single-pass path (full preserve or no preserve)
    fd_in, tmpfile_in = tempfile.mkstemp(suffix=".wav")
    os.close(fd_in)
    fd_out, tmpfile_out = tempfile.mkstemp(suffix=".wav")
    os.close(fd_out)

    try:
        sf.write(tmpfile_in, x, fs)
        cmd.extend([tmpfile_in, tmpfile_out])
        subprocess.run(cmd, capture_output=True, check=True, timeout=120)

        result, _ = sf.read(tmpfile_out)
        if result.ndim > 1:
            result = result[:, 0]
        return result.astype(np.float64)

    finally:
        for f in [tmpfile_in, tmpfile_out]:
            try:
                os.unlink(f)
            except OSError:
                pass


# ---------------------------------------------------------------------------
# Lightweight post-processing (biquad EQ, no STFT envelope manipulation)
# ---------------------------------------------------------------------------

def apply_eq(x: np.ndarray, fs: int, bands: list) -> np.ndarray:
    """
    Apply parametric EQ using cascaded biquad filters.

    Each band is (center_hz, gain_db, Q). Positive gain = boost,
    negative = cut. Uses scipy IIR peak/notch filters.
    """
    if not bands:
        return x

    from scipy.signal import lfilter

    y = x.copy()
    for center_hz, gain_db, q in bands:
        if abs(gain_db) < 0.1:
            continue

        w0 = center_hz / (fs / 2)
        if w0 >= 1.0 or w0 <= 0.0:
            continue

        if gain_db > 0:
            b, a = iirpeak(w0, q)
            linear_gain = 10.0 ** (gain_db / 20.0) - 1.0
            b_scaled = b * linear_gain
            b_scaled[0] += 1.0
            y = lfilter(b_scaled, a, y)
        else:
            b, a = iirnotch(w0, q)
            linear_atten = 1.0 - 10.0 ** (gain_db / 20.0)
            b_scaled = b * linear_atten
            b_scaled[0] += (1.0 - linear_atten)
            y = lfilter(b_scaled, a, y)

    return y


def add_breathiness(
    x: np.ndarray,
    fs: int,
    amount: float = 0.02,
    shelf_hz: float = 3000.0,
) -> np.ndarray:
    """
    Add subtle high-frequency noise to simulate breathiness.
    Uses a simple highpass-filtered noise blend.
    """
    if amount < 0.001:
        return x

    noise = np.random.randn(len(x))

    # Simple first-order highpass
    from scipy.signal import butter, lfilter
    w0 = shelf_hz / (fs / 2)
    if w0 >= 1.0:
        w0 = 0.95
    b, a = butter(2, w0, btype="high")
    noise_hp = lfilter(b, a, noise)

    # Match RMS to signal
    sig_rms = np.sqrt(np.mean(x ** 2)) + 1e-10
    noise_rms = np.sqrt(np.mean(noise_hp ** 2)) + 1e-10
    noise_hp = noise_hp * (sig_rms / noise_rms)

    return x + amount * noise_hp


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def process_formants(
    x: np.ndarray,
    fs: int,
    config: VocalTransform,
) -> np.ndarray:
    """
    Apply lightweight post-processing (EQ + breathiness).
    Kept as a named function for API compatibility.
    """
    y = apply_eq(x, fs, config.eq_bands)
    y = add_breathiness(y, fs, config.breathiness, config.breathiness_shelf_hz)
    return y


def transform_file(
    input_path: str,
    output_path: str,
    config: VocalTransform | None = None,
) -> dict:
    """
    Full pipeline: load → RB pitch shift with formant control → EQ → save.
    """
    if config is None:
        config = VocalTransform()

    input_path = Path(input_path)
    output_path = Path(output_path)

    # Load audio
    x, fs = sf.read(str(input_path))
    if x.ndim > 1:
        x = x[:, 0]
    x = x.astype(np.float64)
    original_length = len(x)

    # Step 1: Pitch shift via Rubber Band
    if abs(config.pitch_semitones) > 0.01:
        x = pitch_shift_rb(
            x, fs,
            pitch_semitones=config.pitch_semitones,
            formant_semitones=config.formant_semitones,
            engine=config.rb_engine,
        )

    # Step 2: Lightweight post-processing
    y = process_formants(x, fs, config)

    # Normalize
    peak = np.max(np.abs(y))
    if peak > 0:
        y = y / peak * 0.95

    # Save
    output_path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(output_path), y, fs)

    return {
        "input": str(input_path),
        "output": str(output_path),
        "sample_rate": fs,
        "duration_sec": original_length / fs,
        "config": dataclasses.asdict(config),
    }
