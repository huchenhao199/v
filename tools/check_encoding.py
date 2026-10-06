"""check_encoding.py -- 看看仓库里几个文件的编码和换行符（发布前自查用）。

用法: python check_encoding.py <文件...>
"""
import sys

BOM = b'\xef\xbb\xbf'

for p in sys.argv[1:]:
    b = open(p, 'rb').read()
    crlf = b.count(b'\r\n')
    lf = b.count(b'\n') - crlf
    print(f"{p:36s} size={len(b):7d}  BOM={b[:3] == BOM}  CRLF={crlf}  bareLF={lf}")
