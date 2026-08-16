#!/usr/bin/env python3
"""Convert vkpt override materials (ovrd/mat/*.ktx2) into a Q2RTX-style
.pkz: textures/<name>.png + materials/ovrd.mat.

The ovrd/mat layout follows the vkpt override convention (rtname + postfix):
    maps/<name>.ktx2      - albedo (postfix "")
    maps/<name>_n.ktx2    - normal map (postfix "_n")
    maps/<name>_rme.ktx2  - roughness(R)/metallic(G)/emission(B) (postfix "_rme")

These KTX2 files have supercompression = 0 and are either raw RGBA8
(VK_FORMAT_R8G8B8A8_UNORM), BC5 (VK_FORMAT_BC5_UNORM_BLOCK, normal maps) or
BC7 (VK_FORMAT_BC7_UNORM_BLOCK, large albedos).

The converter emits, per world texture "textures/<name>":
    textures/<name>.png        - albedo (only if the override albedo exists)
    textures/<name>_n.png      - normal, with metallic baked into its alpha
                                 (from RME.G), z reconstructed for BC5 maps
    textures/<name>_gloss.png  - gloss = 1 - RME.R (roughness inverted)
    textures/<name>_luma.png   - emission from RME.B (only if non-black)
and a materials/ovrd.mat entry referencing them. The result is zipped into
a .pkz (plain zip, deflate) that the engine mounts like a PAK.

Usage:
    python convert_ovrd_mat.py <ovrd/mat> <out.pkz>
"""

import os
import struct
import sys
import zipfile

try:
    import numpy as np
    import texture2ddecoder
    from PIL import Image
except ImportError as e:
    sys.exit("missing dependency: %s (pip install numpy pillow texture2ddecoder)" % e)

VK_R8G8B8A8_UNORM = 0x25
VK_BC5_UNORM = 0x8D
VK_BC7_UNORM = 0x91


def parse_ktx2(data):
    if len(data) < 208:
        return None
    ident = data[0:12]
    if ident[1:4] != b"KTX":
        return None
    vk = struct.unpack_from("<I", data, 12)[0]
    type_size = struct.unpack_from("<I", data, 16)[0]
    w = struct.unpack_from("<I", data, 20)[0]
    h = struct.unpack_from("<I", data, 24)[0]
    depth = struct.unpack_from("<I", data, 28)[0]
    layers = struct.unpack_from("<I", data, 32)[0]
    faces = struct.unpack_from("<I", data, 36)[0]
    levels = struct.unpack_from("<I", data, 40)[0]
    sc = struct.unpack_from("<I", data, 44)[0]

    # KTX2: 80-byte header, then level index with levelCount entries of 24
    # bytes each (byteOffset u64, byteLength u64, uncompressedByteLength u64).
    # Level 0 (full resolution) is the first entry, at offset 80.
    l0_off = struct.unpack_from("<Q", data, 80)[0]
    l0_len = struct.unpack_from("<Q", data, 88)[0]

    if sc != 0:
        return ("supercompressed", vk, w, h)
    # layerCount is 0 for a single-layer texture (0 = no layers), faces must be 1
    if faces != 1 or layers > 1 or depth != 0:
        return ("array", vk, w, h)

    level = data[l0_off : l0_off + l0_len]
    return (vk, w, h, level)


def decode(vk, w, h, level):
    if vk == VK_R8G8B8A8_UNORM:
        n = w * h * 4
        if len(level) < n:
            return None
        return np.frombuffer(level[:n], dtype=np.uint8).reshape(h, w, 4)
    if vk == VK_BC5_UNORM:
        rgba = texture2ddecoder.decode_bc5(level, w, h)
        return np.frombuffer(rgba, dtype=np.uint8).reshape(h, w, 4)
    if vk == VK_BC7_UNORM:
        rgba = texture2ddecoder.decode_bc7(level, w, h)
        return np.frombuffer(rgba, dtype=np.uint8).reshape(h, w, 4)
    return None


def make_flat_normal(w, h, alpha):
    img = np.zeros((h, w, 4), dtype=np.uint8)
    img[:, :, 0] = 128
    img[:, :, 1] = 128
    img[:, :, 2] = 255
    img[:, :, 3] = alpha
    return img


def scan_dir(dirpath):
    """Decode every .ktx2 in one directory. Returns {base: {post: img}} where
    post is "base", "_n" or "_rme" (stripped from the file name)."""
    out = {}
    if not os.path.isdir(dirpath):
        return out
    for fn in sorted(os.listdir(dirpath)):
        if not fn.lower().endswith(".ktx2"):
            continue
        base = fn[:-5]
        try:
            with open(os.path.join(dirpath, fn), "rb") as f:
                raw = f.read()
        except OSError as e:
            print("  !! cannot read %s: %s" % (fn, e))
            continue
        p = parse_ktx2(raw)
        if not p or p[0] == "supercompressed" or p[0] == "array":
            print("  !! unsupported ktx2 %s: %s" % (fn, p))
            continue
        vk, w, h, level = p
        img = decode(vk, w, h, level)
        if img is None:
            print("  !! decode failed %s (vk=0x%X %dx%d)" % (fn, vk, w, h))
            continue
        post = ""
        for s in ("_n", "_rme"):
            if base.endswith(s):
                post = s
                base = base[: -len(s)]
                break
        if post == "":
            post = "base"
        out.setdefault(base, {})[post] = img
        print("  decoded %s (%dx%d, vk=0x%X, %s)" % (fn, w, h, vk, post))
    return out


def emit_material(z, mat_key, outprefix, set_, counters):
    """Write textures for one material into the zip and return its .mat lines.
    outprefix is the file path without extension, e.g. "textures/foo" or
    "progs/armor/0". counters = [n_base, n_norm, n_gloss, n_luma]."""
    n_base, n_norm, n_gloss, n_luma = counters
    lines = ["%s:" % mat_key]

    if "base" in set_:
        z.writestr(outprefix + ".png", _png_bytes(set_["base"]))
        lines.append("\ttexture_base %s.png" % outprefix)
        n_base += 1

    normal = set_.get("_n")
    rme = set_.get("_rme")
    metal_alpha = None
    if rme is not None:
        g = rme[:, :, 1].astype(np.float32)
        if g.max() > g.min() + 4:  # non-trivial metallic variation
            metal_alpha = rme[:, :, 1]

    if normal is not None:
        h, w = normal.shape[0], normal.shape[1]
        rgb = normal[:, :, :3].copy()
        b = rgb[:, :, 2].astype(np.float32)
        # BC5 normal maps have no B channel; reconstruct z
        if b.max() <= b.min():
            r = rgb[:, :, 0].astype(np.float32) / 255.0 * 2.0 - 1.0
            g = rgb[:, :, 1].astype(np.float32) / 255.0 * 2.0 - 1.0
            zz = np.sqrt(np.clip(1.0 - r * r - g * g, 0.0, 1.0))
            rgb[:, :, 2] = (zz * 0.5 + 0.5) * 255.0
        alpha = metal_alpha if metal_alpha is not None else np.full((h, w), 255, np.uint8)
        out_img = np.dstack([rgb, alpha]).astype(np.uint8)
        z.writestr(outprefix + "_n.png", _png_bytes(out_img))
        lines.append("\ttexture_normals %s_n.png" % outprefix)
        n_norm += 1
    elif metal_alpha is not None:
        h, w = metal_alpha.shape
        z.writestr(outprefix + "_n.png", _png_bytes(make_flat_normal(w, h, metal_alpha)))
        lines.append("\ttexture_normals %s_n.png" % outprefix)
        n_norm += 1

    if metal_alpha is not None:
        lines.append("\tmetalness_factor 1.0")

    if rme is not None:
        r = rme[:, :, 0].astype(np.float32)
        gloss = np.clip(1.0 - r / 255.0, 0.0, 1.0) * 255.0
        gloss_img = np.dstack([gloss, gloss, gloss, np.full_like(gloss, 255)]).astype(np.uint8)
        z.writestr(outprefix + "_gloss.png", _png_bytes(gloss_img))
        lines.append("\ttexture_gloss %s_gloss.png" % outprefix)
        n_gloss += 1

        em = rme[:, :, 2]
        if em.max() > 8:  # has emission
            luma = np.dstack([em, em, em, np.full_like(em, 255)]).astype(np.uint8)
            z.writestr(outprefix + "_luma.png", _png_bytes(luma))
            lines.append("\ttexture_emissive %s_luma.png" % outprefix)
            n_luma += 1

    counters[0], counters[1], counters[2], counters[3] = n_base, n_norm, n_gloss, n_luma
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: convert_ovrd_mat.py <ovrd/mat> <out.pkz>")
    src = sys.argv[1]
    out = sys.argv[2]

    entries = []
    counters = [0, 0, 0, 0]

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        # ---- world textures (maps/) ----
        maps_dir = os.path.join(src, "maps")
        if os.path.isdir(maps_dir):
            decoded = scan_dir(maps_dir)
            for name in sorted(decoded):
                entries.append(emit_material(z, "textures/%s" % name,
                                             "textures/%s" % name, decoded[name], counters))

        # ---- model skins and sprites (progs/) ----
        progs_dir = os.path.join(src, "progs")
        if os.path.isdir(progs_dir):
            for model in sorted(os.listdir(progs_dir)):
                mdir = os.path.join(progs_dir, model)
                if not os.path.isdir(mdir):
                    continue
                frames = scan_dir(mdir)
                ext = "" if model.endswith(".spr") else ".mdl"
                for frame in sorted(frames):
                    mat_key = "progs/%s%s:frame%s" % (model, ext, frame)
                    entries.append(emit_material(z, mat_key,
                                                 "progs/%s/%s" % (model, frame),
                                                 frames[frame], counters))

        z.writestr("materials/ovrd.mat", "\n\n".join(entries) + "\n")

    print("\nwrote %s: %d materials (%d albedo, %d normal, %d gloss, %d luma)" %
          (out, len(entries), counters[0], counters[1], counters[2], counters[3]))


def _png_bytes(img):
    from io import BytesIO
    buf = BytesIO()
    Image.fromarray(img, "RGBA").save(buf, "PNG", compress_level=6)
    return buf.getvalue()


if __name__ == "__main__":
    main()
