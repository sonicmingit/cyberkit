#!/usr/bin/env python3
"""为标准车载表情包生成中文动态总览和静态总览。

依赖 Pillow。默认扫描“车载表情素材”根目录中名称符合
“三位编号-表情包名称-v版本号”的目录。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageSequence


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_NAME = re.compile(r"^\d{3}-.+-v[1-9]\d*$")
ASSET_NAME = re.compile(r"^car_([a-z0-9]+(?:_[a-z0-9]+)*)$")

STATE_ORDER = (
    "wake",
    "move_off",
    "accelerate",
    "cruise",
    "idle_01",
    "idle_02",
    "idle_03",
    "turn_left",
    "turn_right",
    "turn_end",
    "reverse",
    "decelerate",
    "hard_brake",
    "bump",
    "wait",
    "arrive",
    "sleep",
    "night",
    "signal_lost",
)

STATE_LABELS = {
    "wake": "唤醒",
    "move_off": "起步",
    "accelerate": "加速",
    "cruise": "巡航",
    "idle_01": "待机 1",
    "idle_02": "待机 2",
    "idle_03": "待机 3",
    "turn_left": "左转",
    "turn_right": "右转",
    "turn_end": "回正",
    "reverse": "倒车",
    "decelerate": "减速",
    "hard_brake": "急刹",
    "bump": "颠簸",
    "wait": "等候/校准",
    "arrive": "停车",
    "sleep": "休眠",
    "night": "夜间",
    "signal_lost": "信号中断",
}

# 静态总览选取最能表达动作的代表姿态，而不是机械地全部取首帧。
REPRESENTATIVE_POSITION = {
    "wake": 0.65,
    "move_off": 0.65,
    "accelerate": 0.55,
    "cruise": 0.0,
    "idle_01": 0.5,
    "idle_02": 0.5,
    "idle_03": 0.5,
    "turn_left": 0.70,
    "turn_right": 0.70,
    "turn_end": 0.45,
    "reverse": 0.50,
    "decelerate": 0.55,
    "hard_brake": 0.45,
    "bump": 0.35,
    "wait": 0.50,
    "arrive": 0.55,
    "sleep": 0.75,
    "night": 0.30,
    "signal_lost": 0.55,
}

TILE_WIDTH = 180
TILE_HEIGHT = 210
ART_SIZE = 160
FONT_SIZE = 20
MAX_PREVIEW_FRAMES = 80


@dataclass
class Clip:
    state: str
    path: Path
    frames: list[Image.Image]
    durations_ms: list[int]

    @property
    def total_ms(self) -> int:
        return sum(self.durations_ms)

    def frame_at(self, elapsed_ms: int) -> Image.Image:
        cursor = elapsed_ms % self.total_ms
        for frame, duration in zip(self.frames, self.durations_ms):
            if cursor < duration:
                return frame
            cursor -= duration
        return self.frames[-1]

    def representative_frame(self) -> Image.Image:
        position = REPRESENTATIVE_POSITION[self.state]
        index = round(position * (len(self.frames) - 1))
        return self.frames[index]


def state_from_path(path: Path) -> str:
    match = ASSET_NAME.fullmatch(path.stem)
    if not match:
        raise ValueError(f"素材文件名不符合 car_<state> 规则：{path.name}")
    state = match.group(1)
    if state not in STATE_LABELS:
        raise ValueError(f"未登记中文名称的状态：{state}")
    return state


def load_clip(path: Path) -> Clip:
    frames: list[Image.Image] = []
    durations: list[int] = []
    with Image.open(path) as source:
        default_duration = int(source.info.get("duration", 100) or 100)
        for frame in ImageSequence.Iterator(source):
            rendered = frame.convert("RGB")
            if rendered.size != (360, 360):
                raise ValueError(f"GIF 必须为 360x360：{path}，实际 {rendered.size}")
            frames.append(rendered.copy())
            duration = int(frame.info.get("duration", default_duration) or default_duration)
            durations.append(max(20, duration))
    if not frames:
        raise ValueError(f"GIF 没有可用帧：{path}")
    return Clip(state_from_path(path), path, frames, durations)


def find_font(explicit: Path | None) -> ImageFont.FreeTypeFont:
    candidates = [
        explicit,
        Path("C:/Windows/Fonts/msyhbd.ttc"),
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/simhei.ttf"),
        Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"),
        Path("/System/Library/Fonts/PingFang.ttc"),
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return ImageFont.truetype(str(candidate), FONT_SIZE)
    raise FileNotFoundError("找不到支持中文的字体；请通过 --font 指定字体文件")


def prepare_art(frame: Image.Image) -> Image.Image:
    return frame.resize((ART_SIZE, ART_SIZE), Image.Resampling.LANCZOS)


def draw_centered_label(
    draw: ImageDraw.ImageDraw,
    label: str,
    tile_x: int,
    tile_y: int,
    font: ImageFont.FreeTypeFont,
) -> None:
    box = draw.textbbox((0, 0), label, font=font)
    width = box[2] - box[0]
    x = tile_x + (TILE_WIDTH - width) // 2 - box[0]
    y = tile_y + 178 - box[1]
    draw.text((x, y), label, font=font, fill=(245, 245, 245))


def render_sheet(
    clips: list[Clip],
    font: ImageFont.FreeTypeFont,
    elapsed_ms: int | None,
) -> Image.Image:
    columns = min(4, len(clips))
    rows = math.ceil(len(clips) / columns)
    sheet = Image.new("RGB", (columns * TILE_WIDTH, rows * TILE_HEIGHT), "black")
    draw = ImageDraw.Draw(sheet)
    for index, clip in enumerate(clips):
        column = index % columns
        row = index // columns
        tile_x = column * TILE_WIDTH
        tile_y = row * TILE_HEIGHT
        frame = clip.representative_frame() if elapsed_ms is None else clip.frame_at(elapsed_ms)
        sheet.paste(prepare_art(frame), (tile_x + (TILE_WIDTH - ART_SIZE) // 2, tile_y + 5))
        draw_centered_label(draw, STATE_LABELS[clip.state], tile_x, tile_y, font)
    return sheet


def validate_counterparts(package: Path, clips: list[Clip]) -> None:
    gif_stems = {clip.path.stem for clip in clips}
    eaf_dir = package / "eaf"
    if not eaf_dir.is_dir():
        raise FileNotFoundError(f"缺少 eaf 目录：{package}")
    eaf_files = sorted(eaf_dir.glob("*.eaf"))
    eaf_stems = {path.stem for path in eaf_files}
    if gif_stems != eaf_stems:
        missing_eaf = sorted(gif_stems - eaf_stems)
        missing_gif = sorted(eaf_stems - gif_stems)
        raise ValueError(
            f"GIF/EAF 未一一对应：{package.name}；"
            f"缺 EAF={missing_eaf}，缺 GIF={missing_gif}"
        )
    eaf_data: dict[str, bytes] = {}
    for path in eaf_files:
        state_from_path(path)
        data = path.read_bytes()
        eaf_data[path.name] = data
        if data[:4] != b"\x89EAF":
            raise ValueError(f"EAF 文件头无效：{path}")

    manifest_path = eaf_dir / "manifest.json"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"缺少 EAF 清单：{manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    records = {item["output"]: item for item in manifest}
    if len(records) != len(manifest):
        raise ValueError(f"manifest.json 存在重复 output：{package.name}")
    outputs = set(records)
    expected = {path.name for path in eaf_files}
    if outputs != expected:
        raise ValueError(
            f"manifest.json 与 EAF 文件不一致：{package.name}；"
            f"清单缺失={sorted(expected - outputs)}，多余={sorted(outputs - expected)}"
        )
    for filename, data in eaf_data.items():
        record = records[filename]
        expected_source = Path(filename).with_suffix(".gif").name
        checks = {
            "source": expected_source,
            "width": 360,
            "height": 360,
            "frames": int.from_bytes(data[4:8], "little"),
            "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        for field, expected_value in checks.items():
            if record.get(field) != expected_value:
                raise ValueError(
                    f"manifest.json 的 {filename}.{field} 不一致："
                    f"记录={record.get(field)!r}，实际={expected_value!r}"
                )


def generate_package(package: Path, font: ImageFont.FreeTypeFont) -> None:
    allowed_root_items = {
        "eaf",
        "gif",
        "README.md",
        "全部表情预览.gif",
        "全部静态表情预览.png",
    }
    unexpected = sorted(path.name for path in package.iterdir() if path.name not in allowed_root_items)
    if unexpected:
        raise ValueError(f"表情包根目录存在非标准项目：{package.name}：{unexpected}")
    if not (package / "README.md").is_file():
        raise FileNotFoundError(f"缺少单包 README.md：{package}")
    gif_dir = package / "gif"
    if not gif_dir.is_dir():
        raise FileNotFoundError(f"缺少 gif 目录：{package}")
    clips_by_state = {clip.state: clip for clip in map(load_clip, sorted(gif_dir.glob("*.gif")))}
    if not clips_by_state:
        raise ValueError(f"gif 目录为空：{gif_dir}")
    clips = [clips_by_state[state] for state in STATE_ORDER if state in clips_by_state]
    if len(clips) != len(clips_by_state):
        raise ValueError(f"存在未纳入固定排序的状态：{package.name}")
    validate_counterparts(package, clips)

    static_sheet = render_sheet(clips, font, elapsed_ms=None)
    static_sheet.save(package / "全部静态表情预览.png", format="PNG", optimize=True)

    total_ms = max(clip.total_ms for clip in clips)
    source_step = min(duration for clip in clips for duration in clip.durations_ms)
    step_ms = max(50, source_step)
    if math.ceil(total_ms / step_ms) > MAX_PREVIEW_FRAMES:
        step_ms = math.ceil(total_ms / MAX_PREVIEW_FRAMES / 10) * 10
    timeline = list(range(0, max(total_ms, step_ms), step_ms))
    preview_frames = [
        render_sheet(clips, font, elapsed_ms).convert(
            "P", palette=Image.Palette.ADAPTIVE, colors=256
        )
        for elapsed_ms in timeline
    ]
    preview_frames[0].save(
        package / "全部表情预览.gif",
        format="GIF",
        save_all=True,
        append_images=preview_frames[1:],
        duration=step_ms,
        loop=0,
        disposal=2,
        optimize=True,
    )
    print(
        f"{package.name}: {len(clips)} 个状态，"
        f"动态总览 {len(preview_frames)} 帧 @ {step_ms}ms，静态总览已生成"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "packages",
        nargs="*",
        help="要处理的表情包目录名；省略时处理全部标准编号目录",
    )
    parser.add_argument("--font", type=Path, help="支持中文的 TrueType/OpenType 字体")
    args = parser.parse_args()

    if args.packages:
        packages = [ROOT / name for name in args.packages]
    else:
        packages = sorted(
            path for path in ROOT.iterdir() if path.is_dir() and PACKAGE_NAME.fullmatch(path.name)
        )
    if not packages:
        raise SystemExit("没有找到标准表情包目录")
    for package in packages:
        if not package.is_dir() or not PACKAGE_NAME.fullmatch(package.name):
            raise ValueError(f"表情包目录名不符合规则：{package}")

    font = find_font(args.font)
    for package in packages:
        generate_package(package, font)


if __name__ == "__main__":
    main()
