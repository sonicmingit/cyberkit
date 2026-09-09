"""Validate the actual V2 images, partition capacities and runtime asset names."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from prepare_v2_assets import frames, validate_rle_frame


def verify(build):
    partitions = {}
    raw = (build / "partition_table/partition-table.bin").read_bytes()
    for offset in range(0, len(raw), 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBII16sI", raw, offset)
        if magic != 0x50AA:
            break
        partitions[label.split(b"\0")[0].decode()] = (address, size)
    flash = json.loads((build / "flasher_args.json").read_text())
    results = {}
    for name, key in (("ota_0", "app"), ("assets", "assets"), ("model", "model")):
        data = (build / flash[key]["file"]).read_bytes()
        address, capacity = partitions[name]
        assert int(flash[key]["offset"], 0) == address, f"{name}: flash offset mismatch"
        assert len(data) <= capacity, f"{name}: {len(data)} exceeds {capacity}"
        results[name] = {"bytes": len(data), "capacity": capacity,
                         "sha256": hashlib.sha256(data).hexdigest()}

    model = (build / flash["model"]["file"]).read_bytes()
    assert struct.unpack_from("<I", model)[0] == 1, "V2 profile must contain one model"
    assert model[4:36].split(b"\0")[0] == b"wn9_xiaotexiaote_tts2", "Wrong wake model"
    assert "小特小特".encode() in model, "Missing actual wake word metadata"

    assets = (build / flash["assets"]["file"]).read_bytes()
    count, checksum, length = struct.unpack_from("<III", assets)
    assert length == len(assets) - 12 and sum(assets[12:]) & 0xffff == checksum
    files = {}
    base = 12 + count * 44  # Profile uses the legacy mmap format, 32-byte names.
    for i in range(count):
        name, size, offset, width, height = struct.unpack_from("<32sIIHH", assets, 12 + i * 44)
        name = name.split(b"\0")[0].decode()
        assert name not in files and assets[base+offset:base+offset+2] == b"ZZ"
        files[name] = assets[base+offset+2:base+offset+2+size]
        assert len(files[name]) == size
    emotes = {item["name"]: item for item in json.loads(files["index.json"])["emoji_collection"]}
    events = ("cruise", "wait", "accelerate", "decelerate", "hard_brake", "bump",
              "turn_left", "turn_right", "signal_lost")
    for pack in ("tita", "default"):
        for event in events:
            name = f"car_{pack}_{event}"
            assert emotes[name]["file"] in files, f"Missing resource: {name}"
    frame_count = 0
    for name, data in files.items():
        if name.startswith("car_") and name.endswith(".eaf") or name == "insert.eaf":
            for frame in frames(data):
                validate_rle_frame(frame)
                frame_count += 1
    results["resource_check"] = {"files": count, "car_emotions": 18, "validated_frames": frame_count}
    compressed = list((build / "custom_ota_binaries").glob("*.packed"))
    for file in compressed:
        # Optional output may belong to an older build; only report its capacity,
        # never equate it with a verified live OTA or current application hash.
        assert file.stat().st_size <= partitions["ota_1"][1]
        results[file.name] = {"bytes": file.stat().st_size, "capacity": partitions["ota_1"][1]}
    print(json.dumps(results, ensure_ascii=False, indent=2))
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    verify(args.build_dir)
