#!/usr/bin/env python3
"""从已批准的 004 默认表情动作方案生成 GIF/EAF 交付资源。"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageSequence


SOURCE_DIR = Path(__file__).resolve().parent
ASSET_ROOT = SOURCE_DIR.parents[1]
PACKAGE = ASSET_ROOT / "004-默认表情-v1"
GIF_DIR = PACKAGE / "gif"
EAF_DIR = PACKAGE / "eaf"
EAF_TOOL_PATH = ASSET_ROOT / "工具" / "生成暖白双眼v2.py"

WIDTH = 360
HEIGHT = 360
FRAME_COUNT = 48
FRAME_DURATION_MS = 50
EAF_SAMPLE_STEP = 2
EAF_ACTIVE_COLORS = 24

WHITE = (250, 250, 248, 255)
PINK = (255, 118, 185, 255)
YELLOW = (255, 215, 92, 255)
CYAN = (48, 224, 238, 255)
WARM = (255, 223, 153, 255)
BLACK = (0, 0, 0, 255)

STATE_ORDER = (
    "wake", "move_off", "accelerate", "cruise",
    "idle_01", "idle_02", "idle_03",
    "turn_left", "turn_right", "turn_end", "reverse",
    "decelerate", "hard_brake", "bump", "wait",
    "arrive", "sleep", "night", "signal_lost",
)
LOOP_RECOMMENDED = {
    "cruise", "idle_01", "idle_02", "idle_03", "wait", "sleep", "night"
}


def load_eaf_tool():
    spec = importlib.util.spec_from_file_location("car_eaf_tool", EAF_TOOL_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载 EAF 工具：{EAF_TOOL_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def ease(x: float) -> float:
    x = max(0.0, min(1.0, x))
    return x * x * (3.0 - 2.0 * x)


def peak(t: float, center: float, width: float) -> float:
    distance = abs((t - center + 0.5) % 1.0 - 0.5)
    return ease(max(0.0, 1.0 - distance / width))


class Face:
    def __init__(self, size: int = 360, supersample: int = 2):
        self.size = size
        self.ss = supersample
        self.image = Image.new("RGBA", (size * supersample, size * supersample), BLACK)

    def n(self, value: float) -> int:
        return int(round(value * self.ss))

    def layer(self):
        mask = Image.new("L", self.image.size, 0)
        return mask, ImageDraw.Draw(mask)

    def paint(self, mask: Image.Image, color, blur: float = 4.0):
        if blur:
            glow = mask.filter(ImageFilter.GaussianBlur(self.n(blur)))
            glow = glow.point(lambda value: min(120, int(value * 0.45)))
            glow_layer = Image.new("RGBA", self.image.size, color[:3] + (0,))
            glow_layer.putalpha(glow)
            self.image.alpha_composite(glow_layer)
        solid = Image.new("RGBA", self.image.size, color)
        solid.putalpha(mask)
        self.image.alpha_composite(solid)

    def ellipse(self, box, color=WHITE, blur: float = 4.0):
        mask, draw = self.layer()
        draw.ellipse(tuple(self.n(value) for value in box), fill=255)
        self.paint(mask, color, blur)

    def rounded(self, box, radius: float, color=WHITE, blur: float = 4.0):
        mask, draw = self.layer()
        draw.rounded_rectangle(
            tuple(self.n(value) for value in box), radius=self.n(radius), fill=255
        )
        self.paint(mask, color, blur)

    def polygon(self, points, color=WHITE, blur: float = 4.0):
        mask, draw = self.layer()
        draw.polygon([(self.n(x), self.n(y)) for x, y in points], fill=255)
        self.paint(mask, color, blur)

    def line(self, points, width: float = 12.0, color=WHITE, blur: float = 4.0):
        mask, draw = self.layer()
        scaled = [(self.n(x), self.n(y)) for x, y in points]
        line_width = self.n(width)
        draw.line(scaled, fill=255, width=line_width, joint="curve")
        radius = line_width // 2
        for x, y in (scaled[0], scaled[-1]):
            draw.ellipse((x-radius, y-radius, x+radius, y+radius), fill=255)
        self.paint(mask, color, blur)

    def finish(self) -> Image.Image:
        result = self.image.resize(
            (self.size, self.size), Image.Resampling.LANCZOS
        ).convert("RGB")
        pixels = result.load()
        safe_radius_sq = 165 * 165
        for y in range(self.size):
            dy = y - self.size // 2
            for x in range(self.size):
                dx = x - self.size // 2
                if dx*dx + dy*dy > safe_radius_sq:
                    pixels[x, y] = (0, 0, 0)
        return result


def open_eye(c, cx, cy, rx=42, ry=46, pdx=0, pdy=0, pr=16, color=WHITE):
    c.ellipse((cx-rx, cy-ry, cx+rx, cy+ry), color)
    c.ellipse((cx+pdx-pr, cy+pdy-pr, cx+pdx+pr, cy+pdy+pr), BLACK, 0)


def upper_arc(c, cx, cy, width=86, rise=36, thick=15):
    points = []
    for index in range(21):
        u = -1 + index / 10
        points.append((cx + u*width/2, cy + rise*u*u))
    c.line(points, thick)


def lower_arc(c, cx, cy, width=82, dip=24, thick=14):
    points = []
    for index in range(21):
        u = -1 + index / 10
        points.append((cx + u*width/2, cy - dip*u*u))
    c.line(points, thick)


def wedge(c, cx, cy, direction, squeeze=0):
    rx, ry = 48, 37 - 13*squeeze
    tilt = 18
    if direction > 0:
        points = [
            (cx-rx, cy-ry+tilt), (cx+rx, cy-ry-tilt),
            (cx+rx-5, cy+ry), (cx-rx+8, cy+ry-2),
        ]
    else:
        points = [
            (cx-rx, cy-ry-tilt), (cx+rx, cy-ry+tilt),
            (cx+rx-8, cy+ry-2), (cx-rx+5, cy+ry),
        ]
    c.polygon(points)


def droop(c, cx, cy, side, depth=1):
    slope = 18 + 10*depth
    if side < 0:
        points = [
            (cx-46, cy-13), (cx+43, cy-slope),
            (cx+31, cy+19), (cx-34, cy+29),
        ]
    else:
        points = [
            (cx-43, cy-slope), (cx+46, cy-13),
            (cx+34, cy+29), (cx-31, cy+19),
        ]
    c.polygon(points)


def blush(c, y=230, spread=111, width=46, amount=1):
    actual_width, height = width*amount, 23*amount
    for x in (180-spread, 180+spread):
        c.rounded(
            (x-actual_width/2, y-height/2, x+actual_width/2, y+height/2),
            height/2, PINK, 5,
        )


def marks(c, x, y, direction=1, scale=1):
    c.line([(x, y), (x+direction*14*scale, y-40*scale)], 10*scale, YELLOW, 4)
    c.line(
        [(x+direction*22*scale, y+2), (x+direction*49*scale, y-20*scale)],
        9*scale, YELLOW, 4,
    )


def chevrons(c, x, y, direction, travel=0):
    for index in range(2):
        cx = x + direction*(index*22 + travel)
        c.line(
            [(cx-direction*13, y-21), (cx+direction*2, y), (cx-direction*13, y+21)],
            9, CYAN, 4,
        )


def eye_or_blink(c, cx, cy, amount, pdx=0, pdy=0):
    if amount > 0.82:
        upper_arc(c, cx, cy+1, 82, 7, 14)
    else:
        open_eye(c, cx, cy, 42+3*amount, max(7, 46*(1-0.82*amount)), pdx, pdy, 16)


def render(state: str, t: float) -> Image.Image:
    c = Face()
    wave = math.sin(2*math.pi*t)
    pulse = (wave + 1) / 2
    action = peak(t, 0.34, 0.28)

    if state == "wake":
        pop = peak(t, 0.24, 0.30)
        open_eye(c, 123, 181, 39+9*pop, 43+10*pop, pr=9+15*pop)
        open_eye(c, 237, 181, 39+9*pop, 43+10*pop, pr=9+15*pop)
        marks(c, 83, 139, -1, 0.72+0.62*pop)
    elif state == "move_off":
        smile = math.sin(math.pi*t)**2
        if smile < 0.70:
            close = smile/0.70
            eye_or_blink(c, 123, 178, close, pdy=-4*close)
            eye_or_blink(c, 237, 178, close, pdy=-4*close)
        else:
            upper_arc(c, 123, 171, 88, 36, 15)
            upper_arc(c, 237, 171, 88, 36, 15)
        mouth_height = 10 + 20*smile
        c.rounded((166, 207-mouth_height/2, 194, 207+mouth_height/2), 10, WHITE, 4)
        blush(c, 222, 112, 49, 0.55+0.65*smile)
    elif state == "accelerate":
        shift = 20*action
        wedge(c, 120-shift, 182, 1, action)
        wedge(c, 240-shift, 182, -1, action)
        for index, yy in enumerate((139, 176, 213)):
            start = 278 + 8*index + 10*action
            length = 18 + 46*action
            c.line([(start, yy), (min(336, start+length), yy-7+index*7)], 8, WHITE, 3)
    elif state == "cruise":
        blink = peak(t, 0.78, 0.09)
        eye_or_blink(c, 123, 180, blink)
        eye_or_blink(c, 237, 180, blink)
    elif state == "idle_01":
        left = max(peak(t, 0.23, 0.08), peak(t, 0.72, 0.08))
        right = max(peak(t, 0.35, 0.08), peak(t, 0.84, 0.08))
        eye_or_blink(c, 123, 180, left)
        eye_or_blink(c, 237, 180, right)
    elif state == "idle_02":
        look = 30*math.sin(2*math.pi*t)
        open_eye(c, 123, 180, 45, 47, look, -5*abs(wave), 15)
        open_eye(c, 237, 180, 45, 47, look, -5*abs(wave), 15)
    elif state == "idle_03":
        smile = math.sin(math.pi*t)**2
        if smile < 0.58:
            open_eye(c, 123, 179, 42, 46-24*smile, 0, -5*smile, 15)
            open_eye(c, 237, 179, 42, 46-24*smile, 0, -5*smile, 15)
        else:
            upper_arc(c, 123, 172, 88, 37, 15)
            upper_arc(c, 237, 172, 88, 37, 15)
        blush(c, 229, 112, 51, 0.42+0.75*smile)
    elif state == "turn_left":
        look = -27 - 5*pulse
        open_eye(c, 123, 180, 45, 47, look, 0, 15)
        open_eye(c, 237, 180, 45, 47, look, 0, 15)
        chevrons(c, 57, 180, -1, 5+12*pulse)
    elif state == "turn_right":
        look = 27 + 5*pulse
        open_eye(c, 123, 180, 45, 47, look, 0, 15)
        open_eye(c, 237, 180, 45, 47, look, 0, 15)
        chevrons(c, 303, 180, 1, 5+12*pulse)
    elif state == "turn_end":
        look = 31*math.sin(2*math.pi*t)*math.sin(math.pi*t)**2
        open_eye(c, 123, 180, 43, 46, look, 0, 15)
        open_eye(c, 237, 180, 43, 46, look, 0, 15)
    elif state == "reverse":
        drop = 17 + 9*pulse
        spread = 4*wave
        open_eye(c, 123-spread, 177, 44, 47, 0, drop, 16)
        open_eye(c, 237+spread, 177, 44, 47, 0, drop, 16)
        blush(c, 238, 112, 48, 0.75+0.25*pulse)
        marks(c, 276, 139, 1, 0.72+0.40*pulse)
    elif state == "decelerate":
        settle = math.sin(math.pi*t)**2
        upper_arc(c, 123, 176+5*settle, 78+18*settle, 25+18*settle, 15)
        upper_arc(c, 237, 176+5*settle, 78+18*settle, 25+18*settle, 15)
        blush(c, 229+4*settle, 112, 50, 0.70+0.38*settle)
    elif state == "hard_brake":
        rx = 40 + 11*action
        ry = 48 - 27*action
        open_eye(c, 123-8*action, 179+8*action, rx, ry, 0, 0, 13+11*action)
        open_eye(c, 237+8*action, 179+8*action, rx, ry, 0, 0, 13+11*action)
        marks(c, 277, 139, 1, 0.70+0.65*action)
    elif state == "bump":
        shock = math.sin(4*math.pi*t) * (0.65+0.35*math.cos(2*math.pi*t))
        y1, y2 = 180-21*shock, 180+18*shock
        c.line([(74, y1-20), (122, y1+7), (77, y1+34)], 17, WHITE, 5)
        open_eye(c, 238, y2, 46+4*abs(shock), 46-16*abs(shock), 0, -7*shock, 16)
        marks(c, 67, 235, -1, 0.72+0.28*abs(shock))
        marks(c, 286, 231, 1, 0.64+0.30*abs(shock))
    elif state == "wait":
        lift_left = 11*math.sin(2*math.pi*t)
        lift_right = -11*math.sin(2*math.pi*t)
        c.rounded((70, 153+lift_left, 154, 172+lift_left), 8, WHITE, 4)
        c.rounded((206, 153+lift_right, 290, 172+lift_right), 8, WHITE, 4)
        active = int((t*9) % 3)
        for index, x in enumerate((144, 180, 216)):
            bounce = -13 if index == active else 4
            radius = 8 if index == active else 5
            c.ellipse((x-radius, 230+bounce-radius, x+radius, 230+bounce+radius), WHITE, 4)
    elif state == "arrive":
        smile = math.sin(math.pi*t)**2
        if smile < 0.55:
            eye_or_blink(c, 123, 177, smile/0.55)
            eye_or_blink(c, 237, 177, smile/0.55)
        else:
            upper_arc(c, 123, 173, 92, 40, 16)
            upper_arc(c, 237, 173, 92, 40, 16)
        blush(c, 230, 112, 52, 0.42+0.72*smile)
    elif state == "sleep":
        breathe = math.sin(2*math.pi*t)
        lower_arc(c, 123, 194+9*breathe, 88+8*pulse, 28, 15)
        lower_arc(c, 237, 194+9*breathe, 88+8*pulse, 28, 15)
    elif state == "night":
        radius = 31 + 13*pulse
        c.ellipse((123-radius, 180-radius, 123+radius, 180+radius), WARM, 8)
        c.ellipse((237-radius, 180-radius, 237+radius, 180+radius), WARM, 8)
    elif state == "signal_lost":
        sadness = 0.45+0.55*pulse
        droop(c, 123, 188+7*sadness, -1, sadness)
        droop(c, 237, 188+7*sadness, 1, sadness)
        count = 1 + int((t*6) % 3)
        x, y = 278, 135-8*pulse
        for radius in (18, 32, 46)[:count]:
            points = []
            for index in range(10):
                angle = -math.pi/2 + index*(math.pi/2)/9
                points.append((x + radius*math.cos(angle), y + radius*math.sin(angle)))
            c.line(points, 8, YELLOW, 4)
    else:
        raise ValueError(f"未知状态：{state}")
    return c.finish()


def save_gif(path: Path, frames: list[Image.Image], quantize_frames):
    quantized = quantize_frames(frames)
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


def quantize_eaf_frames(frames: list[Image.Image]) -> list[Image.Image]:
    """Use fewer active colors while retaining the firmware's 256-entry table."""

    palette_source = Image.new("RGB", (WIDTH, HEIGHT * len(frames)), (0, 0, 0))
    for index, frame in enumerate(frames):
        palette_source.paste(frame, (0, index * HEIGHT))
    palette = palette_source.quantize(
        colors=EAF_ACTIVE_COLORS,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    return [
        frame.quantize(palette=palette, dither=Image.Dither.NONE)
        for frame in frames
    ]


def main():
    eaf_tool = load_eaf_tool()
    gif_quantize_frames = eaf_tool.quantize_frames
    eaf_tool.quantize_frames = quantize_eaf_frames
    GIF_DIR.mkdir(parents=True, exist_ok=True)
    EAF_DIR.mkdir(parents=True, exist_ok=True)
    for path in GIF_DIR.glob("car_*.gif"):
        path.unlink()
    for path in EAF_DIR.glob("car_*.eaf"):
        path.unlink()

    records = []
    for state in STATE_ORDER:
        frames = [render(state, index/FRAME_COUNT) for index in range(FRAME_COUNT)]
        gif_path = GIF_DIR / f"car_{state}.gif"
        eaf_path = EAF_DIR / f"car_{state}.eaf"
        save_gif(gif_path, frames, gif_quantize_frames)
        with Image.open(gif_path) as gif_image:
            default_duration = int(gif_image.info.get("duration", FRAME_DURATION_MS))
            gif_durations = [
                int(frame.info.get("duration", default_duration))
                for frame in ImageSequence.Iterator(gif_image)
            ]

        eaf_frames = frames[::EAF_SAMPLE_STEP]
        eaf_data = eaf_tool.make_eaf(eaf_frames)
        eaf_path.write_bytes(eaf_data)
        decoded = eaf_tool.decode_eaf_indices(eaf_data)
        expected = [frame.tobytes() for frame in eaf_tool.quantize_frames(eaf_frames)]
        if decoded != expected:
            raise ValueError(f"EAF RLE 回读不一致：{state}")

        records.append({
            "source": gif_path.name,
            "output": eaf_path.name,
            "width": WIDTH,
            "height": HEIGHT,
            "frames": len(eaf_frames),
            "duration_ms": [FRAME_DURATION_MS * EAF_SAMPLE_STEP] * len(eaf_frames),
            "size_bytes": len(eaf_data),
            "sha256": hashlib.sha256(eaf_data).hexdigest(),
            "gif_frames": len(gif_durations),
            "gif_duration_ms": gif_durations,
            "gif_timeline_slots": len(frames),
            "gif_timeline_step_ms": FRAME_DURATION_MS,
            "gif_sha256": hashlib.sha256(gif_path.read_bytes()).hexdigest(),
            "eaf_temporal_sample_step": EAF_SAMPLE_STEP,
            "active_palette_colors": EAF_ACTIVE_COLORS,
            "encoding": "EAF 8-bit indexed BGRA palette + RLE, 12x30-row blocks",
            "state": f"car_{state}",
            "loop_recommended": state in LOOP_RECOMMENDED,
            "end_behavior": "loop" if state in LOOP_RECOMMENDED else "return_to_current_state",
        })
        print(f"generated car_{state}: gif={gif_path.stat().st_size}, eaf={len(eaf_data)}")

    manifest_path = EAF_DIR / "manifest.json"
    manifest_path.write_text(
        json.dumps(records, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"clips={len(records)}")
    print(f"gif_bytes={sum((GIF_DIR / item['source']).stat().st_size for item in records)}")
    print(f"eaf_bytes={sum(item['size_bytes'] for item in records)}")
    print(f"source_sha256={hashlib.sha256((SOURCE_DIR / '设计稿.png').read_bytes()).hexdigest()}")


if __name__ == "__main__":
    main()
