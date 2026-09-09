#!/usr/bin/env python3
"""从已确认的 NOMI 动画方案导出正式 GIF/EAF 资源。"""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path

from PIL import Image, ImageSequence


HERE = Path(__file__).resolve().parent
ASSET_ROOT = HERE.parents[1]
PACKAGE = ASSET_ROOT / "005-NOMI表情-v1"
SOURCE_IMAGE = ASSET_ROOT / "005-NOMI表情" / "设计稿.png"
PREVIEW_RENDERER = HERE / "生成动态方案预览.py"
EAF_TOOL_PATH = ASSET_ROOT / "工具" / "生成暖白双眼v2.py"

FRAME_COUNT = 120
FRAME_DURATION_MS = 50
EAF_SAMPLE_STEP = 2
EAF_ACTIVE_COLORS = 24
STATE_ORDER = (
    "wake", "move_off", "accelerate", "cruise",
    "idle_01", "idle_02", "idle_03",
    "turn_left", "turn_right", "turn_end", "reverse",
    "decelerate", "hard_brake", "bump", "wait",
    "arrive", "sleep", "night", "signal_lost",
)
LOOP_STATES = {"cruise", "idle_01", "idle_02", "idle_03", "wait", "sleep", "night"}


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载模块：{path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def quantize_eaf_frames(frames):
    source = Image.new("RGB", (360, 360 * len(frames)), (0, 0, 0))
    for index, frame in enumerate(frames):
        source.paste(frame.convert("RGB"), (0, index * 360))
    palette = source.quantize(
        colors=EAF_ACTIVE_COLORS,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    return [
        frame.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE)
        for frame in frames
    ]


def save_gif(path: Path, frames):
    source = Image.new("RGB", (360, 360 * len(frames)), (0, 0, 0))
    for index, frame in enumerate(frames):
        source.paste(frame.convert("RGB"), (0, index * 360))
    palette = source.quantize(
        colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
    )
    quantized = [
        frame.convert("RGB").quantize(palette=palette, dither=Image.Dither.NONE)
        for frame in frames
    ]
    quantized[0].save(
        path,
        format="GIF",
        save_all=True,
        append_images=quantized[1:],
        duration=FRAME_DURATION_MS,
        loop=0,
        disposal=2,
        optimize=False,
    )


def main() -> None:
    renderer = load_module(PREVIEW_RENDERER, "nomi_preview_renderer")
    eaf_tool = load_module(EAF_TOOL_PATH, "nomi_eaf_tool")
    eaf_tool.quantize_frames = quantize_eaf_frames
    gif_dir = PACKAGE / "gif"
    eaf_dir = PACKAGE / "eaf"
    gif_dir.mkdir(parents=True, exist_ok=True)
    eaf_dir.mkdir(parents=True, exist_ok=True)
    for path in gif_dir.glob("car_*.gif"):
        path.unlink()
    for path in eaf_dir.glob("car_*.eaf"):
        path.unlink()

    records = []
    for state in STATE_ORDER:
        frames = [
            renderer.render(state, index * FRAME_DURATION_MS / 1000)
            for index in range(FRAME_COUNT)
        ]
        gif_path = gif_dir / f"car_{state}.gif"
        eaf_path = eaf_dir / f"car_{state}.eaf"
        save_gif(gif_path, frames)

        eaf_frames = frames[::EAF_SAMPLE_STEP]
        eaf_data = eaf_tool.make_eaf(eaf_frames)
        eaf_path.write_bytes(eaf_data)
        decoded = eaf_tool.decode_eaf_indices(eaf_data)
        expected = [bytes(frame.getdata()) for frame in quantize_eaf_frames(eaf_frames)]
        if decoded != expected:
            raise ValueError(f"EAF RLE 回读不一致：{state}")

        with Image.open(gif_path) as gif:
            gif_durations = [
                int(frame.info.get("duration", FRAME_DURATION_MS))
                for frame in ImageSequence.Iterator(gif)
            ]
            if gif.size != (360, 360) or sum(gif_durations) != FRAME_COUNT * FRAME_DURATION_MS:
                raise ValueError(f"GIF 尺寸或时间轴不符合要求：{state}")

        records.append({
            "source": gif_path.name,
            "output": eaf_path.name,
            "width": 360,
            "height": 360,
            "frames": len(eaf_frames),
            "duration_ms": [FRAME_DURATION_MS * EAF_SAMPLE_STEP] * len(eaf_frames),
            "size_bytes": len(eaf_data),
            "sha256": hashlib.sha256(eaf_data).hexdigest(),
            "gif_frames": len(gif_durations),
            "gif_duration_ms": gif_durations,
            "gif_timeline_slots": FRAME_COUNT,
            "gif_timeline_step_ms": FRAME_DURATION_MS,
            "gif_size_bytes": gif_path.stat().st_size,
            "gif_sha256": hashlib.sha256(gif_path.read_bytes()).hexdigest(),
            "eaf_temporal_sample_step": EAF_SAMPLE_STEP,
            "active_palette_colors": EAF_ACTIVE_COLORS,
            "encoding": "EAF 8-bit indexed BGRA palette + RLE, 12x30-row blocks",
            "state": f"car_{state}",
            "loop_recommended": state in LOOP_STATES,
            "end_behavior": "loop" if state in LOOP_STATES else "return_to_current_state",
        })
        print(f"{state}: gif={gif_path.stat().st_size} eaf={len(eaf_data)}")

    (eaf_dir / "manifest.json").write_text(
        json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"generated={len(records)} clips")
    print(f"gif_bytes={sum(item['gif_size_bytes'] for item in records)}")
    print(f"eaf_bytes={sum(item['size_bytes'] for item in records)}")
    print(f"source_sha256={hashlib.sha256(SOURCE_IMAGE.read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
