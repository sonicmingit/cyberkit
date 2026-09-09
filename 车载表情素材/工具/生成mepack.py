#!/usr/bin/env python3
"""Build and verify deterministic MEPACK v1 expression packages.

The source is a reviewed ``NNN-名称-vN`` master directory.  The generated
``.mepack`` contains only a UTF-8 manifest, a fixed-size state index and the
original EAF payloads; previews and editable source material are never packed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


MAGIC = b"MEPK"
SCHEMA_VERSION = 1
HEADER_SIZE = 128
PAYLOAD_ALIGNMENT = 4096
ENTRY_SIZE = 96
HEADER = struct.Struct("<4sHHIIIIIQQQ32s32s12s")
ENTRY = struct.Struct("<32sQQ32sHHHHII")
FLAG_LOOP = 0x0001

MASTER_NAME = re.compile(r"^(?P<number>\d{3})-(?P<name>.+)-v(?P<version>[1-9]\d*)$")
PACK_ID = re.compile(r"^[a-z][a-z0-9_]{0,30}$")
STATE_ID = re.compile(r"^[a-z][a-z0-9_]{0,30}$")
README_PACK_ID = re.compile(r"\|\s*`pack_id`\s*\|\s*`([a-z][a-z0-9_]*)`\s*\|")


class PackError(ValueError):
    pass


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PackError(f"无法读取 JSON：{path}: {exc}") from exc


def validate_eaf(data: bytes, expected_width: int, expected_height: int) -> int:
    if len(data) < 16 or data[:4] != b"\x89EAF":
        raise PackError("EAF 文件头无效")
    count, checksum, body_length = struct.unpack_from("<III", data, 4)
    if count == 0 or body_length != len(data) - 16:
        raise PackError("EAF 帧数或长度无效")
    if sum(data[16:]) & 0xFFFFFFFF != checksum:
        raise PackError("EAF 校验和无效")
    table_end = 16 + count * 8
    if table_end > len(data):
        raise PackError("EAF 帧索引越界")
    payload_base = table_end
    for index in range(count):
        size, offset = struct.unpack_from("<II", data, 16 + index * 8)
        frame = data[payload_base + offset:payload_base + offset + size]
        if len(frame) != size or len(frame) < 30 or frame[:4] != b"ZZ_S":
            raise PackError(f"EAF 第 {index} 帧无效")
        frame_payload = frame[2:]
        if frame_payload[9] != 8:
            raise PackError(f"EAF 第 {index} 帧不是 8-bit 索引色")
        width, height, blocks, block_height = struct.unpack_from("<HHHH", frame_payload, 10)
        if (width, height) != (expected_width, expected_height):
            raise PackError(
                f"EAF 第 {index} 帧尺寸为 {width}x{height}，预期 "
                f"{expected_width}x{expected_height}"
            )
        if block_height == 0 or blocks != (height + block_height - 1) // block_height:
            raise PackError(f"EAF 第 {index} 帧分块信息无效")
        block_data_offset = 18 + blocks * 4 + 256 * 4
        if block_data_offset > len(frame_payload):
            raise PackError(f"EAF 第 {index} 帧调色板或分块表越界")
        for block_index in range(blocks):
            block_size = struct.unpack_from("<I", frame_payload, 18 + block_index * 4)[0]
            block = frame_payload[block_data_offset:block_data_offset + block_size]
            if len(block) != block_size or not block or block[0] != 0 or (block_size - 1) % 2:
                raise PackError(f"EAF 第 {index} 帧第 {block_index} 个 RLE 分块无效")
            expected_pixels = width * min(block_height, height - block_index * block_height)
            if sum(block[1::2]) != expected_pixels:
                raise PackError(f"EAF 第 {index} 帧第 {block_index} 个 RLE 像素数无效")
            block_data_offset += block_size
        if block_data_offset != len(frame_payload):
            raise PackError(f"EAF 第 {index} 帧存在未解析尾部数据")
    return count


def gif_dimensions(path: Path) -> tuple[int, int]:
    try:
        data = path.read_bytes()[:10]
    except OSError as exc:
        raise PackError(f"无法读取 GIF：{path}: {exc}") from exc
    if len(data) != 10 or data[:6] not in (b"GIF87a", b"GIF89a"):
        raise PackError(f"GIF 文件头无效：{path}")
    return struct.unpack_from("<HH", data, 6)


def source_entries(raw: Any) -> list[dict[str, Any]]:
    if isinstance(raw, list):
        entries = raw
    elif isinstance(raw, dict):
        entries = raw.get("states", raw.get("assets"))
    else:
        entries = None
    if not isinstance(entries, list) or not all(isinstance(item, dict) for item in entries):
        raise PackError("eaf/manifest.json 必须是状态对象数组，或包含 states/assets 数组")
    return entries


def infer_fps(item: dict[str, Any]) -> int:
    explicit = item.get("fps")
    if isinstance(explicit, int) and 1 <= explicit <= 63:
        return explicit
    durations = item.get("duration_ms")
    if not isinstance(durations, list) or not durations or not all(
        isinstance(value, int) and value > 0 for value in durations
    ):
        raise PackError(f"{item.get('output', item.get('state'))} 缺少有效 fps/duration_ms")
    if len(set(durations)) != 1:
        raise PackError(f"{item.get('output', item.get('state'))} 使用可变帧时长，必须显式提供 fps")
    fps = round(1000 / durations[0])
    if not 1 <= fps <= 63:
        raise PackError(f"推导出的 FPS {fps} 超出设备范围 1..63")
    return fps


def parse_identity(source: Path, pack_id_override: str | None) -> dict[str, Any]:
    match = MASTER_NAME.fullmatch(source.name)
    if not match:
        raise PackError("素材母版目录必须命名为 NNN-表情包名称-vN")
    pack_id = pack_id_override
    if pack_id is None:
        readme = source / "README.md"
        try:
            readme_text = readme.read_text(encoding="utf-8")
        except OSError as exc:
            raise PackError(f"无法读取 {readme}: {exc}") from exc
        id_match = README_PACK_ID.search(readme_text)
        if not id_match:
            raise PackError("README.md 未登记 `pack_id`，请补齐或使用 --pack-id")
        pack_id = id_match.group(1)
    if not PACK_ID.fullmatch(pack_id):
        raise PackError("pack_id 必须是 1..31 位小写 ASCII snake_case")
    return {
        "number": match.group("number"),
        "display_name": match.group("name"),
        "pack_id": pack_id,
        "version": int(match.group("version")),
    }


def prepare_state(source: Path, item: dict[str, Any]) -> dict[str, Any]:
    raw_state = item.get("state")
    output = item.get("output")
    if not isinstance(raw_state, str) or not isinstance(output, str):
        raise PackError("manifest 状态必须同时包含字符串 state 和 output")
    state = raw_state.removeprefix("car_")
    if not STATE_ID.fullmatch(state):
        raise PackError(f"状态名无效：{raw_state}")
    expected_output = f"car_{state}.eaf"
    if output != expected_output:
        raise PackError(f"{raw_state} 的 output 应为 {expected_output}，实际为 {output}")

    eaf_path = source / "eaf" / output
    gif_path = source / "gif" / f"car_{state}.gif"
    try:
        data = eaf_path.read_bytes()
    except OSError as exc:
        raise PackError(f"无法读取 EAF：{eaf_path}: {exc}") from exc
    width = item.get("width")
    height = item.get("height")
    if (width, height) != (360, 360):
        raise PackError(f"{output} 清单尺寸必须为 360x360")
    gif_size = gif_dimensions(gif_path)
    if gif_size != (360, 360):
        raise PackError(f"{gif_path.name} 尺寸为 {gif_size[0]}x{gif_size[1]}，预期 360x360")
    frames = validate_eaf(data, width, height)
    digest = hashlib.sha256(data).hexdigest()
    if item.get("frames") != frames:
        raise PackError(f"{output} 清单帧数与 EAF 不一致")
    if item.get("size_bytes") != len(data):
        raise PackError(f"{output} 清单大小与 EAF 不一致")
    if item.get("sha256") != digest:
        raise PackError(f"{output} 清单 SHA-256 与 EAF 不一致")
    return {
        "id": state,
        "source": output,
        "width": width,
        "height": height,
        "frames": frames,
        "fps": infer_fps(item),
        "loop": bool(item.get("loop_recommended", item.get("loop", False))),
        "end_behavior": item.get("end_behavior", "loop" if item.get("loop_recommended") else "return_to_current_state"),
        "size": len(data),
        "sha256": digest,
        "data": data,
    }


def validate_source_inventory(source: Path, states: list[dict[str, Any]], allow_legacy: bool) -> None:
    ids = [state["id"] for state in states]
    if len(ids) != len(set(ids)):
        raise PackError("manifest 包含重复状态")
    required = {"cruise", "idle_01", "idle_02", "idle_03"}
    missing = sorted(required - set(ids))
    if missing and not allow_legacy:
        raise PackError("缺少新包必备状态：" + ", ".join(missing))

    expected_eaf = {f"car_{state}.eaf" for state in ids}
    actual_eaf = {path.name for path in (source / "eaf").glob("*.eaf")}
    expected_gif = {f"car_{state}.gif" for state in ids}
    actual_gif = {path.name for path in (source / "gif").glob("*.gif")}
    if actual_eaf != expected_eaf:
        raise PackError(f"EAF 文件集合与 manifest 不一致：缺少 {sorted(expected_eaf-actual_eaf)}，多余 {sorted(actual_eaf-expected_eaf)}")
    if actual_gif != expected_gif:
        raise PackError(f"GIF 文件集合与 manifest 不一致：缺少 {sorted(expected_gif-actual_gif)}，多余 {sorted(actual_gif-expected_gif)}")


def build_package(args: argparse.Namespace) -> dict[str, Any]:
    source = args.source.resolve()
    identity = parse_identity(source, args.pack_id)
    source_manifest_path = source / "eaf" / "manifest.json"
    try:
        source_manifest_bytes = source_manifest_path.read_bytes()
    except OSError as exc:
        raise PackError(f"无法读取 {source_manifest_path}: {exc}") from exc
    entries = source_entries(load_json(source_manifest_path))
    states = sorted((prepare_state(source, item) for item in entries), key=lambda item: item["id"])
    validate_source_inventory(source, states, args.allow_legacy_missing_idle)

    payload = bytearray()
    for state in states:
        state["offset"] = len(payload)
        payload.extend(state["data"])
        payload.extend(b"\0" * (align(len(payload), 4) - len(payload)))

    manifest_states = []
    for state in states:
        manifest_states.append({key: state[key] for key in (
            "id", "source", "offset", "size", "sha256", "width", "height",
            "frames", "fps", "loop", "end_behavior"
        )})
    manifest: dict[str, Any] = {
        "format": "mepack",
        "schema_version": SCHEMA_VERSION,
        "package": identity,
        "compatibility": {
            "board": args.board,
            "display_width": 360,
            "display_height": 360,
            "eaf_encoding": "EAF 8-bit indexed BGRA palette + RLE",
            "loader_schema": SCHEMA_VERSION,
            "min_firmware": args.min_firmware,
        },
        "fallback": {"default_state": "cruise"},
        "source": {
            "directory": source.name,
            "manifest_sha256": hashlib.sha256(source_manifest_bytes).hexdigest(),
        },
        "signature": {"algorithm": "none", "status": "unsigned"},
        "states": manifest_states,
    }
    if args.allow_legacy_missing_idle:
        manifest["legacy_missing_idle_allowed"] = True
    manifest_bytes = json.dumps(
        manifest, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")

    index = bytearray()
    for state in states:
        encoded_id = state["id"].encode("ascii")
        flags = FLAG_LOOP if state["loop"] else 0
        index.extend(ENTRY.pack(
            encoded_id.ljust(32, b"\0"), state["offset"], state["size"],
            bytes.fromhex(state["sha256"]), state["fps"], flags,
            state["width"], state["height"], state["frames"], 0,
        ))

    payload_offset = align(HEADER_SIZE + len(manifest_bytes) + len(index), PAYLOAD_ALIGNMENT)
    total_size = payload_offset + len(payload)
    header = HEADER.pack(
        MAGIC, SCHEMA_VERSION, HEADER_SIZE, 0, len(manifest_bytes), len(states),
        ENTRY_SIZE, len(index), payload_offset, len(payload), total_size,
        sha256(manifest_bytes), sha256(payload), b"\0" * 12,
    )
    package = header + manifest_bytes + index
    package += b"\0" * (payload_offset - len(package))
    package += payload
    if len(package) != total_size:
        raise AssertionError("内部错误：包长度计算不一致")

    output = args.output
    if output is None:
        output = source.parent / "设备资源包" / (
            f"{identity['number']}-{identity['pack_id']}-v{identity['version']}.mepack"
        )
    output = output.resolve()
    expected_name = f"{identity['number']}-{identity['pack_id']}-v{identity['version']}.mepack"
    if output.name != expected_name:
        raise PackError(f"输出文件名必须为 {expected_name}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + ".tmp")
    try:
        temp.write_bytes(package)
        temp.replace(output)
    finally:
        if temp.exists():
            temp.unlink()
    package_hash = hashlib.sha256(package).hexdigest()
    sidecar = output.with_suffix(output.suffix + ".sha256")
    sidecar.write_text(f"{package_hash}  {output.name}\n", encoding="ascii")
    return {
        "output": str(output),
        "bytes": len(package),
        "sha256": package_hash,
        "states": len(states),
        "payload_bytes": len(payload),
        "identity": identity,
        "unsigned": True,
    }


def verify_package(path: Path) -> dict[str, Any]:
    data = path.resolve().read_bytes()
    if len(data) < HEADER_SIZE:
        raise PackError("MEPACK 文件短于固定包头")
    values = HEADER.unpack_from(data)
    (magic, schema, header_size, flags, manifest_size, file_count, entry_size,
     index_size, payload_offset, payload_size, total_size, manifest_hash,
     payload_hash, reserved) = values
    if magic != MAGIC or schema != SCHEMA_VERSION or header_size != HEADER_SIZE:
        raise PackError("MEPACK 魔数、版本或包头长度无效")
    if flags != 0 or reserved != b"\0" * 12:
        raise PackError("MEPACK v1 包头包含不支持的标志")
    if entry_size != ENTRY_SIZE or index_size != file_count * ENTRY_SIZE:
        raise PackError("MEPACK 索引长度无效")
    if total_size != len(data) or payload_offset + payload_size != total_size:
        raise PackError("MEPACK 总长度或 payload 边界无效")
    manifest_start = HEADER_SIZE
    manifest_end = manifest_start + manifest_size
    index_end = manifest_end + index_size
    if index_end > payload_offset:
        raise PackError("MEPACK manifest/index 越界")
    manifest_bytes = data[manifest_start:manifest_end]
    payload = data[payload_offset:]
    if sha256(manifest_bytes) != manifest_hash or sha256(payload) != payload_hash:
        raise PackError("MEPACK manifest 或 payload SHA-256 无效")
    if any(data[index_end:payload_offset]):
        raise PackError("MEPACK 对齐填充必须为零")
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise PackError(f"MEPACK manifest 无效：{exc}") from exc
    if manifest.get("format") != "mepack" or manifest.get("schema_version") != schema:
        raise PackError("MEPACK manifest 格式或 schema_version 无效")
    compatibility = manifest.get("compatibility")
    if not isinstance(compatibility, dict) or (
        compatibility.get("display_width"), compatibility.get("display_height")
    ) != (360, 360) or compatibility.get("loader_schema") != schema:
        raise PackError("MEPACK 兼容性清单无效")
    package = manifest.get("package")
    if not isinstance(package, dict):
        raise PackError("MEPACK 缺少 package 身份")
    number = package.get("number")
    pack_id = package.get("pack_id")
    version = package.get("version")
    display_name = package.get("display_name")
    if not isinstance(number, str) or not re.fullmatch(r"\d{3}", number):
        raise PackError("MEPACK 包编号无效")
    if not isinstance(pack_id, str) or not PACK_ID.fullmatch(pack_id):
        raise PackError("MEPACK pack_id 无效")
    if not isinstance(version, int) or version < 1 or not isinstance(display_name, str) or not display_name:
        raise PackError("MEPACK 名称或内容版本无效")
    expected_filename = f"{number}-{pack_id}-v{version}.mepack"
    if path.name != expected_filename:
        raise PackError(f"MEPACK 文件名应为 {expected_filename}")
    fallback = manifest.get("fallback")
    if not isinstance(fallback, dict) or fallback.get("default_state") != "cruise":
        raise PackError("MEPACK 默认回退状态必须为 cruise")
    manifest_states = manifest.get("states")
    if not isinstance(manifest_states, list) or len(manifest_states) != file_count:
        raise PackError("MEPACK manifest 状态数量与索引不一致")
    by_id = {item.get("id"): item for item in manifest_states if isinstance(item, dict)}
    if len(by_id) != file_count:
        raise PackError("MEPACK manifest 状态缺失或重复")

    seen: set[str] = set()
    ranges: list[tuple[int, int, str]] = []
    for index in range(file_count):
        offset = manifest_end + index * ENTRY_SIZE
        (raw_id, resource_offset, resource_size, resource_hash, fps, entry_flags,
         width, height, frames, entry_reserved) = ENTRY.unpack_from(data, offset)
        try:
            state_id = raw_id.split(b"\0", 1)[0].decode("ascii")
        except UnicodeError as exc:
            raise PackError("MEPACK 状态 ID 不是 ASCII") from exc
        if not STATE_ID.fullmatch(state_id) or state_id in seen:
            raise PackError(f"MEPACK 状态 ID 无效或重复：{state_id}")
        seen.add(state_id)
        if entry_reserved != 0 or entry_flags & ~FLAG_LOOP:
            raise PackError(f"{state_id} 包含不支持的索引标志")
        if resource_offset % 4 or not 1 <= fps <= 63 or (width, height) != (360, 360):
            raise PackError(f"{state_id} 的对齐、FPS 或尺寸无效")
        end = resource_offset + resource_size
        if end > len(payload):
            raise PackError(f"{state_id} payload 越界")
        resource = payload[resource_offset:end]
        if sha256(resource) != resource_hash:
            raise PackError(f"{state_id} EAF SHA-256 无效")
        actual_frames = validate_eaf(resource, width, height)
        record = by_id[state_id]
        expected = {
            "offset": resource_offset, "size": resource_size,
            "sha256": resource_hash.hex(), "width": width, "height": height,
            "frames": frames, "fps": fps, "loop": bool(entry_flags & FLAG_LOOP),
        }
        for key, value in expected.items():
            if record.get(key) != value:
                raise PackError(f"{state_id} 的 manifest/index 字段 {key} 不一致")
        if actual_frames != frames:
            raise PackError(f"{state_id} 的 EAF/index 帧数不一致")
        if record.get("source") != f"car_{state_id}.eaf":
            raise PackError(f"{state_id} 的源文件名无效")
        ranges.append((resource_offset, end, state_id))

    required = {"cruise", "idle_01", "idle_02", "idle_03"}
    missing = sorted(required - seen)
    if missing and manifest.get("legacy_missing_idle_allowed") is not True:
        raise PackError("MEPACK 缺少必备状态：" + ", ".join(missing))
    ranges.sort()
    cursor = 0
    for start, end, state_id in ranges:
        if start < cursor:
            raise PackError(f"{state_id} payload 与前一资源重叠")
        if any(payload[cursor:start]):
            raise PackError(f"{state_id} 前的 payload 对齐填充必须为零")
        cursor = end
    if any(payload[cursor:]):
        raise PackError("MEPACK payload 尾部填充必须为零")

    return {
        "file": str(path.resolve()),
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "schema_version": schema,
        "states": file_count,
        "payload_bytes": payload_size,
        "identity": package,
        "signature": manifest.get("signature"),
    }


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="从车载表情素材母版生成或校验 MEPACK v1")
    commands = root.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("pack", help="从 NNN-名称-vN 素材母版生成 .mepack")
    pack.add_argument("source", type=Path, help="素材母版目录")
    pack.add_argument("--output", type=Path, help="输出 .mepack；默认写入同级设备资源包目录")
    pack.add_argument("--pack-id", help="覆盖 README.md 中登记的 pack_id")
    pack.add_argument("--board", default="CyberVoc-Board-V2_0", help="目标板型")
    pack.add_argument("--min-firmware", default=None, help="最低兼容固件；测试包可省略")
    pack.add_argument("--allow-legacy-missing-idle", action="store_true", help="仅用于已登记的历史包")
    verify = commands.add_parser("verify", help="完整回读并校验 .mepack")
    verify.add_argument("package", type=Path)
    return root


def main() -> int:
    args = parser().parse_args()
    try:
        result = build_package(args) if args.command == "pack" else verify_package(args.package)
    except (OSError, PackError, struct.error) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
