"""Render the project's original phone/record symbol into Windows icon assets.

Pillow is a development dependency only. The executable embeds the resulting
ICO and requires no external image files at runtime.
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "cpp/assets"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
BLUE = "#2B69E6"
WHITE = "#FFFFFF"
RED = "#F04452"
# Original handset silhouette, authored in a 256-unit square.
HANDSET = (
    ("M", 77, 50),
    ("C", 70, 46, 62, 48, 57, 55),
    ("L", 49, 67),
    ("C", 40, 80, 43, 99, 51, 117),
    ("C", 68, 154, 101, 186, 138, 203),
    ("C", 155, 211, 174, 213, 187, 205),
    ("L", 200, 196),
    ("C", 208, 191, 209, 183, 203, 176),
    ("L", 182, 151),
    ("C", 177, 145, 170, 144, 164, 149),
    ("L", 152, 158),
    ("C", 148, 161, 143, 160, 139, 157),
    ("C", 122, 147, 108, 133, 98, 117),
    ("C", 95, 112, 95, 107, 98, 103),
    ("L", 107, 91),
    ("C", 112, 85, 111, 78, 105, 73),
    ("L", 83, 54),
    ("C", 81, 52, 79, 51, 77, 50),
    ("Z",),
)


def handset_points() -> list[tuple[float, float]]:
    """Sample the same cubic paths used by the editable SVG for raster output."""
    points = []
    current = (0.0, 0.0)
    for kind, *values in HANDSET:
        if kind in ("M", "L"):
            current = tuple(values)
            points.append(current)
        elif kind == "C":
            start = current
            first, second, end = values[:2], values[2:4], values[4:]
            for step in range(1, 49):
                t = step / 48
                u = 1 - t
                points.append(tuple(u**3 * start[axis] + 3 * u**2 * t * first[axis]
                                    + 3 * u * t**2 * second[axis] + t**3 * end[axis]
                                    for axis in range(2)))
            current = tuple(end)
    return points


def render(size: int) -> Image.Image:
    """Render each native size independently with transparent, antialiased edges."""
    scale = size * 6 / 256
    image = Image.new("RGBA", (size * 6, size * 6), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle(tuple(value * scale for value in (8, 8, 248, 248)),
                           radius=53 * scale, fill=BLUE)
    draw.polygon([(x * scale, y * scale) for x, y in handset_points()], fill=WHITE)
    draw.ellipse(tuple(value * scale for value in (157, 38, 213, 94)), fill=WHITE)
    draw.ellipse(tuple(value * scale for value in (163, 44, 207, 88)), fill=RED)
    image = image.resize((size, size), Image.Resampling.LANCZOS)
    # Remove almost invisible resampling specks outside the transparent tile.
    image.putalpha(image.getchannel("A").point([0 if alpha < 3 else alpha for alpha in range(256)]))
    return image


def main() -> int:
    """Write an editable vector master, preview and all embedded Windows icon sizes."""
    OUTPUT.mkdir(parents=True, exist_ok=True)
    path = " ".join(kind + (" " + " ".join(map(str, values)) if values else "")
                    for kind, *values in HANDSET)
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <title>PhoneRecord — phone and recording</title>
  <rect x="8" y="8" width="240" height="240" rx="53" fill="{BLUE}"/>
  <path d="{path}" fill="{WHITE}"/>
  <circle cx="185" cy="66" r="28" fill="{WHITE}"/>
  <circle cx="185" cy="66" r="22" fill="{RED}"/>
</svg>
'''
    (OUTPUT / "phonerecord.svg").write_text(svg, encoding="utf-8")
    frames = [render(size) for size in SIZES]
    frames[-1].save(OUTPUT / "phonerecord.png")
    frames[-1].save(OUTPUT / "phonerecord.ico", format="ICO",
                    sizes=[(size, size) for size in SIZES], append_images=frames[:-1])
    print("PhoneRecord original SVG, PNG and ICO generated: " + ", ".join(map(str, SIZES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
