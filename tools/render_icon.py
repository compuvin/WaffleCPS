"""Render the WaffleCPS icon. Requires Pillow; run from the repository root."""

from pathlib import Path

from PIL import Image, ImageDraw


SCALE = 4
SIZE = 512


def box(x1, y1, x2, y2):
    return tuple(round(value * SCALE) for value in (x1, y1, x2, y2))


def point(x, y):
    return round(x * SCALE), round(y * SCALE)


icon = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))

# A tilted waffle with a thick edge and an inset grid.
waffle = Image.new("RGBA", (340 * SCALE, 340 * SCALE), (0, 0, 0, 0))
draw = ImageDraw.Draw(waffle)
draw.rounded_rectangle(box(13, 13, 327, 327), radius=55 * SCALE, fill="#925219")
draw.rounded_rectangle(box(23, 23, 317, 317), radius=47 * SCALE, fill="#E7A541")
draw.rounded_rectangle(box(37, 37, 303, 303), radius=38 * SCALE, fill="#F5C46A")

for offset in (94, 145, 196, 247):
    draw.line((point(offset, 39), point(offset, 301)), fill="#C27A29", width=12 * SCALE)
    draw.line((point(39, offset), point(301, offset)), fill="#C27A29", width=12 * SCALE)
draw.rounded_rectangle(box(142, 142, 202, 202), radius=11 * SCALE,
                       fill="#FFF1B2", outline="#E7C96F", width=3 * SCALE)

waffle = waffle.rotate(11, resample=Image.Resampling.BICUBIC, expand=True)
icon.alpha_composite(waffle, point(28, 120))

# The handheld radio overlaps the waffle so both shapes remain clear at small sizes.
draw = ImageDraw.Draw(icon)
draw.rounded_rectangle(box(361, 44, 391, 168), radius=12 * SCALE, fill="#203C3D")
draw.rounded_rectangle(box(314, 123, 352, 170), radius=9 * SCALE, fill="#203C3D")
draw.rounded_rectangle(box(292, 145, 467, 448), radius=29 * SCALE,
                       fill="#F8F7ED")
draw.rounded_rectangle(box(300, 153, 459, 440), radius=24 * SCALE,
                       fill="#273C3F")
draw.rounded_rectangle(box(316, 184, 443, 274), radius=13 * SCALE,
                       fill="#152F33")
draw.rounded_rectangle(box(326, 194, 433, 264), radius=8 * SCALE,
                       fill="#8DD8C5")
draw.line((point(340, 217), point(410, 217)), fill="#397C75", width=8 * SCALE)
draw.line((point(340, 239), point(389, 239)), fill="#397C75", width=6 * SCALE)

for y in (311, 328, 345, 362):
    draw.rounded_rectangle(box(324, y, 435, y + 6), radius=3 * SCALE, fill="#60777A")

for x in (339, 416):
    cx, cy = point(x, 404)
    radius = 14 * SCALE
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill="#D9A44B")

output = Path(__file__).resolve().parents[1] / "assets" / "waffle-radio.png"
output.parent.mkdir(parents=True, exist_ok=True)
icon.resize((SIZE, SIZE), Image.Resampling.LANCZOS).save(output)
print(output)
