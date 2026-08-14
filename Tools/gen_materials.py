#!/usr/bin/env python3
"""Generate a Q2RTX-style .mat for a Quake HD texture pack (Rygel convention).

Scans <pack>/textures recursively. Every base texture (a file whose name does
not end with _norm/_bump/_gloss/_luma/_glow) gets a .mat entry referencing:

    texture_base      <base> (with its real extension)
    texture_normals   <base>_norm (fallback: <base>_bump)
    texture_gloss     <base>_gloss
    texture_emissive  <base>_luma (fallback: <base>_glow)

The layout matches Q2RTX material.c load_material_file(): "<name>:" on its own
line, tab-indented "key value" lines, full file names with extensions.

Usage:
    python gen_materials.py <packdir> <out.mat>
"""

import argparse
import os
import sys

SUFFIXES = ("_norm", "_bump", "_gloss", "_luma", "_glow")
EXTS = (".jpg", ".jpeg", ".png", ".tga", ".pcx")


def is_variant(filename):
    stem = os.path.splitext(filename)[0].lower()
    return any(stem.endswith(s) for s in SUFFIXES)


def find_file(dirpath, base, ext):
    for cand in (ext, ext.upper()):
        if os.path.isfile(os.path.join(dirpath, base + cand)):
            return cand
    return None


def find_variant(dirpath, base, suffix):
    for ext in EXTS:
        found = find_file(dirpath, base + suffix, ext)
        if found:
            return suffix + found
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("packdir", help="texture pack root (contains textures/)")
    ap.add_argument("out", help="output .mat path")
    args = ap.parse_args()

    texroot = os.path.join(args.packdir, "textures")
    if not os.path.isdir(texroot):
        sys.exit("no textures/ under %s" % args.packdir)

    # collect all supported image files: key(lowercased relpath) -> (dirpath, relname)
    all_files = {}
    for dirpath, dirnames, filenames in os.walk(texroot):
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() not in EXTS:
                continue
            rel = os.path.relpath(dirpath, texroot).replace("\\", "/")
            rel = "textures/" + ("" if rel == "." else rel + "/")
            all_files[(rel + fn).lower()] = (dirpath, rel + fn)

    entries = []
    for key in sorted(all_files):
        dirpath, relname = all_files[key]
        stem = os.path.splitext(relname)[0]  # textures/foo (no extension)
        if is_variant(relname):
            continue

        local_base = os.path.basename(stem)
        lines = ["%s:" % stem]
        lines.append("\ttexture_base %s" % relname)

        norm = find_variant(dirpath, local_base, "_norm")
        if not norm:
            norm = find_variant(dirpath, local_base, "_bump")
        if norm:
            lines.append("\ttexture_normals %s%s" % (stem, norm))

        gloss = find_variant(dirpath, local_base, "_gloss")
        if gloss:
            lines.append("\ttexture_gloss %s%s" % (stem, gloss))

        emiss = find_variant(dirpath, local_base, "_luma")
        if not emiss:
            emiss = find_variant(dirpath, local_base, "_glow")
        if emiss:
            lines.append("\ttexture_emissive %s%s" % (stem, emiss))

        entries.append("\n".join(lines))

    outdir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(outdir, exist_ok=True)
    with open(args.out, "w", newline="\n") as f:
        f.write("\n\n".join(entries) + "\n")

    print("wrote %d material entries to %s" % (len(entries), args.out))


if __name__ == "__main__":
    main()
