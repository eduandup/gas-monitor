"""Draws the home-screen / app icons in web/ (the website's cylinder on a dark tile).

Run:  python tools/make_icons.py    (needs Pillow: pip install pillow)
"""
from pathlib import Path
from PIL import Image, ImageDraw

WEB = Path(__file__).resolve().parent.parent / "web"
N = 1024                       # draw large, then scale down for smooth edges
BG, TRACK, LINE, VALVE, GAS = "#1c1c1a", "#33332f", "#55554e", "#8f8e86", "#4caf7a"
LEVEL = 0.62                   # how full the drawn cylinder is

# same shape as cylinder() in web/index.html (an 80 x 200 box), scaled into the tile
s = 3.9
ox, oy = N / 2 - 40 * s, (N - 200 * s) / 2
P = lambda x, y: (ox + x * s, oy + y * s)


def box(x0, y0, x1, y1):
    return [*P(x0, y0), *P(x1, y1)]


img = Image.new("RGB", (N, N), BG)
d = ImageDraw.Draw(img)

d.rounded_rectangle(box(20, 19, 60, 52), radius=5 * s, outline=LINE, width=round(5 * s))   # carry handle
d.rectangle(box(31, 4, 49, 14), fill=VALVE)                                                # valve
d.rectangle(box(36, 13, 44, 21), fill=VALVE)
d.rounded_rectangle(box(8, 34, 72, 194), radius=24 * s, fill=TRACK)                       # body

mask = Image.new("L", (N, N), 0)                                                           # gas, clipped to the body
ImageDraw.Draw(mask).rounded_rectangle(box(8, 34, 72, 194), radius=24 * s, fill=255)
ImageDraw.Draw(mask).rectangle([0, 0, N, P(0, 194 - 160 * LEVEL)[1]], fill=0)
img.paste(Image.new("RGB", (N, N), GAS), (0, 0), mask)

d.rounded_rectangle(box(8, 34, 72, 194), radius=24 * s, outline=LINE, width=round(2.5 * s))

for name, size in [("apple-touch-icon.png", 180), ("icon-192.png", 192), ("icon-512.png", 512), ("favicon-32.png", 32)]:
    img.resize((size, size), Image.LANCZOS).save(WEB / name, optimize=True)
    print("wrote", WEB / name)
