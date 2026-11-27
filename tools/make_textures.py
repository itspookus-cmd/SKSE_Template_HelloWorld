"""Makes the three glow textures for the seam effect as uncompressed .dds files.

All textures are white; the alpha channel holds the glow shape. The plugin
tints them with each tier's color.
"""
import struct
import sys
from pathlib import Path

import numpy as np


def gauss(x, center, width):
    return np.exp(-((x - center) / width) ** 2)


def band(w=32, h=128):
    # Rows go top (v=0) to bottom (v=1). Bright thin core line + soft halo.
    v = (np.arange(h) + 0.5) / h
    profile = np.clip(gauss(v, 0.5, 0.03) + 0.45 * gauss(v, 0.5, 0.16), 0, 1)
    return np.tile(profile[:, None], (1, w))


def ray(w=64, h=256):
    # Rays: base at the bottom (v=1), fading out toward the top (v=0).
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    up = 1.0 - v  # 0 at the base, 1 at the top
    across = gauss(u, 0.5, 0.26)
    along = (1.0 - up) ** 1.8 * np.clip(up / 0.05, 0, 1)
    return np.clip(0.45 * along[:, None] * across[None, :], 0, 1)


def flare(w=128, h=32):
    # Horizontal streak: bright at the chest corner (u=0), fading to the tip (u=1).
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    vertical = np.clip(gauss(v, 0.5, 0.06) + 0.35 * gauss(v, 0.5, 0.22), 0, 1)
    along = (1.0 - u) ** 2.2
    return np.clip(0.8 * vertical[:, None] * along[None, :], 0, 1)


def mip_chain(alpha):
    levels = [alpha]
    while min(levels[-1].shape) > 1:
        a = levels[-1]
        hh, ww = max(a.shape[0] // 2, 1), max(a.shape[1] // 2, 1)
        a = a[: hh * 2, : ww * 2] if a.shape[0] > 1 and a.shape[1] > 1 else a
        if a.shape[0] >= 2 and a.shape[1] >= 2:
            a = a.reshape(hh, 2, ww, 2).mean(axis=(1, 3))
        elif a.shape[0] >= 2:
            a = a.reshape(hh, 2, a.shape[1]).mean(axis=1)
        else:
            a = a.reshape(a.shape[0], ww, 2).mean(axis=2)
        levels.append(a)
    return levels


def write_dds(path, alpha):
    """Uncompressed 32-bit BGRA .dds (white color, alpha = glow), with mipmaps."""
    levels = mip_chain(alpha)
    h, w = alpha.shape
    DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH, DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT = 0x1, 0x2, 0x4, 0x8, 0x1000, 0x20000
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT
    DDPF_ALPHAPIXELS, DDPF_RGB = 0x1, 0x40
    pixel_format = struct.pack("<II4sIIIII", 32, DDPF_RGB | DDPF_ALPHAPIXELS, b"\0\0\0\0", 32,
                               0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    DDSCAPS_COMPLEX, DDSCAPS_TEXTURE, DDSCAPS_MIPMAP = 0x8, 0x1000, 0x400000
    header = struct.pack("<4sIIIIIII", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    header += b"\0" * 44 + pixel_format
    header += struct.pack("<IIIII", DDSCAPS_COMPLEX | DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, 0, 0)
    assert len(header) == 128
    data = bytearray(header)
    for level in levels:
        a = np.round(np.clip(level, 0, 1) * 255).astype(np.uint8)
        bgra = np.empty(a.shape + (4,), np.uint8)
        bgra[..., 0:3] = 255
        bgra[..., 3] = a
        data += bgra.tobytes()
    Path(path).write_bytes(bytes(data))


if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    write_dds(out / "SeamBand.dds", band())
    write_dds(out / "SeamRay.dds", ray())
    write_dds(out / "SeamFlare.dds", flare())
    print("Saved 3 textures to", out)
