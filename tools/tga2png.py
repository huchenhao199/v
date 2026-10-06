"""tga2png.py -- 把 fp.exe --shot 写出来的 32 位无压缩 TGA 转成 PNG（看图用）。

用法: python tga2png.py in.tga out.png [scale]
"""
import struct
import sys

from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

raw = open(src, 'rb').read()
idlen, cmaptype, imgtype = raw[0], raw[1], raw[2]
w, h = struct.unpack_from('<HH', raw, 12)
bpp, desc = raw[16], raw[17]
assert imgtype == 2 and bpp == 32, (imgtype, bpp)

off = 18 + idlen
top_down = bool(desc & 0x20)
# fp.exe 的后备缓冲是 DXGI_FORMAT_R8G8B8A8_UNORM，逐字节抄进 TGA，
# 所以文件里的三个字节就是 R,G,B（TGA 名义上该是 B,G,R，这里按实际的来）
img = Image.frombytes('RGBA', (w, h), raw[off:off + w * h * 4], 'raw', 'RGBA')
if not top_down:
    img = img.transpose(Image.FLIP_TOP_BOTTOM)
img = img.convert('RGB')
if scale != 1.0:
    img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
img.save(dst)
print(f"{dst}  {w}x{h} -> {img.size[0]}x{img.size[1]}")
