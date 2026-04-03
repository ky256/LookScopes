"""
NDI frame capture and analysis.

Receives a single frame from the UE NDI stream using the NDI SDK via ctypes,
then computes color analysis data for AI decision-making.
"""

import ctypes
import ctypes.wintypes
import time
import os
import numpy as np
from pathlib import Path
from typing import Optional

_OUTPUT_DIR = Path(__file__).resolve().parent.parent / "output"

# NDI SDK function signatures (minimal subset for receive)
_ndi_lib = None
_ndi_api = None


def _find_ndi_dll() -> str:
    candidates = [
        r"C:\Program Files\Common Files\OFX\Plugins\NDIReceiverPlugin.ofx.bundle\Contents\Win64\Processing.NDI.Lib.x64.dll",
        r"C:\Program Files\Epic Games\UE_5.7\Engine\Plugins\Media\NDIMedia\Binaries\ThirdParty\Win64\Processing.NDI.Lib.x64.dll",
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    raise FileNotFoundError("Processing.NDI.Lib.x64.dll not found")


class NDIlib_source_t(ctypes.Structure):
    _fields_ = [
        ("p_ndi_name", ctypes.c_char_p),
        ("p_url_address", ctypes.c_char_p),
    ]


class NDIlib_find_create_t(ctypes.Structure):
    _fields_ = [
        ("show_local_sources", ctypes.c_bool),
        ("p_groups", ctypes.c_char_p),
        ("p_extra_ips", ctypes.c_char_p),
    ]


class NDIlib_recv_create_v3_t(ctypes.Structure):
    _fields_ = [
        ("source_to_connect_to", NDIlib_source_t),
        ("color_format", ctypes.c_int),
        ("bandwidth", ctypes.c_int),
        ("allow_video_fields", ctypes.c_bool),
        ("p_ndi_recv_name", ctypes.c_char_p),
    ]


class NDIlib_video_frame_v2_t(ctypes.Structure):
    _fields_ = [
        ("xres", ctypes.c_int),
        ("yres", ctypes.c_int),
        ("FourCC", ctypes.c_int),
        ("frame_rate_N", ctypes.c_int),
        ("frame_rate_D", ctypes.c_int),
        ("picture_aspect_ratio", ctypes.c_float),
        ("frame_format_type", ctypes.c_int),
        ("timecode", ctypes.c_int64),
        ("p_data", ctypes.c_void_p),
        ("line_stride_in_bytes", ctypes.c_int),
        ("p_metadata", ctypes.c_char_p),
        ("timestamp", ctypes.c_int64),
    ]


class NDIlib_audio_frame_v2_t(ctypes.Structure):
    _fields_ = [
        ("sample_rate", ctypes.c_int),
        ("no_channels", ctypes.c_int),
        ("no_samples", ctypes.c_int),
        ("timecode", ctypes.c_int64),
        ("p_data", ctypes.c_void_p),
        ("channel_stride_in_bytes", ctypes.c_int),
        ("p_metadata", ctypes.c_char_p),
        ("timestamp", ctypes.c_int64),
    ]


class NDIlib_metadata_frame_t(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_int),
        ("timecode", ctypes.c_int64),
        ("p_data", ctypes.c_char_p),
    ]


# NDI constants
NDILIB_RECV_COLOR_FORMAT_RGBX_RGBA = 1
NDILIB_RECV_BANDWIDTH_HIGHEST = 0
NDILIB_FRAME_TYPE_VIDEO = 1


def _load_ndi():
    global _ndi_lib
    if _ndi_lib is not None:
        return _ndi_lib

    dll_path = _find_ndi_dll()
    _ndi_lib = ctypes.CDLL(dll_path)

    # NDIlib_v5_load returns a pointer to a struct of function pointers.
    # For simplicity, we'll use the flat C API functions directly.
    _ndi_lib.NDIlib_initialize.restype = ctypes.c_bool
    _ndi_lib.NDIlib_initialize.argtypes = []

    _ndi_lib.NDIlib_find_create_v2.restype = ctypes.c_void_p
    _ndi_lib.NDIlib_find_create_v2.argtypes = [ctypes.POINTER(NDIlib_find_create_t)]

    _ndi_lib.NDIlib_find_wait_for_sources.restype = ctypes.c_bool
    _ndi_lib.NDIlib_find_wait_for_sources.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    _ndi_lib.NDIlib_find_get_current_sources.restype = ctypes.POINTER(NDIlib_source_t)
    _ndi_lib.NDIlib_find_get_current_sources.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]

    _ndi_lib.NDIlib_find_destroy.restype = None
    _ndi_lib.NDIlib_find_destroy.argtypes = [ctypes.c_void_p]

    _ndi_lib.NDIlib_recv_create_v3.restype = ctypes.c_void_p
    _ndi_lib.NDIlib_recv_create_v3.argtypes = [ctypes.POINTER(NDIlib_recv_create_v3_t)]

    _ndi_lib.NDIlib_recv_capture_v2.restype = ctypes.c_int
    _ndi_lib.NDIlib_recv_capture_v2.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(NDIlib_video_frame_v2_t),
        ctypes.POINTER(NDIlib_audio_frame_v2_t),
        ctypes.POINTER(NDIlib_metadata_frame_t),
        ctypes.c_uint32,
    ]

    _ndi_lib.NDIlib_recv_free_video_v2.restype = None
    _ndi_lib.NDIlib_recv_free_video_v2.argtypes = [ctypes.c_void_p, ctypes.POINTER(NDIlib_video_frame_v2_t)]

    _ndi_lib.NDIlib_recv_destroy.restype = None
    _ndi_lib.NDIlib_recv_destroy.argtypes = [ctypes.c_void_p]

    _ndi_lib.NDIlib_destroy.restype = None
    _ndi_lib.NDIlib_destroy.argtypes = []

    if not _ndi_lib.NDIlib_initialize():
        raise RuntimeError("NDIlib_initialize failed")

    return _ndi_lib


def capture_frame(source_name: str = "UE_LookScopes", timeout_ms: int = 5000) -> Optional[np.ndarray]:
    """
    Capture a single RGBA frame from the named NDI source.
    Returns numpy array (H, W, 4) dtype uint8, or None if not found.
    """
    lib = _load_ndi()

    find_create = NDIlib_find_create_t()
    find_create.show_local_sources = True
    find_create.p_groups = None
    find_create.p_extra_ips = None
    finder = lib.NDIlib_find_create_v2(ctypes.byref(find_create))
    if not finder:
        return None

    try:
        lib.NDIlib_find_wait_for_sources(finder, 2000)
        num = ctypes.c_uint32(0)
        sources_ptr = lib.NDIlib_find_get_current_sources(finder, ctypes.byref(num))

        target_src = None
        for i in range(num.value):
            name = sources_ptr[i].p_ndi_name
            if name and source_name.encode() in name:
                target_src = sources_ptr[i]
                break

        if target_src is None:
            return None

        recv_create = NDIlib_recv_create_v3_t()
        recv_create.source_to_connect_to = target_src
        recv_create.color_format = NDILIB_RECV_COLOR_FORMAT_RGBX_RGBA
        recv_create.bandwidth = NDILIB_RECV_BANDWIDTH_HIGHEST
        recv_create.allow_video_fields = False
        recv_create.p_ndi_recv_name = b"MCP_Analyzer"
        recv = lib.NDIlib_recv_create_v3(ctypes.byref(recv_create))
        if not recv:
            return None

        try:
            video = NDIlib_video_frame_v2_t()
            audio = NDIlib_audio_frame_v2_t()
            meta = NDIlib_metadata_frame_t()

            deadline = time.time() + timeout_ms / 1000.0
            while time.time() < deadline:
                ft = lib.NDIlib_recv_capture_v2(
                    recv,
                    ctypes.byref(video),
                    ctypes.byref(audio),
                    ctypes.byref(meta),
                    500,
                )
                if ft == NDILIB_FRAME_TYPE_VIDEO:
                    w, h = video.xres, video.yres
                    stride = video.line_stride_in_bytes
                    if stride <= 0:
                        stride = w * 4

                    total_bytes = h * stride
                    buf = (ctypes.c_uint8 * total_bytes).from_address(video.p_data)
                    raw = np.frombuffer(buf, dtype=np.uint8).copy()

                    # Determine actual pixel width from stride
                    bpp = stride // w if w > 0 else 4
                    if bpp == 4:
                        arr = raw.reshape(h, stride)
                        frame = arr[:, :w * 4].reshape(h, w, 4)
                    elif bpp == 2:
                        # UYVY or similar — convert as best we can
                        arr = raw.reshape(h, stride)
                        frame = np.zeros((h, w, 4), dtype=np.uint8)
                        frame[:, :, 0] = arr[:, 1::2][:, :w]  # Y as grayscale
                        frame[:, :, 1] = arr[:, 1::2][:, :w]
                        frame[:, :, 2] = arr[:, 1::2][:, :w]
                        frame[:, :, 3] = 255
                    else:
                        # Fallback: treat stride as the true row width in RGBA
                        px_per_row = stride // 4
                        arr = raw.reshape(h, px_per_row, 4)
                        frame = arr[:, :min(w, px_per_row), :]

                    lib.NDIlib_recv_free_video_v2(recv, ctypes.byref(video))
                    return frame
        finally:
            lib.NDIlib_recv_destroy(recv)
    finally:
        lib.NDIlib_find_destroy(finder)

    return None


def analyze_frame(frame: np.ndarray) -> dict:
    """
    Compute color analysis from an RGBA uint8 frame.
    Returns dict with histogram, averages, exposure zones, etc.
    """
    rgb = frame[:, :, :3].astype(np.float32) / 255.0
    r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]

    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b

    hist_r, _ = np.histogram(r.ravel(), bins=64, range=(0, 1))
    hist_g, _ = np.histogram(g.ravel(), bins=64, range=(0, 1))
    hist_b, _ = np.histogram(b.ravel(), bins=64, range=(0, 1))
    hist_luma, _ = np.histogram(luma.ravel(), bins=64, range=(0, 1))

    total_pixels = frame.shape[0] * frame.shape[1]
    shadows = float(np.sum(luma < 0.2)) / total_pixels
    midtones = float(np.sum((luma >= 0.2) & (luma <= 0.7))) / total_pixels
    highlights = float(np.sum(luma > 0.7)) / total_pixels

    return {
        "resolution": f"{frame.shape[1]}x{frame.shape[0]}",
        "avg_r": round(float(r.mean()), 4),
        "avg_g": round(float(g.mean()), 4),
        "avg_b": round(float(b.mean()), 4),
        "avg_luma": round(float(luma.mean()), 4),
        "min_luma": round(float(luma.min()), 4),
        "max_luma": round(float(luma.max()), 4),
        "shadows_pct": round(shadows * 100, 1),
        "midtones_pct": round(midtones * 100, 1),
        "highlights_pct": round(highlights * 100, 1),
        "hist_r": hist_r.tolist(),
        "hist_g": hist_g.tolist(),
        "hist_b": hist_b.tolist(),
        "hist_luma": hist_luma.tolist(),
    }


def save_snapshot(frame: np.ndarray, path: Optional[Path] = None) -> Path:
    """Save frame as BMP for visual inspection."""
    if path is None:
        _OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
        path = _OUTPUT_DIR / "snapshot.bmp"

    h, w = frame.shape[:2]
    row_size = (w * 3 + 3) & ~3
    pixel_data_size = row_size * h
    file_size = 54 + pixel_data_size

    with open(path, "wb") as f:
        # BMP header
        f.write(b"BM")
        f.write(file_size.to_bytes(4, "little"))
        f.write((0).to_bytes(4, "little"))
        f.write((54).to_bytes(4, "little"))
        # DIB header
        f.write((40).to_bytes(4, "little"))
        f.write(w.to_bytes(4, "little"))
        f.write(h.to_bytes(4, "little"))
        f.write((1).to_bytes(2, "little"))
        f.write((24).to_bytes(2, "little"))
        f.write((0).to_bytes(4, "little"))
        f.write(pixel_data_size.to_bytes(4, "little"))
        f.write((2835).to_bytes(4, "little"))
        f.write((2835).to_bytes(4, "little"))
        f.write((0).to_bytes(4, "little"))
        f.write((0).to_bytes(4, "little"))

        # BMP stores bottom-to-top, BGR
        padding = b"\x00" * (row_size - w * 3)
        for y in range(h - 1, -1, -1):
            for x in range(w):
                r, g, b = frame[y, x, 0], frame[y, x, 1], frame[y, x, 2]
                f.write(bytes([b, g, r]))
            if padding:
                f.write(padding)

    return path
