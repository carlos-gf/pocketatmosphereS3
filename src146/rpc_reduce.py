#!/usr/bin/env python3
"""
Port of sketch_rpc_depth3_texture.pde to numpy, at panel resolution.

Same operation, same constants. This exists to (a) confirm the port is faithful
before any of it goes near the firmware, and (b) try out animation schemes
without a 20-second flash cycle in the loop.
"""

import numpy as np
from PIL import Image, ImageOps
from scipy.ndimage import uniform_filter1d

# ---- constants, straight out of the sketch ---------------------------------
Q_LOG_MAX, Q_LOG_MIN = 6.0, 1.0
SIGMA_MIN, SIGMA_MAX, SIGMA_GAMMA = 3.5, 35.0, 1.3
HUE_W_SAT, HUE_W_VAL = 0.08, 0.02
HUE_SIGMA_SCALE = 1.0
TEXTURE = 0.25
NOISE_SEED = 12345
REF_W = 900.0


def q_for_depth(d):
    e = Q_LOG_MAX - (Q_LOG_MAX - Q_LOG_MIN) * min(max(d, 0.0), 1.0)
    return max(1, int(round(2.0 ** e)))


def sigma_for_depth(d, W):
    s = SIGMA_MIN + (SIGMA_MAX - SIGMA_MIN) * (min(max(d, 0.0), 1.0) ** SIGMA_GAMMA)
    return s * (W / REF_W)


# ---- blur -------------------------------------------------------------------
def boxes_for_gauss(sigma, n=3):
    w_ideal = np.sqrt((12.0 * sigma * sigma / n) + 1.0)
    wl = int(np.floor(w_ideal))
    if wl % 2 == 0:
        wl -= 1
    wl = max(wl, 1)
    wu = wl + 2
    m_ideal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0)
    m = int(round(m_ideal))
    return [wl if i < m else wu for i in range(n)]


def box_blur3(a, sigma):
    """Three box passes per axis, edges clamped. Same approximation the sketch
    uses, and the same one the device can afford (O(1) per pixel in sigma)."""
    if sigma <= 0:
        return a.copy()
    x = a.astype(np.float64, copy=True)
    for w in boxes_for_gauss(sigma):
        r = (w - 1) // 2
        if r < 1:
            continue
        x = uniform_filter1d(x, size=2 * r + 1, axis=1, mode="nearest")
        x = uniform_filter1d(x, size=2 * r + 1, axis=0, mode="nearest")
    return x


# ---- keys -------------------------------------------------------------------
def luminance(rgb):
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def _hsv(rgb):
    r, g, b = rgb[..., 0] / 255.0, rgb[..., 1] / 255.0, rgb[..., 2] / 255.0
    mx = np.maximum(r, np.maximum(g, b))
    mn = np.minimum(r, np.minimum(g, b))
    dl = mx - mn
    safe = np.where(dl == 0, 1.0, dl)
    hh = np.where(mx == r, ((g - b) / safe) / 6.0,
         np.where(mx == g, ((b - r) / safe + 2.0) / 6.0,
                           ((r - g) / safe + 4.0) / 6.0))
    hh = np.where(dl == 0, 0.0, hh)
    hh = np.where(hh < 0, hh + 1.0, hh)
    ss = np.where(mx == 0, 0.0, dl / np.where(mx == 0, 1.0, mx))
    return hh, ss, mx


def hue_term(rgb):
    hh, ss, vv = _hsv(rgb)
    t = hh + ss * HUE_W_SAT + vv * HUE_W_VAL
    return t / (1.0 + HUE_W_SAT + HUE_W_VAL)


def hue_field(rgb, sigma):
    """Hue averaged as a VECTOR, chroma-weighted. Averaging it as a number makes
    the mean of two deep reds come out cyan."""
    hh, ss, mx = _hsv(rgb)
    wgt = ss * mx
    ang = 2.0 * np.pi * hh
    cx = box_blur3(np.cos(ang) * wgt, sigma)
    sy = box_blur3(np.sin(ang) * wgt, sigma)
    a = np.arctan2(sy, cx)
    a = np.where(a < 0, a + 2.0 * np.pi, a)
    return a / (2.0 * np.pi)


def hash_noise(n, seed=NOISE_SEED):
    """The sketch's integer hash, so a stimulus regenerates bit-identically."""
    i = np.arange(n, dtype=np.uint64)
    x = i * np.uint64(0x9E3779B97F4A7C15) + np.uint64(seed) * np.uint64(0xBF58476D1CE4E5B9)
    x ^= x >> np.uint64(30); x *= np.uint64(0xBF58476D1CE4E5B9)
    x ^= x >> np.uint64(27); x *= np.uint64(0x94D049BB133111EB)
    x ^= x >> np.uint64(31)
    return ((x >> np.uint64(11)) & np.uint64(0xFFFFFF)).astype(np.float64) / 16777216.0


# ---- the operation ----------------------------------------------------------
def reduce(rgb, depth, texture=TEXTURE, field_extra=None, hue_extra=None, noise=None):
    """Depth-driven. Thin wrapper over reduce_qs."""
    return reduce_qs(rgb, q_for_depth(depth), sigma_for_depth(depth, rgb.shape[1]),
                     texture=texture, field_extra=field_extra, hue_extra=hue_extra,
                     noise=noise)


def reduce_qs(rgb, Q, sg, texture=TEXTURE, field_extra=None, hue_extra=None, noise=None):
    """One reduction. `field_extra` and `hue_extra` are the animation hooks:
    additive perturbations of the destination keys. Both leave the output an
    exact permutation of the input, because only the ORDER of destinations
    changes -- never the multiset of source pixels."""
    h, w = rgb.shape[:2]
    n = h * w

    lum = luminance(rgb)
    field = box_blur3(lum, sg)
    if field_extra is not None:
        field = field + field_extra

    ln = lum / 255.0
    if texture > 0:
        if noise is None:
            noise = hash_noise(n).reshape(h, w)
        ln = ln + texture * (noise - 0.5)
    # Q may be FRACTIONAL. floor(ln*Q) is well defined for a real Q -- it just
    # gives ceil(Q) bands, the last one narrower. At integer Q it reduces exactly
    # to the sketch. This is what makes the depth breath continuous: Q is
    # otherwise an integer and every step of it reshuffles the whole image.
    nb = int(np.ceil(Q - 1e-9))
    band = np.clip(np.floor(ln * Q).astype(np.int32), 0, nb - 1)

    skey = band + hue_term(rgb)
    src_idx = np.argsort(skey.ravel(), kind="stable")
    dst_idx = np.argsort(field.ravel(), kind="stable")

    # colour placement inside each band: keep the band's destination REGION
    # (defined by rank, so the depth ladder is untouched) but reorder the
    # positions in it by the blurred hue field, so colours gather into blobs.
    hf = hue_field(rgb, sg * HUE_SIGMA_SCALE)
    if hue_extra is not None:
        hf = np.mod(hf + hue_extra, 1.0)
    hfr = hf.ravel()
    counts = np.bincount(band.ravel(), minlength=nb)
    off = 0
    for k in range(nb):
        c = counts[k]
        if c > 1:
            sl = dst_idx[off:off + c]
            dst_idx[off:off + c] = sl[np.argsort(hfr[sl], kind="stable")]
        off += c

    out = np.empty((n, 3), dtype=np.uint8)
    out[dst_idx] = rgb.reshape(n, 3)[src_idx]
    return out.reshape(h, w, 3)


# ---- io ---------------------------------------------------------------------
def load_square(path, size):
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    s = min(im.size)
    ox, oy = (im.width - s) // 2, (im.height - s) // 2
    im = im.crop((ox, oy, ox + s, oy + s)).resize((size, size), Image.LANCZOS)
    return np.asarray(im, dtype=np.uint8)


def mask_disc(rgb, bg=(0, 0, 0)):
    h, w = rgb.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    r = min(h, w) / 2.0
    out = rgb.copy()
    out[((xx - w / 2 + 0.5) ** 2 + (yy - h / 2 + 0.5) ** 2) > r * r] = bg
    return out
