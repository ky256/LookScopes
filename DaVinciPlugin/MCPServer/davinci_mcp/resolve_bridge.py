"""
DaVinci Resolve bridge — file-based mode.

The OFX plugin inside DaVinci auto-detects file changes and reloads the LUT.
No scripting API needed.
"""

from pathlib import Path

_OUTPUT_DIR = Path(__file__).resolve().parent.parent / "output"

# Default LUT file that the OFX plugin watches
DEFAULT_LUT_PATH = _OUTPUT_DIR / "ai_grade.cube"


def get_lut_output_path(name: str = "ai_grade") -> Path:
    _OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    return _OUTPUT_DIR / f"{name}.cube"
