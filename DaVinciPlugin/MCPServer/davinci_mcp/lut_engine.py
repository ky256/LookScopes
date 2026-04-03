"""
3D LUT (.cube) generation engine.

Applies Lift / Gamma / Gain / Saturation / Contrast transforms to produce
an industry-standard .cube file compatible with DaVinci Resolve, UE, etc.
"""

import numpy as np
from pathlib import Path
from typing import Optional

_OUTPUT_DIR = Path(__file__).resolve().parent.parent / "output"


def generate_3d_lut(
    size: int = 33,
    lift: tuple[float, float, float] = (0.0, 0.0, 0.0),
    gamma: tuple[float, float, float] = (1.0, 1.0, 1.0),
    gain: tuple[float, float, float] = (1.0, 1.0, 1.0),
    saturation: float = 1.0,
    contrast: float = 1.0,
    output_name: str = "ai_grade",
    output_dir: Optional[Path] = None,
) -> Path:
    """
    Generate a 3D .cube LUT file.

    Parameters
    ----------
    size : int
        LUT resolution per axis (17, 33, or 65 typical).
    lift : (R, G, B)
        Shadow offset. 0 = neutral. Positive lifts blacks. Range ~ [-0.5, 0.5].
    gamma : (R, G, B)
        Midtone power. 1 = neutral, >1 brightens midtones, <1 darkens.
    gain : (R, G, B)
        Highlight multiplier. 1 = neutral, >1 brightens.
    saturation : float
        Global saturation. 1 = neutral, 0 = monochrome, >1 = vivid.
    contrast : float
        Pivot-0.5 contrast. 1 = neutral, >1 = punchier.
    output_name : str
        Filename without extension.
    output_dir : Path, optional
        Output directory. Defaults to MCPServer/output/.

    Returns
    -------
    Path
        Absolute path to the generated .cube file.
    """
    if output_dir is None:
        output_dir = _OUTPUT_DIR
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    out_path = output_dir / f"{output_name}.cube"

    steps = np.linspace(0.0, 1.0, size, dtype=np.float64)

    # .cube ordering: B outer (slowest), G middle, R inner (fastest)
    b_grid, g_grid, r_grid = np.meshgrid(steps, steps, steps, indexing="ij")
    rgb = np.stack([r_grid.ravel(), g_grid.ravel(), b_grid.ravel()], axis=-1)

    lift_a = np.array(lift, dtype=np.float64)
    gamma_a = np.array(gamma, dtype=np.float64)
    gain_a = np.array(gain, dtype=np.float64)

    # 1) Gain — scale (most visible in highlights)
    rgb *= gain_a

    # 2) Lift — shadow offset: out = in + lift * (1 - in)
    rgb += lift_a * (1.0 - rgb)

    # 3) Gamma — midtone power: out = in ^ (1 / gamma)
    np.clip(rgb, 0.0, None, out=rgb)
    safe_gamma = np.where(gamma_a > 1e-4, gamma_a, 1e-4)
    rgb = np.power(rgb, 1.0 / safe_gamma)

    # 4) Contrast — pivot at mid-gray
    if contrast != 1.0:
        rgb = (rgb - 0.5) * contrast + 0.5

    # 5) Saturation — Rec.709 luminance
    if saturation != 1.0:
        luma = (0.2126 * rgb[:, 0] + 0.7152 * rgb[:, 1] + 0.0722 * rgb[:, 2])[:, np.newaxis]
        rgb = luma + saturation * (rgb - luma)

    np.clip(rgb, 0.0, 1.0, out=rgb)

    with open(out_path, "w") as f:
        f.write('TITLE "AI Grade"\n')
        f.write(f"LUT_3D_SIZE {size}\n")
        f.write("DOMAIN_MIN 0.0 0.0 0.0\n")
        f.write("DOMAIN_MAX 1.0 1.0 1.0\n\n")
        np.savetxt(f, rgb, fmt="%.6f")

    return out_path.resolve()
