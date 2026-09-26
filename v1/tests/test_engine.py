"""
Tests for the vocal transformation engine.
"""

import numpy as np


def test_eq_passthrough():
    """No EQ bands should return input unchanged."""
    from vocalyx.engine import apply_eq

    x = np.random.randn(8000)
    y = apply_eq(x, 16000, bands=[])
    np.testing.assert_array_equal(y, x)


def test_breathiness_off():
    """Zero breathiness should return input unchanged."""
    from vocalyx.engine import add_breathiness

    x = np.random.randn(8000)
    y = add_breathiness(x, 16000, amount=0.0)
    np.testing.assert_array_equal(y, x)


def test_breathiness_adds_energy():
    """Non-zero breathiness should increase total energy."""
    from vocalyx.engine import add_breathiness

    np.random.seed(42)
    x = np.sin(2 * np.pi * 200 * np.arange(16000) / 16000)
    y = add_breathiness(x, 16000, amount=0.05, shelf_hz=2000.0)

    # Should have more energy above shelf
    from scipy.fft import rfft
    spec_x = np.abs(rfft(x))
    spec_y = np.abs(rfft(y))

    # HF bins (above 2000 Hz) should have more energy
    hf_start = int(2000 / 8000 * len(spec_x))
    assert np.sum(spec_y[hf_start:] ** 2) > np.sum(spec_x[hf_start:] ** 2)


def test_config_defaults():
    """Default config should have sensible values."""
    from vocalyx.engine import VocalTransform

    config = VocalTransform()
    assert config.pitch_semitones == 4.0
    assert config.formant_semitones == 1.5
    assert config.rb_engine == "r3"
    assert len(config.eq_bands) > 0


def test_process_formants_passthrough():
    """Process formants with no EQ and no breathiness should be near-identity."""
    from vocalyx.engine import VocalTransform, process_formants

    config = VocalTransform(eq_bands=[], breathiness=0.0)
    x = np.random.randn(8000)
    y = process_formants(x, 16000, config)
    np.testing.assert_array_equal(y, x)


def test_pitch_shift_rb_identity():
    """Zero pitch shift should return input unchanged."""
    from vocalyx.engine import pitch_shift_rb

    x = np.random.randn(16000)
    y = pitch_shift_rb(x, 16000, 0.0, 0.0)
    np.testing.assert_array_equal(y, x)


def test_pitch_shift_rb_changes_signal():
    """Non-zero pitch shift should produce a different signal."""
    from vocalyx.engine import pitch_shift_rb

    np.random.seed(42)
    t = np.arange(16000) / 16000.0
    x = np.sin(2 * np.pi * 200 * t)

    y = pitch_shift_rb(x, 16000, 4.0, 0.0)

    # Should be different
    assert not np.allclose(x[:len(y)], y[:len(x)], atol=0.1)


def test_pitch_shift_rb_preserves_length_approx():
    """Pitch-shifted output should be approximately same length."""
    from vocalyx.engine import pitch_shift_rb

    np.random.seed(42)
    x = np.random.randn(44100)
    y = pitch_shift_rb(x, 44100, 4.0, 0.0)

    # Allow 5% tolerance on length
    assert abs(len(y) - len(x)) / len(x) < 0.05
