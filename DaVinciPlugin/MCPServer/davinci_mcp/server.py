"""
DaVinci Resolve AI Color Grading — MCP Server

Generates .cube LUT files that the OFX plugin inside DaVinci auto-loads.
No DaVinci scripting API required.
"""

from mcp.server.fastmcp import FastMCP

mcp = FastMCP(
    "davinci-mcp",
    instructions="DaVinci Resolve AI 调色 MCP — LUT 生成与调色控制",
)

_DEFAULT_LUT = "ai_grade"


@mcp.tool()
def generate_lut(
    lift_r: float = 0.0,
    lift_g: float = 0.0,
    lift_b: float = 0.0,
    gamma_r: float = 1.0,
    gamma_g: float = 1.0,
    gamma_b: float = 1.0,
    gain_r: float = 1.0,
    gain_g: float = 1.0,
    gain_b: float = 1.0,
    saturation: float = 1.0,
    contrast: float = 1.0,
    lut_size: int = 33,
    output_name: str = "ai_grade",
) -> str:
    """
    Generate a 3D .cube LUT from color grading parameters.
    The OFX plugin in DaVinci will auto-detect the file change and apply it.

    - Lift  : shadow offset   (0 = neutral, + lifts blacks)   per-channel
    - Gamma : midtone power   (1 = neutral, >1 brightens)     per-channel
    - Gain  : highlight scale  (1 = neutral, >1 brightens)    per-channel
    - Saturation : 1 = neutral, 0 = mono, >1 = vivid
    - Contrast   : 1 = neutral, >1 = punchier (pivot mid-gray)
    - lut_size   : resolution per axis (17 / 33 / 65)
    - output_name: filename without extension (default "ai_grade" — matches OFX default path)
    """
    from .lut_engine import generate_3d_lut

    path = generate_3d_lut(
        size=lut_size,
        lift=(lift_r, lift_g, lift_b),
        gamma=(gamma_r, gamma_g, gamma_b),
        gain=(gain_r, gain_g, gain_b),
        saturation=saturation,
        contrast=contrast,
        output_name=output_name,
    )
    return f"LUT 已生成: {path}\nOFX 插件将自动检测并加载。"


@mcp.tool()
def grade_and_preview(
    lift_r: float = 0.0,
    lift_g: float = 0.0,
    lift_b: float = 0.0,
    gamma_r: float = 1.0,
    gamma_g: float = 1.0,
    gamma_b: float = 1.0,
    gain_r: float = 1.0,
    gain_g: float = 1.0,
    gain_b: float = 1.0,
    saturation: float = 1.0,
    contrast: float = 1.0,
) -> str:
    """
    One-shot: generate LUT → OFX plugin auto-applies → DaVinci shows result.
    Writes to the default ai_grade.cube path that the OFX plugin watches.
    Same parameter semantics as generate_lut.
    """
    from .lut_engine import generate_3d_lut

    path = generate_3d_lut(
        size=33,
        lift=(lift_r, lift_g, lift_b),
        gamma=(gamma_r, gamma_g, gamma_b),
        gain=(gain_r, gain_g, gain_b),
        saturation=saturation,
        contrast=contrast,
        output_name=_DEFAULT_LUT,
    )
    return (
        f"LUT 已生成并写入默认路径: {path}\n"
        "DaVinci OFX 插件将自动检测文件变化并实时预览。"
    )


@mcp.tool()
def reset_grade() -> str:
    """
    Reset to identity LUT (no color change).
    Overwrites the default ai_grade.cube with a neutral transform.
    """
    from .lut_engine import generate_3d_lut

    path = generate_3d_lut(size=17, output_name=_DEFAULT_LUT)
    return f"已重置为中性 LUT: {path}"


@mcp.tool()
def get_lut_info() -> str:
    """
    Show info about the current LUT file on disk.
    """
    from .resolve_bridge import DEFAULT_LUT_PATH
    import os

    if not DEFAULT_LUT_PATH.exists():
        return "默认 LUT 文件不存在，尚未生成。"

    stat = os.stat(DEFAULT_LUT_PATH)
    size_kb = stat.st_size / 1024

    with open(DEFAULT_LUT_PATH, "r") as f:
        first_lines = [f.readline().strip() for _ in range(5)]

    info_lines = [
        f"路径: {DEFAULT_LUT_PATH}",
        f"大小: {size_kb:.1f} KB",
        f"修改时间: {os.path.getmtime(DEFAULT_LUT_PATH)}",
        "头部内容:",
    ] + [f"  {l}" for l in first_lines if l]
    return "\n".join(info_lines)


# ═══════════════════════════════════════════════════════════════
#  画面分析
# ═══════════════════════════════════════════════════════════════

@mcp.tool()
def analyze_viewport(source_name: str = "UE_LookScopes") -> str:
    """
    Capture one frame from the UE NDI stream and return color analysis data.
    Provides: resolution, average RGB, luminance range, shadow/midtone/highlight
    distribution, and per-channel histograms.
    Use this to understand the current image before making grading decisions.
    """
    from .ndi_capture import capture_frame, analyze_frame
    import json

    frame = capture_frame(source_name=source_name)
    if frame is None:
        return "无法捕获 NDI 帧 — 请确认 UE 正在推流且源名称正确。"

    analysis = analyze_frame(frame)
    lines = [
        f"分辨率: {analysis['resolution']}",
        f"平均 RGB: R={analysis['avg_r']}, G={analysis['avg_g']}, B={analysis['avg_b']}",
        f"平均亮度: {analysis['avg_luma']}",
        f"亮度范围: {analysis['min_luma']} ~ {analysis['max_luma']}",
        f"暗部占比: {analysis['shadows_pct']}%",
        f"中间调占比: {analysis['midtones_pct']}%",
        f"高光占比: {analysis['highlights_pct']}%",
        "",
        "直方图 (64 bins, 0→1):",
        f"  R: {analysis['hist_r']}",
        f"  G: {analysis['hist_g']}",
        f"  B: {analysis['hist_b']}",
        f"  Luma: {analysis['hist_luma']}",
    ]
    return "\n".join(lines)


@mcp.tool()
def save_viewport_snapshot(source_name: str = "UE_LookScopes") -> str:
    """
    Capture one frame from the UE NDI stream and save it as a BMP file.
    Returns the path to the saved snapshot image.
    """
    from .ndi_capture import capture_frame, save_snapshot

    frame = capture_frame(source_name=source_name)
    if frame is None:
        return "无法捕获 NDI 帧 — 请确认 UE 正在推流且源名称正确。"

    path = save_snapshot(frame)
    return f"截图已保存: {path} ({frame.shape[1]}x{frame.shape[0]})"
