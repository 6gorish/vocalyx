"""
Vocalyx — pitch shifting with independent formant control for spoken word.

Wraps the Rubber Band command-line tool to shift pitch and formants by
separate amounts, so a voice can be raised or lowered several semitones
while the formants move only part of the way. Adds parametric equalization
and an optional high-frequency noise blend for breathiness.
"""

from vocalyx.engine import VocalTransform, transform_file

__version__ = "0.2.0"

__all__ = ["VocalTransform", "transform_file", "__version__"]
