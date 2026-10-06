"""Parse a minidump and report the faulting instruction address + RIP."""
import struct
import sys

path = sys.argv[1]
data = open(path, 'rb').read()
sig, ver, nstreams, rva_dir, checksum, ts, flags = struct.unpack_from('<4sIIIIIQ', data, 0)
assert sig == b'MDMP', sig
print(f"streams={nstreams} dir_rva={rva_dir}")

streams = {}
for i in range(nstreams):
    stype, dsize, drva = struct.unpack_from('<III', data, rva_dir + i * 12)
    streams.setdefault(stype, []).append((dsize, drva))

MODULE_LIST = 4
EXCEPTION = 6
SYSTEM_INFO = 7
THREAD_LIST = 3
print("stream types:", sorted(streams))

base = None
if MODULE_LIST in streams:
    dsize, drva = streams[MODULE_LIST][0]
    nmod = struct.unpack_from('<I', data, drva)[0]
    print(f"modules={nmod}")
    for m in range(nmod):
        off = drva + 4 + m * 108
        mbase, msize, cksum, mt, namerva = struct.unpack_from('<QIIII', data, off)
        (nlen,) = struct.unpack_from('<I', data, namerva)
        name = data[namerva + 4: namerva + 4 + nlen].decode('utf-16-le')
        if 'fp.exe' in name or 'map.exe' in name:
            base = mbase
            print(f"  {name} base=0x{mbase:x} size=0x{msize:x}")

if EXCEPTION in streams:
    dsize, drva = streams[EXCEPTION][0]
    tid, align = struct.unpack_from('<II', data, drva)
    er = drva + 8
    code, eflags, erec, eaddr = struct.unpack_from('<IIQQ', data, er)
    nparam = struct.unpack_from('<I', data, er + 24)[0]
    params = struct.unpack_from('<' + 'Q' * nparam, data, er + 32)
    print(f"exception tid={tid} code=0x{code:x} addr=0x{eaddr:x} nparam={nparam}")
    print("  params:", [hex(p) for p in params])
    if base:
        print(f"  fault RVA = 0x{eaddr - base:x}")
    # find this thread in the thread list to get its CONTEXT
    ctx = None
    if THREAD_LIST in streams:
        dsize2, drva2 = streams[THREAD_LIST][0]
        nthr = struct.unpack_from('<I', data, drva2)[0]
        for t in range(nthr):
            off = drva2 + 4 + t * 48
            ttid, suspend, prio_class, prio, teb, stack_start, stack_size, stack_rva, ctx_size2, ctx_rva = \
                struct.unpack_from('<IIIIQQIIII', data, off)
            if ttid == tid:
                ctx = ctx_rva
                print(f"  thread ctx_rva=0x{ctx_rva:x} size={ctx_size2} stack_rva=0x{stack_rva:x} stack_size=0x{stack_size:x}")
    if ctx is None:
        ctx = er + 152
    MEMORY64 = 9

    def reg(name):
        off = {'RIP': 0xF8, 'RSP': 0x98, 'RBP': 0xA0, 'RAX': 0x78, 'RCX': 0x80,
               'RDX': 0x88, 'RBX': 0x90, 'RSI': 0xA8, 'RDI': 0xB0, 'R8': 0xB8,
               'R9': 0xC0, 'R10': 0xC8, 'R11': 0xD0, 'R12': 0xD8, 'R13': 0xE0,
               'R14': 0xE8, 'R15': 0xF0}[name]
        return struct.unpack_from('<Q', data, ctx + off)[0]
    rip, rsp = reg('RIP'), reg('RSP')
    print(f"  RIP=0x{rip:x} RSP=0x{rsp:x} RBP=0x{reg('RBP'):x}")
    for rn in ('RAX', 'RBX', 'RCX', 'RDX', 'RSI', 'RDI', 'R8', 'R9', 'R12', 'R13', 'R14', 'R15'):
        print(f"    {rn}=0x{reg(rn):x}")
    if base:
        print(f"  RIP RVA = 0x{rip - base:x}")

    # dump the top of the stack, marking code addresses
    def mem(addr, n):
        if MEMORY64 in streams:
            dsize2, drva2 = streams[MEMORY64][0]
            nrec = struct.unpack_from('<I', data, drva2)[0]
            for r in range(nrec):
                off = drva2 + 4 + r * 16
                saddr, ssize, srva = struct.unpack_from('<QII', data, off)
                if saddr <= addr and addr + n < saddr + ssize:
                    return data[srva + (addr - saddr): srva + (addr - saddr) + n]
        return None

    MEMORY64 = 9
    if MEMORY64 in streams and base:
        raw = mem(rsp, 8 * 24)
        if raw:
            print("  stack:")
            for i in range(0, len(raw), 8):
                v = struct.unpack_from('<Q', raw, i)[0]
                tag = ""
                if base <= v < base + 0x400000:
                    tag = f"  <- code/data RVA 0x{v - base:x}"
                print(f"    [rsp+{i:3d}] 0x{v:016x}{tag}")
