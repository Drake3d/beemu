#!/usr/bin/env python3
# Read-only BeFS (BFS) reader for raw BeOS disk images: list a directory or copy a file out.
#   tools/bfs_read.py IMAGE PARTITION_BYTE_OFFSET PATH [OUTFILE]
# The partition offset is the BFS partition start in bytes (MBR LBA * 512; the test R5 image: 8225280,
# Dano: 1032192). Handles direct and indirect block runs (not double indirect). Never writes the image.
import struct, sys

img, base, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
out = sys.argv[4] if len(sys.argv) > 4 else None
f = open(img, 'rb')

def rd(off, n):
    f.seek(base + off); return f.read(n)

sb = rd(512, 512)
block_size, block_shift = struct.unpack_from('<II', sb, 40)
ag_shift = struct.unpack_from('<i', sb, 72 + 8 - 8 + 4)[0]
# superblock layout: name[32] magic1 byte_order block_size block_shift num_blocks used_blocks
#   inode_size magic2 blocks_per_ag ag_shift num_ags flags log_blocks log_start log_end magic3 root indices
o = 32
magic1, byte_order, block_size, block_shift = struct.unpack_from('<iiII', sb, o); o += 16
num_blocks, used_blocks = struct.unpack_from('<qq', sb, o); o += 16
inode_size, magic2, blocks_per_ag, ag_shift, num_ags, flags = struct.unpack_from('<iiiiii', sb, o); o += 24
o += 8 + 16        # log_blocks, log_start, log_end
o += 4             # magic3
root = struct.unpack_from('<iHH', sb, o)

def run_block(ag, start):
    return (ag << ag_shift) + start

def read_inode(run):
    return rd(run_block(run[0], run[1]) * block_size, block_size)

def runs_of(ino):
    ds = 72
    runs = [struct.unpack_from('<iHH', ino, ds + 8 * i) for i in range(12)]
    runs = [r for r in runs if r[2]]
    ind = struct.unpack_from('<iHH', ino, ds + 104)
    if ind[2]:
        data = rd(run_block(ind[0], ind[1]) * block_size, ind[2] * block_size)
        for i in range(0, len(data), 8):
            r = struct.unpack_from('<iHH', data, i)
            if r[2]: runs.append(r)
    dbl = struct.unpack_from('<iHH', ino, ds + 120)
    if dbl[2]:
        raise SystemExit('double indirect not supported')
    return runs

def read_stream(ino):
    size = struct.unpack_from('<q', ino, 72 + 136)[0]
    buf = b''.join(rd(run_block(a, s) * block_size, n * block_size) for a, s, n in runs_of(ino))
    return buf[:size]

def dir_entries(ino):
    data = read_stream(ino)
    node_size = struct.unpack_from('<I', data, 4)[0]
    ents = {}
    for off in range(node_size, len(data), node_size):
        nd = data[off:off + node_size]
        left, right, overflow = struct.unpack_from('<qqq', nd, 0)
        count, keylen = struct.unpack_from('<HH', nd, 24)
        if overflow != -1 or count == 0 or count > 500:
            continue
        keys = nd[28:28 + keylen]
        lo = (28 + keylen + 7) & ~7
        ends = struct.unpack_from('<%dH' % count, nd, lo)
        vo = lo + 2 * count
        vals = struct.unpack_from('<%dq' % count, nd, vo)
        prev = 0
        for e, v in zip(ends, vals):
            ents[keys[prev:e].decode('utf-8', 'replace')] = v
            prev = e
    return ents

def inode_by_num(num):
    ag = num >> ag_shift
    return read_inode((ag, num & ((1 << ag_shift) - 1), 1))

ino = read_inode(root)
for part in [p for p in path.split('/') if p]:
    ents = dir_entries(ino)
    if part not in ents:
        raise SystemExit('not found: ' + part)
    ino = inode_by_num(ents[part])
mode = struct.unpack_from('<i', ino, 20)[0]
if (mode & 0o170000) == 0o040000:
    for k in sorted(dir_entries(ino)):
        if k not in ('.', '..'): print(k)
else:
    data = read_stream(ino)
    if out: open(out, 'wb').write(data); print(len(data), 'bytes ->', out)
    else: print(len(data), 'bytes')
