"""compare.py -- 把改之前 / 改之后的截图拼成一张对比图（写报告用）。

用法: python compare.py out.png "标题1:文件1" "标题2:文件2" ...
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = r"C:\Windows\Fonts\msyh.ttc"
out = sys.argv[1]
items = [a.split(":", 1) for a in sys.argv[2:]]

imgs = [Image.open(p).convert("RGB") for _, p in items]
w, h = imgs[0].size
cols, rows = len(imgs), 1
bar = 40
canvas = Image.new("RGB", (w * cols, (h + bar) * rows), (24, 26, 30))
d = ImageDraw.Draw(canvas)
font = ImageFont.truetype(FONT, 24)

for i, (title, _) in enumerate(items):
    x = i * w
    canvas.paste(imgs[i], (x, bar))
    tw = d.textlength(title, font=font)
    d.text((x + (w - tw) / 2, 7), title, font=font, fill=(240, 240, 240))
    if i:
        d.line([(x, 0), (x, h + bar)], fill=(90, 94, 100), width=2)

canvas.save(out)
print(f"{out}  {canvas.size[0]}x{canvas.size[1]}")
