"""Headless Blender VSE trailer editor.

Run:
  blender -b -P tools/edit_trailer.py -- \
    --timeline trailer/edit_60.json \
    --captures captures/raw \
    --preview

The script builds a single VSE timeline from captured PNG sequences, adds
lower-thirds and the benchmark card, then renders a video-only preview/master.
No audio is used in this pass.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

import bpy


TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = TOOLS_DIR.parent
FONT_SANS = Path(r"C:\Windows\Fonts\bahnschrift.ttf")
FONT_MONO = Path(r"C:\Windows\Fonts\consola.ttf")
PNG_RE = re.compile(r"(\d+)\.png$", re.IGNORECASE)


def parse_args() -> argparse.Namespace:
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1 :]
    else:
        argv = []
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeline", default="trailer/edit_60.json")
    parser.add_argument("--captures", default="captures/raw")
    parser.add_argument("--output", default="")
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--vertical", action="store_true")
    parser.add_argument("--resolution-percentage", type=int, default=0)
    return parser.parse_args(argv)


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def resolve_path(path: str) -> Path:
    p = Path(path)
    return p if p.is_absolute() else PROJECT_ROOT / p


def read_marker(shot_dir: Path) -> tuple[int, int]:
    marker_path = shot_dir / "marker.txt"
    if not marker_path.exists():
        return 0, 60
    lines = marker_path.read_text(encoding="utf-8").splitlines()
    marker = int(lines[0]) if lines else 0
    source_fps = int(float(lines[2])) if len(lines) > 2 else 60
    return marker, source_fps


def sorted_pngs(shot_dir: Path) -> list[Path]:
    files = [p for p in shot_dir.glob("*.png") if PNG_RE.search(p.name)]
    files.sort(key=lambda p: int(PNG_RE.search(p.name).group(1)))
    return files


def choose_elements(
    files: list[Path],
    marker: int,
    source_fps: int,
    timeline_fps: int,
    source_in: float,
    duration: float,
    speed: float,
) -> list[Path]:
    if not files:
        raise RuntimeError("No PNG frames")
    start_at = max(0, marker + 1)
    count = max(1, int(round(duration * timeline_fps)))
    elements: list[Path] = []
    for i in range(count):
        source_time = source_in + (i / max(1, timeline_fps)) * max(0.01, speed)
        index = start_at + int(round(source_time * source_fps))
        index = max(0, min(len(files) - 1, index))
        elements.append(files[index])
    return elements


def make_image_strip(se, name: str, files: list[Path], channel: int, frame_start: int, render_w: int, render_h: int):
    first = files[0]
    strip = se.strips.new_image(name, str(first), channel, frame_start, fit_method="FILL")
    strip.directory = str(first.parent) + os.sep
    for path in files[1:]:
        strip.elements.append(path.name)
    try:
        image = bpy.data.images.load(str(first), check_existing=True)
        src_w, src_h = image.size
    except Exception:
        src_w, src_h = 0, 0
    if src_w > 0 and src_h > 0:
        scale = max(render_w / float(src_w), render_h / float(src_h))
        strip.transform.scale_x = scale
        strip.transform.scale_y = scale
    return strip


def make_color_strip(se, name: str, channel: int, frame_start: int, frames: int, color):
    strip = se.strips.new_effect(name, "COLOR", channel, frame_start, length=frames)
    strip.color = tuple(color[:3])
    return strip


def make_text_strip(se, index: int, text: dict, frame_start: int, frames: int, height: int, vertical: bool):
    strip = se.strips.new_effect(f"text_{index}", "TEXT", 3, frame_start, length=frames)
    strip.text = str(text.get("text", ""))
    strip.font_size = max(18, int(height * float(text.get("size", 0.035))))
    strip.location = (
        float(text.get("x", 0.06)),
        float(text.get("y", 0.12)),
    )
    anchor = str(text.get("anchor", "left")).lower()
    valign = str(text.get("valign", "bottom")).lower()
    strip.anchor_x = {"left": "LEFT", "center": "CENTER", "right": "RIGHT"}.get(anchor, "LEFT")
    strip.anchor_y = {"top": "TOP", "center": "CENTER", "bottom": "BOTTOM"}.get(valign, "BOTTOM")
    strip.alignment_x = {"left": "LEFT", "center": "CENTER", "right": "RIGHT"}.get(anchor, "LEFT")
    strip.color = tuple(text.get("color", [1.0, 1.0, 1.0, 1.0]))
    strip.use_shadow = True
    strip.shadow_color = (0.0, 0.0, 0.0, 0.82)
    strip.shadow_offset = 0.035
    strip.shadow_blur = 0.02
    strip.use_outline = True
    strip.outline_color = (0.0, 0.0, 0.0, 0.7)
    strip.outline_width = 0.025
    if bool(text.get("box", False)):
        strip.use_box = True
        strip.box_color = (0.015, 0.02, 0.025, 0.62)
        strip.box_margin = 0.012
        strip.box_roundness = 0.15
    font_key = str(text.get("font", "sans")).lower()
    font_file = FONT_MONO if font_key == "mono" else FONT_SANS
    if font_file.exists():
        try:
            strip.font = bpy.data.fonts.load(str(font_file), check_existing=True)
        except Exception:
            pass
    strip.blend_type = "ALPHA_OVER"
    strip.blend_alpha = 1.0
    return strip


def configure_output(scene, width: int, height: int, fps: int, output: Path, preview: bool, pct: int):
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = pct
    scene.render.fps = fps
    scene.render.use_file_extension = False
    scene.render.filepath = str(output)
    scene.render.image_settings.media_type = "VIDEO"
    scene.render.image_settings.file_format = "FFMPEG"
    scene.render.ffmpeg.audio_codec = "NONE"
    if preview:
        scene.render.ffmpeg.format = "MPEG4"
        scene.render.ffmpeg.codec = "H264"
        scene.render.ffmpeg.constant_rate_factor = "HIGH"
        scene.render.ffmpeg.ffmpeg_preset = "GOOD"
    else:
        scene.render.ffmpeg.format = "QUICKTIME"
        scene.render.ffmpeg.codec = "PRORES"
        scene.render.ffmpeg.constant_rate_factor = "PERC_LOSSLESS"
        scene.render.ffmpeg.ffmpeg_preset = "BEST"


def main():
    args = parse_args()
    timeline_path = resolve_path(args.timeline)
    captures_root = resolve_path(args.captures)
    timeline = load_json(timeline_path)
    fps = int(timeline.get("fps", 60))
    width, height = timeline.get("resolution", [3840, 2160])
    if args.vertical:
        width, height = timeline.get("vertical_resolution", [1080, 1920])
    preview_pct = args.resolution_percentage or (50 if args.preview else 100)

    if not args.output:
        stem = timeline_path.stem
        suffix = "_vertical" if args.vertical else ""
        ext = ".mp4" if args.preview else ".mov"
        output = PROJECT_ROOT / "captures" / "edit" / f"{stem}{suffix}_{'preview' if args.preview else 'master'}{ext}"
    else:
        output = resolve_path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    scene = bpy.context.scene
    scene.sequence_editor_create()
    se = scene.sequence_editor
    scene.render.use_sequencer = True
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0

    strips = timeline.get("strips", [])
    if not strips:
        raise RuntimeError("Timeline has no strips")

    total_seconds = max(float(s.get("start", 0.0)) + float(s.get("duration", 0.0)) for s in strips)
    total_seconds = max(total_seconds, float(timeline.get("duration", total_seconds)))
    total_frames = max(1, int(round(total_seconds * fps)))

    # Black base under every shot.
    make_color_strip(se, "black_base", 1, 1, total_frames, (0.005, 0.007, 0.012, 1.0))

    stagger = 0
    for index, entry in enumerate(strips):
        start = float(entry.get("start", 0.0))
        duration = float(entry.get("duration", 1.0))
        frame_start = 1 + int(round(start * fps))
        frames = max(1, int(round(duration * fps)))

        if entry.get("type") == "color" or "color" in entry:
            make_color_strip(se, f"color_{index}", 2, frame_start, frames, entry["color"])
            continue

        if entry.get("type") == "image" or "image" in entry:
            image_path = resolve_path(entry.get("image", ""))
            if not image_path.exists():
                raise RuntimeError(f"Missing image: {image_path}")
            elements = [image_path] * frames
            make_image_strip(se, f"image_{index}", elements, 2, frame_start, int(width), int(height))
            continue

        shot = str(entry.get("shot", ""))
        shot_dir = captures_root / shot
        files = sorted_pngs(shot_dir)
        if not files:
            raise RuntimeError(f"No captures for shot {shot} in {shot_dir}")
        marker, source_fps = read_marker(shot_dir)
        elements = choose_elements(
            files,
            marker,
            source_fps,
            fps,
            float(entry.get("source_in", 0.0)),
            duration,
            float(entry.get("speed", 1.0)),
        )
        scale = float(entry.get("scale", 1.0))
        make_image_strip(se, f"shot_{index}_{shot}", elements, 2, frame_start, int(width), int(height))
        stagger += 1

    # Text sits above the picture. Keep every lower-third on its own lane.
    for index, text in enumerate(timeline.get("texts", [])):
        start = float(text.get("start", 0.0))
        duration = float(text.get("duration", 1.0))
        make_text_strip(
            se,
            index,
            text,
            1 + int(round(start * fps)),
            max(1, int(round(duration * fps))),
            int(height),
            args.vertical,
        )

    scene.frame_start = 1
    scene.frame_end = 1 + total_frames
    configure_output(scene, int(width), int(height), fps, output, args.preview, preview_pct)

    print(
        f"[TrailerEdit] timeline={timeline_path.name} "
        f"shots={len(strips)} duration={total_seconds:.1f}s output={output} "
        f"res={width}x{height} pct={preview_pct} fps={fps}"
    )
    bpy.ops.render.render(animation=True)
    print(f"[TrailerEdit] wrote {output}")


if __name__ == "__main__":
    main()
