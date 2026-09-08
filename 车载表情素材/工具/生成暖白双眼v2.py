#!/usr/bin/env python3
"""从 003 暖白双眼 v2 展示稿生成标准 GIF/EAF 交付资源。

设计稿是带圆形原型屏的 4×4 展示海报，不是可直接播放的精灵表。本脚本
按固定圆心提取 16 个状态，去除海报背景和圆形边框，缩放为 360×360，
再生成低帧数 GIF 与 EAF 8-bit 索引色 RLE 资源。三种待机动画由巡航姿态
参数化生成，保持和固定状态资源相同的画布、调色板和 EAF 结构。
"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / "003-暖白双眼-v2"
SOURCE = ROOT / "源素材" / "003-暖白双眼-v2" / "v2设计图.png"

WIDTH = 360
HEIGHT = 360
CELL_SIZE = 220
CIRCLE_RADIUS = 104.0
SAFE_RADIUS = 164.0
BLOCK_HEIGHT = 30
BLOCKS = HEIGHT // BLOCK_HEIGHT
PALETTE_COLORS = 256

DESIGN_STATES = (
    ("wake", 0, 0),
    ("move_off", 0, 1),
    ("accelerate", 0, 2),
    ("cruise", 0, 3),
    ("turn_left", 1, 0),
    ("turn_right", 1, 1),
    ("turn_end", 1, 2),
    ("reverse", 1, 3),
    ("decelerate", 2, 0),
    ("hard_brake", 2, 1),
    ("bump", 2, 2),
    ("wait", 2, 3),
    ("arrive", 3, 0),
    ("sleep", 3, 1),
    ("night", 3, 2),
    ("signal_lost", 3, 3),
)

# The poster uses four columns with a small amount of optical, rather than
# mathematical, centering. These are measured from the source image's circles.
COL_CENTERS = (147, 400, 655, 909)
ROW_CENTERS = (254, 563, 868, 1177)

STATE_DURATIONS = {
    "wake": (80, 80, 100, 160),
    "move_off": (80, 80, 120, 160),
    "accelerate": (80, 100, 140, 220),
    "cruise": (300, 160, 300, 160),
    "turn_left": (80, 100, 240, 240),
    "turn_right": (80, 100, 240, 240),
    "turn_end": (80, 100, 180, 220),
    "reverse": (100, 120, 260, 260),
    "decelerate": (80, 100, 180, 220),
    "hard_brake": (70, 100, 180, 260),
    "bump": (70, 100, 160, 220),
    "wait": (360, 240, 360, 240),
    "arrive": (80, 100, 220, 320),
    "sleep": (500, 300, 500, 300),
    "night": (500, 300, 500, 300),
    "signal_lost": (360, 240, 360, 240),
}

LOOP_RECOMMENDED = {
    "cruise",
    "wait",
    "sleep",
    "night",
    "idle_01",
    "idle_02",
    "idle_03",
}


def crop_design(image: Image.Image, center_x: int, center_y: int) -> Image.Image:
    """Extract one circular prototype screen and normalize its background."""

    half = CELL_SIZE // 2
    left = int(round(center_x - half))
    top = int(round(center_y - half))
    crop = image.crop((left, top, left + CELL_SIZE, top + CELL_SIZE)).convert("RGB")
    pixels = crop.load()
    center = (CELL_SIZE - 1) / 2.0
    radius_sq = CIRCLE_RADIUS * CIRCLE_RADIUS
    for y in range(CELL_SIZE):
        dy = y - center
        for x in range(CELL_SIZE):
            dx = x - center
            r, g, b = pixels[x, y]
            # Remove the poster's gray background and the thin prototype-screen
            # outline. The actual expression remains comfortably inside this mask.
            if dx * dx + dy * dy > radius_sq or max(r, g, b) < 4:
                pixels[x, y] = (0, 0, 0)
    resized = crop.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    return apply_safe_mask(resized)


def apply_safe_mask(image: Image.Image) -> Image.Image:
    """Keep expression pixels inside the 360×360 circular-screen safety zone."""

    pixels = image.load()
    radius_sq = SAFE_RADIUS * SAFE_RADIUS
    for y in range(HEIGHT):
        dy = y - 180
        for x in range(WIDTH):
            dx = x - 180
            r, g, b = pixels[x, y]
            if dx * dx + dy * dy > radius_sq or max(r, g, b) < 4:
                pixels[x, y] = (0, 0, 0)
    return image


def place_scaled(image: Image.Image, scale_x: float = 1.0, scale_y: float = 1.0,
                 dx: float = 0.0, dy: float = 0.0, angle: float = 0.0) -> Image.Image:
    """Apply a small centered motion while keeping a pure black canvas."""

    transformed = image
    if angle:
        transformed = transformed.rotate(
            angle,
            resample=Image.Resampling.BICUBIC,
            expand=False,
            fillcolor=(0, 0, 0),
        )
    target_size = (
        max(1, round(WIDTH * scale_x)),
        max(1, round(HEIGHT * scale_y)),
    )
    transformed = transformed.resize(target_size, Image.Resampling.BICUBIC)
    output = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    x = round((WIDTH - target_size[0]) / 2 + dx)
    y = round((HEIGHT - target_size[1]) / 2 + dy)
    output.paste(transformed, (x, y))
    return apply_safe_mask(output)


def state_frames(state: str, art: Image.Image) -> list[Image.Image]:
    """Create a short design-derived clip for one fixed vehicle state."""

    transforms = {
        "wake": ((0.86, 0.86, 0, 4, 0), (0.95, 0.95, 0, 1, 0),
                 (1.00, 1.00, 0, 0, 0), (1.02, 0.98, 0, 0, 0)),
        "move_off": ((0.92, 0.96, 0, 3, 0), (0.98, 1.00, 0, 0, 0),
                     (1.02, 1.00, 0, -1, 0), (1.00, 1.02, 0, 0, 0)),
        "accelerate": ((0.96, 0.96, 0, 3, 0), (1.00, 1.00, 0, 0, 0),
                       (1.03, 1.03, 0, -1, 0), (1.00, 1.00, 0, 0, 0)),
        "cruise": ((0.99, 0.99, 0, 0, 0), (1.00, 1.00, 0, 0, 0),
                   (1.01, 1.00, 0, 1, 0), (1.01, 1.01, 0, 0, 0)),
        "turn_left": ((1.00, 1.00, 4, 0, 0), (1.00, 1.00, -2, 0, 0),
                      (1.00, 1.00, -4, 0, 0), (1.00, 1.00, -2, 0, 0)),
        "turn_right": ((1.00, 1.00, -4, 0, 0), (1.00, 1.00, 2, 0, 0),
                       (1.00, 1.00, 4, 0, 0), (1.00, 1.00, 2, 0, 0)),
        "turn_end": ((0.98, 0.98, -2, 0, 0), (1.00, 1.00, 0, 0, 0),
                     (1.01, 1.01, 1, 0, 0), (1.00, 1.00, 0, 0, 0)),
        "reverse": ((0.98, 0.98, 0, 2, 0), (1.00, 1.00, 0, 0, 0),
                    (1.02, 1.02, 0, -1, 0), (1.00, 1.00, 0, 0, 0)),
        "decelerate": ((1.03, 0.86, 0, 4, 0), (1.00, 0.94, 0, 2, 0),
                       (1.00, 1.00, 0, 0, 0), (1.00, 1.00, 0, -1, 0)),
        "hard_brake": ((1.05, 0.88, 0, 4, 0), (1.02, 0.94, 0, 2, 0),
                       (0.98, 1.03, 0, -2, 0), (1.00, 1.00, 0, 0, 0)),
        "bump": ((1.04, 0.90, 0, 5, 0), (0.96, 1.04, 0, -4, 0),
                 (1.02, 0.98, 0, 2, 0), (1.00, 1.00, 0, 0, 0)),
        "wait": ((1.00, 1.00, 0, 0, 0), (1.00, 1.00, 0, 1, 0),
                 (1.01, 1.00, 0, 0, 0), (1.00, 1.00, 0, -1, 0)),
        "arrive": ((0.90, 0.90, 0, 3, 0), (0.98, 0.98, 0, 1, 0),
                   (1.02, 1.02, 0, -1, 0), (1.00, 1.00, 0, 0, 0)),
        "sleep": ((1.00, 1.00, 0, 0, 0), (0.98, 0.98, 0, 1, 0),
                  (1.01, 1.00, 0, 0, 0), (1.00, 1.00, 0, -1, 0)),
        "night": ((0.99, 0.99, 0, 0, 0), (1.00, 1.00, 0, 0, 0),
                  (1.01, 1.00, 0, 1, 0), (1.01, 1.01, 0, 0, 0)),
        "signal_lost": ((1.00, 0.96, 0, 2, 0), (1.00, 1.00, 0, 0, 0),
                        (1.01, 1.00, 0, 0, 0), (1.00, 0.96, 0, -2, 0)),
    }[state]
    return [place_scaled(art, *params) for params in transforms]


def idle_clips(cruise: Image.Image) -> dict[str, tuple[list[Image.Image], list[int]]]:
    """Create three distinct full-playback idle units from the cruise anchor."""

    idle_01 = [
        place_scaled(cruise),
        place_scaled(cruise, scale_y=0.92),
        place_scaled(cruise, scale_y=0.18),
        place_scaled(cruise, scale_y=0.92),
        place_scaled(cruise),
    ]
    idle_02 = [
        place_scaled(cruise),
        place_scaled(cruise, dx=-5),
        place_scaled(cruise, dx=-8),
        place_scaled(cruise, dx=5),
        place_scaled(cruise),
    ]
    idle_03 = [
        place_scaled(cruise, scale_x=0.98, scale_y=0.98, angle=-1.0),
        place_scaled(cruise, scale_x=1.02, scale_y=1.02, angle=-0.5),
        place_scaled(cruise),
        place_scaled(cruise, scale_x=1.02, scale_y=1.02, angle=0.5),
        place_scaled(cruise, scale_x=0.98, scale_y=0.98, angle=1.0),
    ]
    durations = [600, 100, 100, 100, 600]
    return {
        "idle_01": (idle_01, durations),
        "idle_02": (idle_02, durations),
        "idle_03": (idle_03, durations),
    }


def quantize_frames(frames: list[Image.Image]) -> list[Image.Image]:
    """Quantize one clip with a shared palette to avoid color shimmer."""

    palette_source = Image.new("RGB", (WIDTH, HEIGHT * len(frames)), (0, 0, 0))
    for index, frame in enumerate(frames):
        palette_source.paste(frame, (0, index * HEIGHT))
    palette = palette_source.quantize(
        colors=PALETTE_COLORS,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    return [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]


def rle_encode(indices: bytes) -> bytes:
    output = bytearray()
    position = 0
    while position < len(indices):
        value = indices[position]
        run_end = position + 1
        while run_end < len(indices) and indices[run_end] == value and run_end - position < 255:
            run_end += 1
        output.extend((run_end - position, value))
        position = run_end
    return bytes(output)


def make_eaf(frames: list[Image.Image]) -> bytes:
    eaf_frames: list[bytes] = []
    for frame in quantize_frames(frames):
        indices = bytes(frame.getdata())
        palette_rgb = list(frame.getpalette() or [])
        palette_rgb.extend([0] * (PALETTE_COLORS * 3 - len(palette_rgb)))
        palette_bgra = bytearray()
        for i in range(PALETTE_COLORS):
            r, g, b = palette_rgb[i * 3:i * 3 + 3]
            palette_bgra.extend((b, g, r, 255))

        blocks: list[bytes] = []
        for block_index in range(BLOCKS):
            start = block_index * BLOCK_HEIGHT * WIDTH
            end = start + BLOCK_HEIGHT * WIDTH
            blocks.append(bytes((0,)) + rle_encode(indices[start:end]))
        block_lengths = b"".join(struct.pack("<I", len(block)) for block in blocks)
        frame_payload = (
            b"_S\x00"
            + b"\x00\x00\x00\x00\x00\x01"
            + struct.pack("<BHHHH", 8, WIDTH, HEIGHT, BLOCKS, BLOCK_HEIGHT)
            + block_lengths
            + bytes(palette_bgra)
            + b"".join(blocks)
        )
        eaf_frames.append(b"ZZ" + frame_payload)

    table = bytearray()
    offset = 0
    for frame in eaf_frames:
        table.extend(struct.pack("<II", len(frame), offset))
        offset += len(frame)
    body = bytes(table) + b"".join(eaf_frames)
    return b"\x89EAF" + struct.pack("<III", len(eaf_frames), sum(body) & 0xFFFFFFFF, len(body)) + body


def decode_eaf_indices(data: bytes) -> list[bytes]:
    """Read back the generated EAF independently of the firmware decoder."""

    if data[:4] != b"\x89EAF":
        raise ValueError("EAF magic mismatch")
    count, checksum, length = struct.unpack_from("<III", data, 4)
    if length != len(data) - 16 or checksum != sum(data[16:]) & 0xFFFFFFFF:
        raise ValueError("EAF checksum/length mismatch")
    base = 16 + count * 8
    decoded: list[bytes] = []
    for index in range(count):
        size, offset = struct.unpack_from("<II", data, 16 + index * 8)
        frame = data[base + offset:base + offset + size]
        if len(frame) != size or frame[:2] != b"ZZ" or frame[2:4] != b"_S":
            raise ValueError(f"invalid EAF frame {index}")
        payload = frame[2:]
        bit_depth, width, height, blocks, block_height = struct.unpack_from("<BHHHH", payload, 9)
        if (bit_depth, width, height, blocks, block_height) != (8, WIDTH, HEIGHT, BLOCKS, BLOCK_HEIGHT):
            raise ValueError(f"invalid EAF geometry in frame {index}")
        lengths = [struct.unpack_from("<I", payload, 18 + block * 4)[0] for block in range(blocks)]
        palette_offset = 18 + blocks * 4
        data_offset = palette_offset + PALETTE_COLORS * 4
        cursor = data_offset
        pixels = bytearray()
        for block, block_length in enumerate(lengths):
            block_data = payload[cursor:cursor + block_length]
            cursor += block_length
            if not block_data or block_data[0] != 0:
                raise ValueError(f"invalid RLE block {index}:{block}")
            for run, value in zip(block_data[1::2], block_data[2::2]):
                pixels.extend([value] * run)
            expected = WIDTH * min(BLOCK_HEIGHT, HEIGHT - block * BLOCK_HEIGHT)
            if len(pixels) != WIDTH * BLOCK_HEIGHT * (block + 1):
                raise ValueError(f"RLE length mismatch in frame {index}:{block}")
            if len(pixels) < WIDTH * BLOCK_HEIGHT * block + expected:
                raise ValueError(f"RLE block too short in frame {index}:{block}")
        if len(pixels) != WIDTH * HEIGHT or cursor != len(payload):
            raise ValueError(f"unexpected EAF payload length in frame {index}")
        decoded.append(bytes(pixels))
    return decoded


def save_gif(path: Path, frames: list[Image.Image], durations: list[int]) -> None:
    quantized = quantize_frames(frames)
    quantized[0].save(
        path,
        format="GIF",
        save_all=True,
        append_images=quantized[1:],
        duration=durations,
        loop=0,
        disposal=2,
        optimize=False,
    )


def write_clip(state: str, frames: list[Image.Image], durations: list[int]) -> dict:
    gif_path = PACKAGE / "gif" / f"car_{state}.gif"
    eaf_path = PACKAGE / "eaf" / f"car_{state}.eaf"
    save_gif(gif_path, frames, durations)
    eaf_data = make_eaf(frames)
    eaf_path.write_bytes(eaf_data)
    decoded = decode_eaf_indices(eaf_data)
    expected = [bytes(quantized.getdata()) for quantized in quantize_frames(frames)]
    if decoded != expected:
        raise ValueError(f"EAF RLE readback mismatch: {state}")
    return {
        "source": gif_path.name,
        "output": eaf_path.name,
        "width": WIDTH,
        "height": HEIGHT,
        "frames": len(frames),
        "duration_ms": durations,
        "size_bytes": len(eaf_data),
        "sha256": hashlib.sha256(eaf_data).hexdigest(),
        "encoding": "EAF 8-bit indexed BGRA palette + RLE, 12x30-row blocks",
        "state": f"car_{state}",
        "loop_recommended": state in LOOP_RECOMMENDED,
    }


def main() -> None:
    if not SOURCE.is_file():
        raise FileNotFoundError(f"找不到设计源图：{SOURCE}")
    source_image = Image.open(SOURCE).convert("RGB")
    if source_image.size != (1055, 1491):
        raise ValueError(f"设计图尺寸变化，需重新确认提取坐标：{source_image.size}")

    PACKAGE.mkdir(parents=True, exist_ok=True)
    gif_dir = PACKAGE / "gif"
    eaf_dir = PACKAGE / "eaf"
    gif_dir.mkdir(exist_ok=True)
    eaf_dir.mkdir(exist_ok=True)
    for path in gif_dir.glob("car_*.gif"):
        path.unlink()
    for path in eaf_dir.glob("car_*.eaf"):
        path.unlink()

    art: dict[str, Image.Image] = {}
    for state, row, column in DESIGN_STATES:
        art[state] = crop_design(source_image, COL_CENTERS[column], ROW_CENTERS[row])

    records: list[dict] = []
    for state, _, _ in DESIGN_STATES:
        records.append(write_clip(state, state_frames(state, art[state]), list(STATE_DURATIONS[state])))

    for state, frames_and_durations in idle_clips(art["cruise"]).items():
        frames, durations = frames_and_durations
        records.append(write_clip(state, frames, durations))

    # Manifest order follows the display order used by the preview generator.
    order = [
        "wake", "move_off", "accelerate", "cruise", "idle_01", "idle_02", "idle_03",
        "turn_left", "turn_right", "turn_end", "reverse", "decelerate", "hard_brake",
        "bump", "wait", "arrive", "sleep", "night", "signal_lost",
    ]
    records.sort(key=lambda item: order.index(item["state"].removeprefix("car_")))
    (eaf_dir / "manifest.json").write_text(
        json.dumps(records, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"source_sha256={hashlib.sha256(SOURCE.read_bytes()).hexdigest()}")
    print(f"generated={len(records)} clips")
    print(f"eaf_bytes={sum(item['size_bytes'] for item in records)}")


if __name__ == "__main__":
    main()
