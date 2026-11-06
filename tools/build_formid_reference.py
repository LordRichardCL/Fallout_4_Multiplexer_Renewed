#!/usr/bin/env python3
"""Builds resources/Fallout4_reference_editorids.bin from Fallout4_main_FormIDs.json.

The JSON (Git LFS, about 330 MB) lists every record of Fallout4.esm and the six DLC masters as
exported by another tool: plugin, the lower 24 bits of the FormID, and the Editor ID, or a
placeholder such as "[PlacedObjectBinaryOverlay_0E9610]" when the record has none. Only the rows
with a real Editor ID are kept (about 141,500). A DLC row whose FormID and Editor ID are also a
Fallout4.esm row is that DLC's override of the base-game record, and is flagged as such.

Format (little-endian): a header, then a zlib stream.
  header:  b"FO4XREF\\0", u32 version (1), u32 payload size, u32 compressed size,
           32 bytes SHA-256 of the source JSON
  payload: u32 plugin count; per plugin: u16 name length, name (UTF-8), u32 source rows
           (all rows, placeholders included), u32 highest object id
           u32 row count; per row (sorted by key, then text): u32 key = plugin << 24 | object id,
           u32 text offset | 0x80000000 when the row is an override of a Fallout4.esm record
           u32 text size; Editor IDs, each NUL-terminated
Run: python3 -I tools/build_formid_reference.py resources/Fallout4_main_FormIDs.json resources/Fallout4_reference_editorids.bin
"""
import hashlib
import json
import re
import struct
import sys
import zlib

PLACEHOLDER = re.compile(r"^\[\w+BinaryOverlay_[0-9A-Fa-f]{6}\]$")


def main(src, dst):
    h = hashlib.sha256()
    with open(src, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    with open(src, "rb") as f:
        rows = json.load(f)

    plugins = []  # in first-seen order: Fallout4.esm first
    index = {}
    source_rows = {}
    max_id = {}
    kept = []
    for r in rows:
        name = r["plugin"]
        if name not in index:
            index[name] = len(plugins)
            plugins.append(name)
            source_rows[name] = 0
            max_id[name] = 0
        oid = int(r["formid"], 16)
        if oid > 0xFFFFFF:
            raise SystemExit("object id over 24 bits: %r" % r)
        source_rows[name] += 1
        max_id[name] = max(max_id[name], oid)
        if not PLACEHOLDER.match(r["entry"]):
            kept.append((index[name], oid, r["entry"]))
    if plugins[0].lower() != "fallout4.esm":
        raise SystemExit("expected Fallout4.esm first")
    base = {(oid, edid) for p, oid, edid in kept if p == 0}

    kept.sort(key=lambda t: (t[0], t[1], t[2]))
    text = bytearray()
    table = bytearray()
    for p, oid, edid in kept:
        offset = len(text)
        text += edid.encode("utf-8") + b"\0"
        flag = 0x80000000 if p != 0 and (oid, edid) in base else 0
        table += struct.pack("<II", (p << 24) | oid, offset | flag)
    if len(text) >= 0x80000000:
        raise SystemExit("text block too large")

    payload = bytearray(struct.pack("<I", len(plugins)))
    for name in plugins:
        b = name.encode("utf-8")
        payload += struct.pack("<H", len(b)) + b + struct.pack("<II", source_rows[name], max_id[name])
    payload += struct.pack("<I", len(kept)) + table + struct.pack("<I", len(text)) + text
    packed = zlib.compress(bytes(payload), 9)
    with open(dst, "wb") as f:
        f.write(b"FO4XREF\0" + struct.pack("<III", 1, len(payload), len(packed)) + h.digest() + packed)
    print("%d rows read, %d Editor IDs kept, %d plugins, %d bytes written" % (len(rows), len(kept), len(plugins),
                                                                              52 + len(packed)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
