"""Rough preview of the seam glow: draws a plain box 'chest' and the glow model
(read back from the .nif via its known geometry rules) with additive blending.
Only for checking proportions; it is not how Skyrim renders."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import make_textures as T  # noqa: E402

W, H = 900, 620
CHEST = dict(w=60.0, d=32.0, h=34.0)
SEAM_HEIGHT = 0.6
INFLATE = 0.6
COLOR = np.array([1.0, 0.78, 0.0])
STRENGTH = 2.0

# Same geometry rules as make_seam_nif.cpp
BAND_HEIGHT, RAY_HEIGHT, FLARE_HEIGHT, FLARE_LENGTH, OUT = 10.0, 28.0, 7.0, 0.22, 0.004
SIDES = [((0, -0.5), (1, 0), (0, -1), 6), ((0, 0.5), (-1, 0), (0, 1), 6),
         ((0.5, 0), (0, 1), (1, 0), 3), ((-0.5, 0), (0, -1), (-1, 0), 3)]


def at(s, t, z, out=0.0):
    (ox, oy), (ax, ay), (nx, ny), _ = s
    return np.array([ox + ax * t + nx * out, oy + ay * t + ny * out, z])


def quads():
    band, rays, flares = [], [], []
    h = BAND_HEIGHT / 2
    for s in SIDES:
        band.append(([at(s, -.5, -h), at(s, .5, -h), at(s, .5, h), at(s, -.5, h)], [(0, 1), (1, 1), (1, 0), (0, 0)]))
        n = s[3]
        for i in range(n):
            t = -0.5 + (i + 0.5) / n
            lean, z1 = t * 0.35, RAY_HEIGHT * (0.75 + 0.25 * np.cos(i * 2.1))
            rays.append(([at(s, t - .05, -1, OUT), at(s, t + .05, -1, OUT), at(s, t + lean + .12, z1, OUT + .14),
                          at(s, t + lean - .12, z1, OUT + .14)], [(0, 1), (1, 1), (1, 0), (0, 0)]))
    fh = FLARE_HEIGHT / 2
    for s in SIDES[:2]:
        flares.append(([at(s, .5, -fh, OUT), at(s, .5 + FLARE_LENGTH, -fh, OUT), at(s, .5 + FLARE_LENGTH, fh, OUT),
                        at(s, .5, fh, OUT)], [(0, 1), (1, 1), (1, 0), (0, 0)]))
        flares.append(([at(s, -.5 - FLARE_LENGTH, -fh, OUT), at(s, -.5, -fh, OUT), at(s, -.5, fh, OUT),
                        at(s, -.5 - FLARE_LENGTH, fh, OUT)], [(1, 1), (0, 1), (0, 0), (1, 0)]))
    return band, rays, flares


def place(p):
    # Stretch the 1x1 model to the chest (like the plugin) and lift it to the seam.
    sx, sy = CHEST['w'] + 2 * INFLATE, CHEST['d'] + 2 * INFLATE
    return np.array([p[0] * sx, p[1] * sy, p[2] + CHEST['h'] * SEAM_HEIGHT])


def camera(yaw=-25, pitch=-22, dist=150):
    y, p = np.radians(yaw), np.radians(pitch)
    fwd = np.array([np.sin(y) * np.cos(p), np.cos(y) * np.cos(p), np.sin(p)])
    eye = np.array([0, 0, CHEST['h'] * 0.5]) - fwd * dist
    right = np.cross(fwd, [0, 0, 1]); right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    return eye, fwd, right, up


EYE, FWD, RIGHT, UP = camera()
FOCAL = 900


def project(p):
    d = p - EYE
    z = d @ FWD
    return np.array([W / 2 + FOCAL * (d @ RIGHT) / z, H / 2 - FOCAL * (d @ UP) / z, z])


def raster(tri, on_pixels):
    a, b, c = (project(p) for p in tri)
    xs, ys = [a[0], b[0], c[0]], [a[1], b[1], c[1]]
    x0, x1 = max(int(min(xs)), 0), min(int(max(xs)) + 1, W)
    y0, y1 = max(int(min(ys)), 0), min(int(max(ys)) + 1, H)
    if x0 >= x1 or y0 >= y1:
        return
    gx, gy = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
    den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(den) < 1e-9:
        return
    l1 = ((b[1] - c[1]) * (gx - c[0]) + (c[0] - b[0]) * (gy - c[1])) / den
    l2 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / den
    l3 = 1 - l1 - l2
    inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
    # perspective-correct weights
    w = np.stack([l1 / a[2], l2 / b[2], l3 / c[2]])
    wsum = w.sum(0)
    depth = 1 / wsum
    on_pixels(slice(y0, y1), slice(x0, x1), inside, w / wsum, depth)


def main(out):
    img = np.zeros((H, W, 3))
    img[:] = [0.10, 0.09, 0.08]
    zbuf = np.full((H, W), np.inf)

    # Floor
    floor = [np.array(p) for p in [(-120, -90, 0), (120, -90, 0), (120, 90, 0), (-120, 90, 0)]]
    for tri in ([floor[0], floor[1], floor[2]], [floor[0], floor[2], floor[3]]):
        def paint(ys, xs, inside, w, depth):
            m = inside & (depth < zbuf[ys, xs])
            zbuf[ys, xs][m] = depth[m]
            img[ys, xs][m] = [0.20, 0.19, 0.17]
        raster(tri, paint)

    # Chest box
    hw, hd, hh = CHEST['w'] / 2, CHEST['d'] / 2, CHEST['h']
    v = {k: np.array(p) for k, p in dict(
        a=(-hw, -hd, 0), b=(hw, -hd, 0), c=(hw, hd, 0), d=(-hw, hd, 0),
        e=(-hw, -hd, hh), f=(hw, -hd, hh), g=(hw, hd, hh), h=(-hw, hd, hh)).items()}
    faces = [("abfe", .42), ("bcgf", .30), ("cdhg", .36), ("daeh", .30), ("efgh", .50)]
    for names, shade in faces:
        q = [v[n] for n in names]
        for tri in ([q[0], q[1], q[2]], [q[0], q[2], q[3]]):
            def paint(ys, xs, inside, w, depth, shade=shade):
                m = inside & (depth < zbuf[ys, xs])
                zbuf[ys, xs][m] = depth[m]
                img[ys, xs][m] = np.array([0.36, 0.24, 0.13]) * shade * 2.2
            raster(tri, paint)
    # seam line drawn on the box (lid edge)
    # Glow parts, additive, depth-tested against the chest
    band, rays, flares = quads()
    textures = {"band": T.band(), "ray": T.ray(), "flare": T.flare()}
    glow = np.zeros((H, W))
    for name, parts in (("band", band), ("ray", rays), ("flare", flares)):
        tex = textures[name]
        th, tw = tex.shape
        for pts, uvs in parts:
            P = [place(p) for p in pts]
            UV = [np.array(u, float) for u in uvs]
            for (i, j, k) in ((0, 1, 2), (0, 2, 3)):
                def paint(ys, xs, inside, w, depth, i=i, j=j, k=k, UV=UV, tex=tex):
                    m = inside & (depth < zbuf[ys, xs] - 0.05)
                    uv = w[0][..., None] * UV[i] + w[1][..., None] * UV[j] + w[2][..., None] * UV[k]
                    tx = np.clip((uv[..., 0] * tw).astype(int), 0, tw - 1)
                    ty = np.clip((uv[..., 1] * th).astype(int), 0, th - 1)
                    a = tex[ty, tx]
                    glow[ys, xs][m] += a[m]
                raster([P[i], P[j], P[k]], paint)
    img += glow[..., None] * COLOR * STRENGTH
    img = 1 - np.exp(-img * 1.4)  # soft tone curve so bright parts bloom to white-ish
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out)
    print("saved", out)


if __name__ == "__main__":
    main(sys.argv[1])
