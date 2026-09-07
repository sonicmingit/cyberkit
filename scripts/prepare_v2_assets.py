"""Create a reproducible V2.0 asset profile without changing source animations.

Keeps TITA and CAT, samples whole independent EAF frames and adjusts FPS to
preserve approximate duration. Generated directories must live under a build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct


def frames(data):
    if data[:4] != b"\x89EAF":
        raise ValueError("Not an EAF file")
    count, checksum, length = struct.unpack_from("<III", data, 4)
    if length != len(data) - 16 or sum(data[16:]) & 0xffffffff != checksum:
        raise ValueError("Invalid EAF checksum/length")
    if not count or 16 + count * 8 > len(data):
        raise ValueError("Invalid frame count")
    result = []
    base = 16 + count * 8
    for i in range(count):
        size, offset = struct.unpack_from("<II", data, 16 + i * 8)
        frame = data[base+offset:base+offset+size]
        if len(frame) != size or frame[:2] != b"ZZ":
            raise ValueError("Invalid EAF frame")
        result.append(frame)
    return result


def validate_rle_frame(frame):
    # Matches this repository's gfx_eaf_dec.c (_S, 8-bit palette, RLE blocks).
    payload = frame[2:]
    if payload[:2] == b"_C":
        # Legacy insert ends in an emotion flag, copied byte-for-byte.
        if len(payload) < 17 or not payload[16:]:
            raise ValueError("Invalid emotion flag")
        return
    if payload[:2] != b"_S" or payload[9] != 8:
        raise ValueError("Profile expects independent 8-bit EAF frames")
    width, height, blocks, block_height = struct.unpack_from("<HHHH", payload, 10)
    if (width, height) != (360, 360) or not block_height or blocks != (height + block_height - 1)//block_height:
        raise ValueError("Invalid frame geometry")
    offset = 18 + blocks * 4 + 256 * 4
    for block_index in range(blocks):
        size = struct.unpack_from("<I", payload, 18 + block_index * 4)[0]
        block = payload[offset:offset + size]
        if len(block) != size or not block or block[0] != 0 or (size - 1) % 2:
            raise ValueError("Invalid RLE block")
        expected = width * min(block_height, height - block_index * block_height)
        if sum(block[1::2]) != expected:
            raise ValueError("RLE decoded pixel count mismatch")
        offset += size
    if offset != len(payload):
        raise ValueError("Unexpected trailing frame bytes")


def compact(data, maximum):
    source = frames(data)
    for frame in source:
        validate_rle_frame(frame)
    count = min(maximum, len(source))
    indices = [round(i * (len(source)-1) / (count-1)) for i in range(count)] if count > 1 else [0]
    selected = [source[i] for i in indices]
    offset = 0
    table = bytearray()
    for frame in selected:
        table += struct.pack("<II", len(frame), offset)
        offset += len(frame)
    body = bytes(table) + b"".join(selected)
    result = b"\x89EAF" + struct.pack("<III", count, sum(body) & 0xffffffff, len(body)) + body
    frames(result)
    return result, count / len(source)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--packs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.source, args.output, dirs_exist_ok=True)
    emote_path = args.output / "360_360/emote.json"
    emotes = json.loads(emote_path.read_text(encoding="utf-8"))
    records = []
    # The large legacy insertion clip dominates the old profile. Keep its full
    # semantic sequence as sampled frames instead of removing the feature.
    insert = args.output / "emoji_large/insert.eaf"
    original = insert.read_bytes()
    packed, ratio = compact(original, 12)
    insert.write_bytes(packed)
    for entry in emotes:
        if entry["src"] == "insert.eaf":
            entry["fps"] = max(1, round(entry["fps"] * ratio))
    records.append({"name": "insert.eaf", "original": len(original), "bytes": len(packed), "ratio": ratio})
    for pack in ("tita", "cat"):
        for source in sorted((args.packs / pack).glob("*.eaf")):
            data = source.read_bytes()
            data, ratio = compact(data, 6 if pack == "tita" else 4)
            target = f"car_{pack}_{source.stem.removeprefix(pack + '_').removeprefix('car_')}.eaf"
            (args.output / "emoji_large" / target).write_bytes(data)
            emotes.append({"emote": target[:-4], "src": target, "loop": True,
                           "fps": max(1, round((15 if pack == "tita" else 8) * ratio))})
            records.append({"name": target, "bytes": len(data), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                            "sha256": hashlib.sha256(data).hexdigest()})
    # No matching TITA driving art exists yet: use its neutral cruise explicitly,
    # rather than relabel an unrelated animation as a braking/acceleration clip.
    cruise = next(e for e in emotes if e["emote"] == "car_tita_cruise")
    for state in ("accelerate", "decelerate", "hard_brake"):
        emotes.append(dict(cruise, emote="car_tita_" + state))
    emote_path.write_text(json.dumps(emotes, ensure_ascii=False, indent=2), encoding="utf-8")
    total = sum(p.stat().st_size for p in args.output.rglob("*") if p.is_file())
    if total > 5500 * 1024 - 32 * 1024:
        raise ValueError(f"Source asset profile {total} exceeds budget incl. packing reserve")
    # Keep audit metadata out of the firmware resource tree.
    args.output.with_suffix(".manifest.json").write_text(json.dumps({"source_bytes": total, "assets": records}, indent=2), encoding="utf-8")
    print(f"V2 TITA+CAT asset profile: {total} bytes (final image still requires validation)")


if __name__ == "__main__":
    main()
