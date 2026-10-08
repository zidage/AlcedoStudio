#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Write the raster input test fixtures into this directory.

Every file is small (16x12 pixels) and its name states the color description it carries.
The ICC profiles, the PNG files and the OpenEXR files are built byte by byte here, so the
chunks, tags and attributes in each file are exactly the ones that the tests expect.
Pillow writes the JPEG files and tifffile writes the TIFF files.

Requirements: Python 3.10+, numpy, Pillow, tifffile.

Usage: python generate_raster_fixtures.py
"""

from __future__ import annotations

import io
import pathlib
import struct
import zlib

import numpy as np
import tifffile
from PIL import Image

OUT_DIR = pathlib.Path(__file__).resolve().parent
ICC_CONFIG_DIR = OUT_DIR.parents[2] / "src" / "config" / "icc"

WIDTH = 16
HEIGHT = 12

# CIE xy chromaticities: R, G, B, white.
SRGB = ((0.64, 0.33), (0.30, 0.60), (0.15, 0.06), (0.3127, 0.3290))
DISPLAY_P3 = ((0.680, 0.320), (0.265, 0.690), (0.150, 0.060), (0.3127, 0.3290))
REC2020 = ((0.708, 0.292), (0.170, 0.797), (0.131, 0.046), (0.3127, 0.3290))
PROPHOTO = ((0.7347, 0.2653), (0.1596, 0.8404), (0.0366, 0.0001), (0.3457, 0.3585))
D50_XYZ = (0.9642, 1.0, 0.8249)

BRADFORD = np.array(
    [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]
)


# ---------------------------------------------------------------------------------------------
# Color math
# ---------------------------------------------------------------------------------------------


def xy_to_xyz(x: float, y: float) -> np.ndarray:
    return np.array([x / y, 1.0, (1.0 - x - y) / y])


def rgb_to_xyz_matrix(space) -> np.ndarray:
    columns = np.stack([xy_to_xyz(*space[i]) for i in range(3)], axis=1)
    scale = np.linalg.solve(columns, xy_to_xyz(*space[3]))
    return columns * scale


def bradford(src_white_xyz, dst_white_xyz) -> np.ndarray:
    src = BRADFORD @ np.asarray(src_white_xyz)
    dst = BRADFORD @ np.asarray(dst_white_xyz)
    return np.linalg.inv(BRADFORD) @ np.diag(dst / src) @ BRADFORD


def srgb_eotf(v: np.ndarray) -> np.ndarray:
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


# ---------------------------------------------------------------------------------------------
# ICC profile builder
# ---------------------------------------------------------------------------------------------


def s15(value: float) -> bytes:
    return struct.pack(">i", int(round(value * 65536.0)))


def xyz_type(xyz) -> bytes:
    return b"XYZ " + bytes(4) + b"".join(s15(v) for v in xyz)


def sf32_type(matrix: np.ndarray) -> bytes:
    return b"sf32" + bytes(4) + b"".join(s15(v) for v in matrix.reshape(-1))


def curv_gamma(gamma: float) -> bytes:
    return b"curv" + bytes(4) + struct.pack(">IH", 1, int(round(gamma * 256.0)))


def para_type(function_type: int, params) -> bytes:
    return b"para" + bytes(4) + struct.pack(">HH", function_type, 0) + b"".join(
        s15(v) for v in params
    )


def mluc_type(text: str) -> bytes:
    utf16 = text.encode("utf-16-be")
    return b"mluc" + bytes(4) + struct.pack(">II", 1, 12) + b"enUS" + struct.pack(
        ">II", len(utf16), 28
    ) + utf16


def desc_v2_type(text: str) -> bytes:
    ascii_bytes = text.encode("ascii") + b"\0"
    return (
        b"desc"
        + bytes(4)
        + struct.pack(">I", len(ascii_bytes))
        + ascii_bytes
        + struct.pack(">II", 0, 0)
        + struct.pack(">HB", 0, 0)
        + bytes(67)
    )


def cicp_type(primaries: int, transfer: int, matrix: int = 0, full_range: int = 1) -> bytes:
    return b"cicp" + bytes(4) + bytes([primaries, transfer, matrix, full_range])


def lut16_type(input_curves, clut: np.ndarray, grid: int, out_channels: int) -> bytes:
    """lut16Type with identity matrix, the given input curves, the CLUT and identity output."""
    in_channels = len(input_curves)
    n_in = len(input_curves[0])
    n_out = 2
    body = b"mft2" + bytes(4) + bytes([in_channels, out_channels, grid, 0])
    identity = np.eye(3)
    body += b"".join(s15(v) for v in identity.reshape(-1))
    body += struct.pack(">HH", n_in, n_out)
    for curve in input_curves:
        body += b"".join(struct.pack(">H", int(round(c * 65535.0))) for c in curve)
    body += b"".join(struct.pack(">H", int(v)) for v in clut.reshape(-1))
    for _ in range(out_channels):
        body += struct.pack(">HH", 0, 65535)
    return body


def build_icc(
    tags: list[tuple[bytes, bytes]],
    *,
    version: int = 0x04400000,
    device_class: bytes = b"mntr",
    color_space: bytes = b"RGB ",
    pcs: bytes = b"XYZ ",
) -> bytes:
    """Assemble an ICC profile from (signature, data) tags. Tag data is 4-byte aligned."""
    count = len(tags)
    table_size = 4 + 12 * count
    offset = 128 + table_size
    table = struct.pack(">I", count)
    payload = b""
    for signature, data in tags:
        padded = data + bytes((-len(data)) % 4)
        table += struct.pack(">4sII", signature, offset + len(payload), len(data))
        payload += padded
    size = 128 + table_size + len(payload)
    header = struct.pack(">I", size) + b"lcms" + struct.pack(">I", version)
    header += device_class + color_space + pcs
    header += struct.pack(">6H", 2026, 10, 4, 0, 0, 0)
    header += b"acsp" + b"MSFT" + bytes(4) + bytes(4) + bytes(4) + bytes(8)
    header += struct.pack(">I", 0)
    header += b"".join(s15(v) for v in D50_XYZ)
    header += b"lcms" + bytes(16) + bytes(28)
    assert len(header) == 128
    return header + table + payload


def matrix_shaper_icc(
    description: str,
    space,
    trc: bytes,
    *,
    with_chad: bool = True,
    version: int = 0x04400000,
    extra_tags: list[tuple[bytes, bytes]] | None = None,
    colorant_override: np.ndarray | None = None,
) -> bytes:
    """RGB matrix-shaper profile.

    With ``with_chad`` the colorants are Bradford-adapted to D50, the media white is D50 and the
    `chad` tag stores the adaptation (ICC v4 practice). Without it the colorants are still
    D50-adapted but the media white tag stores the native white (ICC v2 practice).
    """
    native = rgb_to_xyz_matrix(space)
    white = xy_to_xyz(*space[3])
    adapt = bradford(white, D50_XYZ)
    colorants = adapt @ native if colorant_override is None else colorant_override
    desc = mluc_type(description) if version >= 0x04000000 else desc_v2_type(description)
    tags = [(b"desc", desc)]
    if with_chad:
        tags.append((b"wtpt", xyz_type(D50_XYZ)))
        tags.append((b"chad", sf32_type(adapt)))
    else:
        tags.append((b"wtpt", xyz_type(white)))
    tags += [
        (b"rXYZ", xyz_type(colorants[:, 0])),
        (b"gXYZ", xyz_type(colorants[:, 1])),
        (b"bXYZ", xyz_type(colorants[:, 2])),
        (b"rTRC", trc),
        (b"gTRC", trc),
        (b"bTRC", trc),
    ]
    tags += extra_tags or []
    return build_icc(tags, version=version)


SRGB_PARA = para_type(3, (2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045))


def srgb_icc() -> bytes:
    return matrix_shaper_icc("sRGB IEC61966-2.1", SRGB, SRGB_PARA)


def display_p3_icc() -> bytes:
    return matrix_shaper_icc("Display P3", DISPLAY_P3, SRGB_PARA)


def gray_gamma22_icc() -> bytes:
    tags = [
        (b"desc", mluc_type("Gray Gamma 2.2")),
        (b"wtpt", xyz_type(D50_XYZ)),
        (b"kTRC", curv_gamma(2.2)),
    ]
    return build_icc(tags, color_space=b"GRAY")


def cmyk_icc() -> bytes:
    grid = 2
    clut = np.zeros((grid,) * 4 + (3,), dtype=np.uint16)
    for idx in np.ndindex(*(grid,) * 4):
        darkness = min(1.0, sum(idx) / 2.0)
        lightness = 100.0 * (1.0 - darkness)
        clut[idx] = (int(round(lightness / 100.0 * 65280)), 32768, 32768)
    curves = [np.linspace(0.0, 1.0, 2)] * 4
    tags = [
        (b"desc", mluc_type("Synthetic CMYK")),
        (b"wtpt", xyz_type(D50_XYZ)),
        (b"A2B0", lut16_type(curves, clut, grid, 3)),
    ]
    return build_icc(tags, device_class=b"prtr", color_space=b"CMYK", pcs=b"Lab ")


def lut_based_rgb_icc() -> bytes:
    """RGB profile with only an A2B0 lut16 (sRGB to PCS XYZ D50), no matrix-shaper tags."""
    grid = 9
    srgb_to_d50 = bradford(xy_to_xyz(*SRGB[3]), D50_XYZ) @ rgb_to_xyz_matrix(SRGB)
    clut = np.zeros((grid, grid, grid, 3), dtype=np.uint16)
    axis = np.linspace(0.0, 1.0, grid)
    for r in range(grid):
        for g in range(grid):
            for b in range(grid):
                xyz = srgb_to_d50 @ np.array([axis[r], axis[g], axis[b]])
                # lut16 PCS XYZ encoding: 1.0 = 0x8000.
                clut[r, g, b] = np.clip(np.round(xyz * 32768.0), 0, 65535)
    curve = srgb_eotf(np.linspace(0.0, 1.0, 256))
    tags = [
        (b"desc", mluc_type("Synthetic LUT sRGB")),
        (b"wtpt", xyz_type(D50_XYZ)),
        (b"A2B0", lut16_type([curve, curve, curve], clut, grid, 3)),
    ]
    return build_icc(tags)


# ---------------------------------------------------------------------------------------------
# Pixels
# ---------------------------------------------------------------------------------------------


def gradient(channels: int = 3) -> np.ndarray:
    """Values in [0, 1], shape (HEIGHT, WIDTH, channels)."""
    y, x = np.mgrid[0:HEIGHT, 0:WIDTH]
    planes = [x / (WIDTH - 1), y / (HEIGHT - 1), (x + y) / (WIDTH + HEIGHT - 2), np.full(x.shape, 0.5)]
    return np.stack(planes[:channels], axis=-1)


def to_uint(values: np.ndarray, bits: int) -> np.ndarray:
    scale = (1 << bits) - 1
    dtype = np.uint8 if bits == 8 else np.uint16
    return np.clip(np.round(values * scale), 0, scale).astype(dtype)


# ---------------------------------------------------------------------------------------------
# JPEG
# ---------------------------------------------------------------------------------------------


def exif_adobe_rgb_r03() -> bytes:
    """EXIF APP1 payload: ColorSpace = 0xFFFF and InteroperabilityIndex = "R03"."""
    ifd0_offset = 8
    ifd0 = struct.pack("<H", 1) + struct.pack("<HHII", 0x8769, 4, 1, 0) + struct.pack("<I", 0)
    exif_offset = ifd0_offset + len(ifd0)
    exif_ifd_size = 2 + 2 * 12 + 4
    interop_offset = exif_offset + exif_ifd_size
    ifd0 = struct.pack("<H", 1) + struct.pack("<HHII", 0x8769, 4, 1, exif_offset) + struct.pack(
        "<I", 0
    )
    exif_ifd = struct.pack("<H", 2)
    exif_ifd += struct.pack("<HHIHH", 0xA001, 3, 1, 0xFFFF, 0)
    exif_ifd += struct.pack("<HHII", 0xA005, 4, 1, interop_offset)
    exif_ifd += struct.pack("<I", 0)
    interop = struct.pack("<H", 1) + struct.pack("<HHI", 0x0001, 2, 4) + b"R03\0"
    interop += struct.pack("<I", 0)
    tiff = b"II*\0" + struct.pack("<I", ifd0_offset) + ifd0 + exif_ifd + interop
    return b"Exif\0\0" + tiff


def exif_orientation(orientation: int, prefix: bool = True) -> bytes:
    """EXIF TIFF structure with only IFD0 Orientation."""
    ifd0 = struct.pack("<H", 1) + struct.pack("<HHIHH", 0x0112, 3, 1, orientation, 0)
    ifd0 += struct.pack("<I", 0)
    tiff = b"II*\0" + struct.pack("<I", 8) + ifd0
    return (b"Exif\0\0" + tiff) if prefix else tiff


def write_jpeg(name: str, *, icc: bytes | None = None, exif: bytes | None = None) -> None:
    image = Image.fromarray(to_uint(gradient(), 8), "RGB")
    kwargs = {"quality": 95}
    if icc is not None:
        kwargs["icc_profile"] = icc
    if exif is not None:
        kwargs["exif"] = exif
    image.save(OUT_DIR / name, "JPEG", **kwargs)


def write_cmyk_jpeg(name: str) -> None:
    image = Image.fromarray(to_uint(gradient(4), 8), "CMYK")
    image.save(OUT_DIR / name, "JPEG", quality=95, icc_profile=cmyk_icc())


# ---------------------------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------------------------


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(
        ">I", zlib.crc32(kind + data) & 0xFFFFFFFF
    )


def write_png(
    name: str,
    pixels: np.ndarray,
    color_type: int,
    *,
    chunks: list[tuple[bytes, bytes]] | None = None,
    palette: bytes | None = None,
) -> None:
    height, width = pixels.shape[:2]
    bit_depth = 16 if pixels.dtype == np.uint16 else 8
    raw = io.BytesIO()
    for row in pixels:
        raw.write(b"\0")
        if bit_depth == 16:
            raw.write(row.astype(">u2").tobytes())
        else:
            raw.write(row.tobytes())
    out = b"\x89PNG\r\n\x1a\n"
    out += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0))
    for kind, data in chunks or []:
        out += png_chunk(kind, data)
    if palette is not None:
        out += png_chunk(b"PLTE", palette)
    out += png_chunk(b"IDAT", zlib.compress(raw.getvalue()))
    out += png_chunk(b"IEND", b"")
    (OUT_DIR / name).write_bytes(out)


def iccp_chunk(name: str, icc: bytes) -> tuple[bytes, bytes]:
    return b"iCCP", name.encode("latin-1") + b"\0\0" + zlib.compress(icc)


def chrm_chunk(space) -> tuple[bytes, bytes]:
    values = [space[3][0], space[3][1], space[0][0], space[0][1], space[1][0], space[1][1],
              space[2][0], space[2][1]]
    return b"cHRM", b"".join(struct.pack(">I", int(round(v * 100000))) for v in values)


def gama_chunk(decoding_gamma: float) -> tuple[bytes, bytes]:
    return b"gAMA", struct.pack(">I", int(round(100000.0 / decoding_gamma)))


def mdcv_chunk(space, max_nits: float, min_nits: float) -> tuple[bytes, bytes]:
    data = b""
    for i in range(3):
        data += struct.pack(">HH", int(round(space[i][0] / 0.00002)), int(round(space[i][1] / 0.00002)))
    data += struct.pack(">HH", int(round(space[3][0] / 0.00002)), int(round(space[3][1] / 0.00002)))
    data += struct.pack(">II", int(round(max_nits * 10000)), int(round(min_nits * 10000)))
    return b"mDCv", data


# ---------------------------------------------------------------------------------------------
# TIFF
# ---------------------------------------------------------------------------------------------


def write_tiff(name: str, pixels: np.ndarray, *, icc: bytes | None = None) -> None:
    extratags = []
    if icc is not None:
        extratags.append((34675, 7, len(icc), icc, True))
    tifffile.imwrite(
        OUT_DIR / name,
        pixels,
        photometric="rgb",
        extratags=extratags,
        metadata=None,
        software=None,
    )


def write_nikon_tiff(*, raw: bool) -> None:
    """A 32x24 Nikon TIFF: CFA sensor samples or exported RGB, with no color matrix.

    The invented model keeps LibRaw's camera matrix table from supplying a matrix.
    The RAW file has a metadata-only IFD0 and a CFA SubIFD, as the Z50 II NEF does.
    All pixels are generated; no camera image or personal metadata is included.
    """
    width, height = 32, 24
    channels = 1 if raw else 3
    camera = [(271, 2, 6, b"Nikon\0"), (272, 2, 19, b"Alcedo CFA fixture\0")]
    image = [
        (256, 4, 1, width), (257, 4, 1, height), (258, 3, 1, 16),
        (259, 3, 1, 1), (262, 3, 1, 32803 if raw else 2),
        (273, 4, 1, 0), (277, 3, 1, channels), (278, 4, 1, height),
        (279, 4, 1, width * height * channels * 2),
    ]
    if raw:
        image += [(33421, 3, 2, 0x00020002), (33422, 1, 4, 0x02010100)]
    first = camera + [(330, 4, 1, 8 + 2 + 3 * 12 + 4)] if raw else camera + image
    directories = [sorted(first), sorted(image)] if raw else [sorted(first)]
    values_offset = 8 + sum(2 + len(entries) * 12 + 4 for entries in directories)
    camera_bytes = b"".join(value for _, _, _, value in camera)
    pixels_offset = values_offset + len(camera_bytes)
    encoded = bytearray(b"II*\0" + struct.pack("<I", 8))
    for entries in directories:
        encoded += struct.pack("<H", len(entries))
        for tag, kind, count, value in entries:
            if isinstance(value, bytes):
                field = values_offset
                values_offset += len(value)
            else:
                field = pixels_offset if tag == 273 else value
            encoded += struct.pack("<HHII", tag, kind, count, field)
        encoded += struct.pack("<I", 0)
    encoded += camera_bytes
    encoded += struct.pack("<H", 1000) * (width * height * channels)
    name = "nikon_cfa_without_color_matrix.tif" if raw else "nikon_rgb_with_camera_metadata.tif"
    (OUT_DIR / name).write_bytes(encoded)


# ---------------------------------------------------------------------------------------------
# OpenEXR (single-part scanline, no compression)
# ---------------------------------------------------------------------------------------------


def exr_attribute(name: str, kind: str, data: bytes) -> bytes:
    return name.encode() + b"\0" + kind.encode() + b"\0" + struct.pack("<i", len(data)) + data


def write_exr(name: str, pixels: np.ndarray, *, half: bool, attributes: list[bytes]) -> None:
    height, width = pixels.shape[:2]
    pixel_type = 1 if half else 2
    channel_names = ["B", "G", "R"]  # chlist is sorted by name
    chlist = b""
    for channel in channel_names:
        chlist += channel.encode() + b"\0" + struct.pack("<iB3xii", pixel_type, 0, 1, 1)
    chlist += b"\0"
    box = struct.pack("<iiii", 0, 0, width - 1, height - 1)
    header = b"\x76\x2f\x31\x01" + struct.pack("<I", 2)
    header += exr_attribute("channels", "chlist", chlist)
    header += exr_attribute("compression", "compression", b"\0")
    header += exr_attribute("dataWindow", "box2i", box)
    header += exr_attribute("displayWindow", "box2i", box)
    header += exr_attribute("lineOrder", "lineOrder", b"\0")
    header += exr_attribute("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += exr_attribute("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    header += exr_attribute("screenWindowWidth", "float", struct.pack("<f", 1.0))
    for attribute in attributes:
        header += attribute
    header += b"\0"
    dtype = "<f2" if half else "<f4"
    channel_index = {"R": 0, "G": 1, "B": 2}
    blocks = []
    for y in range(height):
        line = b"".join(
            pixels[y, :, channel_index[c]].astype(dtype).tobytes() for c in channel_names
        )
        blocks.append(struct.pack("<ii", y, len(line)) + line)
    table_offset = len(header)
    first_block = table_offset + 8 * height
    offsets = []
    position = first_block
    for block in blocks:
        offsets.append(position)
        position += len(block)
    data = header + b"".join(struct.pack("<Q", o) for o in offsets) + b"".join(blocks)
    (OUT_DIR / name).write_bytes(data)


def chromaticities_attribute(space) -> bytes:
    values = [space[0][0], space[0][1], space[1][0], space[1][1], space[2][0], space[2][1],
              space[3][0], space[3][1]]
    return exr_attribute("chromaticities", "chromaticities", struct.pack("<8f", *values))


# ---------------------------------------------------------------------------------------------
# Fixture set
# ---------------------------------------------------------------------------------------------


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    linear = gradient()

    # JPEG
    write_jpeg("srgb_icc_8bit.jpg", icc=srgb_icc())
    write_jpeg("display_p3_icc_8bit.jpg", icc=display_p3_icc())
    write_jpeg("adobe_rgb_exif_r03_no_icc.jpg", exif=exif_adobe_rgb_r03())
    write_jpeg("untagged_8bit.jpg")
    write_cmyk_jpeg("cmyk_icc.jpg")
    write_jpeg("srgb_orientation6_8bit.jpg", icc=srgb_icc(), exif=exif_orientation(6))
    degenerate = rgb_to_xyz_matrix(SRGB)
    degenerate[:, 1] = degenerate[:, 0]
    write_jpeg(
        "degenerate_primaries_icc_8bit.jpg",
        icc=matrix_shaper_icc("Degenerate", SRGB, SRGB_PARA, colorant_override=degenerate),
    )

    # PNG
    rgb16 = to_uint(linear, 16)
    rgb8 = to_uint(linear, 8)
    write_png(
        "cicp_rec2020_pq_16bit.png",
        rgb16,
        2,
        chunks=[
            (b"cICP", bytes([9, 16, 0, 1])),
            mdcv_chunk(REC2020, 4000.0, 0.005),
            iccp_chunk("Display P3", display_p3_icc()),
            (b"sRGB", b"\0"),
        ],
    )
    write_png("iccp_display_p3.png", rgb8, 2, chunks=[iccp_chunk("Display P3", display_p3_icc())])
    write_png("srgb_chunk.png", rgb8, 2, chunks=[(b"sRGB", b"\0"), gama_chunk(2.2)])
    write_png("gama_chrm_only.png", rgb8, 2, chunks=[gama_chunk(1.8), chrm_chunk(REC2020)])
    write_png("gama_without_chrm.png", rgb8, 2, chunks=[gama_chunk(2.2)])
    palette = bytes(v for i in range(4) for v in (i * 80, 255 - i * 80, 128))
    indices = (np.mgrid[0:HEIGHT, 0:WIDTH][1] % 4).astype(np.uint8)
    write_png("palette_untagged.png", indices, 3, palette=palette)
    gray = to_uint(gradient(1)[:, :, 0], 8)
    write_png("gray_gamma22.png", gray, 0, chunks=[iccp_chunk("Gray", gray_gamma22_icc())])
    rgba = to_uint(np.concatenate([linear, gradient(2)[:, :, :1]], axis=-1), 8)
    write_png("rgba_with_alpha.png", rgba, 6, chunks=[(b"sRGB", b"\0")])
    write_png("rgb_same_as_rgba_with_alpha.png", rgba[:, :, :3].copy(), 2,
              chunks=[(b"sRGB", b"\0")])
    write_png("srgb_mirrored_orientation2.png", rgb8, 2,
              chunks=[(b"sRGB", b"\0"), (b"eXIf", exif_orientation(2, prefix=False))])

    # TIFF
    write_nikon_tiff(raw=True)
    write_nikon_tiff(raw=False)
    write_tiff(
        "rec2020_icc_16bit.tif",
        rgb16,
        icc=matrix_shaper_icc("Rec2020 Gamma 2.4 v2", REC2020, curv_gamma(2.4), with_chad=False,
                              version=0x02100000),
    )
    write_tiff(
        "prophoto_icc_16bit.tif",
        rgb16,
        icc=matrix_shaper_icc("ProPhoto RGB v2", PROPHOTO, curv_gamma(1.8), with_chad=False,
                              version=0x02100000),
    )
    linear_icc = matrix_shaper_icc("Linear Rec709", SRGB, curv_gamma(1.0))
    write_tiff("float32_linear_icc.tif", linear.astype(np.float32), icc=linear_icc)
    write_tiff("float32_srgb_icc.tif", linear.astype(np.float32), icc=srgb_icc())
    write_tiff("float32_no_icc.tif", linear.astype(np.float32))
    write_tiff("uncompressed_rgb16_no_make.tif", rgb16)
    write_tiff("lut_based_rgb_icc.tif", rgb16, icc=lut_based_rgb_icc())
    write_tiff(
        "icc_v44_cicp_rec2020_pq_16bit.tif",
        rgb16,
        icc=matrix_shaper_icc("Rec2020 PQ cicp", SRGB, curv_gamma(2.2),
                              extra_tags=[(b"cicp", cicp_type(9, 16))]),
    )
    for profile in sorted(ICC_CONFIG_DIR.glob("*.icc")):
        write_tiff(f"alcedo_export_{profile.stem}.tif", rgb16, icc=profile.read_bytes())

    # OpenEXR
    write_exr("chromaticities_p3_half.exr", linear, half=True,
              attributes=[chromaticities_attribute(DISPLAY_P3)])
    write_exr("aces_container_flag.exr", linear, half=True,
              attributes=[exr_attribute("acesImageContainerFlag", "int", struct.pack("<i", 1))])
    write_exr("no_chromaticities_float.exr", linear, half=False, attributes=[])


if __name__ == "__main__":
    main()
