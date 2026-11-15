# Merges the two mods' scheduler_04.bf: the male Cutscenes Restored file,
# with its route-specific changes run only when BIT 4104 (male) is set, so the
# female route plays exactly what the Kotone Cutscenes Project file does.
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())
assert d[8:12] == b'FLW0'
nsec = struct.unpack_from('<I', d, 16)[0]
secs = {}
for k in range(nsec):
    t, es, ec, off = struct.unpack_from('<IIII', d, 32 + k * 16)
    secs[t] = [es, ec, off]
def entries(t):
    es, ec, off = secs[t]
    return [bytes(d[off + i * es: off + (i + 1) * es]) for i in range(ec)]
procs, labels = entries(0), entries(1)
es, ec, off = secs[2]
words = [struct.unpack_from('<I', d, off + i * 4)[0] for i in range(ec)]
tail = bytes(d[secs[3][2]:])  # messages (empty) + strings

def W(op, arg=0): return op | ((arg & 0xFFFF) << 16)
def dec(w): return w & 0xFFFF, w >> 16
PUSHIS, COMM, PUSHREG, IF, GOTO = 29, 8, 4, 28, 13
def proc_off(name):
    for e in procs:
        if e[:24].split(b'\0')[0].decode() == name: return struct.unpack_from('<I', e, 24)[0]
    raise KeyError(name)
def find(start, pattern):
    for i in range(start, start + 400):
        if [dec(words[i + j]) for j in range(len(pattern))] == pattern: return i
    raise ValueError(pattern)

L0 = len(labels)
inserts = []   # (old index, [words], {label: offset within inserted block or 'after'})
h6 = proc_off('sdl04_06_PM_D_hook')
for n_ev, ev in enumerate(([(PUSHIS, 2), (PUSHIS, 100), (COMM, 16384)], [(PUSHIS, 1), (PUSHIS, 101), (COMM, 16384)])):
    i = find(h6, ev)
    lab = L0 + n_ev
    inserts.append((i, [W(PUSHIS, 4104), W(COMM, 7), W(PUSHREG), W(IF, lab)], None))
    inserts.append((i + 3, [], lab))               # label right after the event call
h8 = proc_off('sdl04_08_PM_D_hook')
i = find(h8, [(COMM, 16407)])
lf, le = L0 + 2, L0 + 3
inserts.append((i, [W(PUSHIS, 4104), W(COMM, 7), W(PUSHREG), W(IF, lf), W(GOTO, le)], None))
inserts.append((i, [W(COMM, 16406)], lf))          # female: the vanilla call the male mod removed
inserts.append((i, [], le))

# Build the new code and the old->new index map.
new, labelpos = [], {}
inserts.sort(key=lambda x: x[0])
k = 0
oldmap = {}
for idx in range(len(words) + 1):
    while k < len(inserts) and inserts[k][0] == idx:
        at, block, lab = inserts[k]
        if lab is not None: labelpos[lab] = len(new)
        new.extend(block); k += 1
    oldmap[idx] = len(new)
    if idx < len(words): new.append(words[idx])
assert sorted(labelpos) == [L0, L0 + 1, L0 + 2, L0 + 3], labelpos

def rebase(e):
    e = bytearray(e); o = struct.unpack_from('<I', e, 24)[0]
    struct.pack_into('<I', e, 24, oldmap[o]); return bytes(e)
procs = [rebase(e) for e in procs]
labels = [rebase(e) for e in labels]
for j, lab in enumerate(sorted(labelpos)):
    e = bytearray(32); e[:len(b'MergeRoute_%d' % j)] = b'MergeRoute_%d' % j
    struct.pack_into('<I', e, 24, labelpos[lab]); labels.append(bytes(e))

out = bytearray(d[:secs[0][2]])
p0 = len(out); out += b''.join(procs)
l0 = len(out); out += b''.join(labels)
c0 = len(out); out += b''.join(struct.pack('<I', w) for w in new)
m0 = len(out); out += tail
for k2 in range(nsec):
    t, es2, ec2, off2 = struct.unpack_from('<IIII', out, 32 + k2 * 16)
    if t == 0: v = (es2, len(procs), p0)
    elif t == 1: v = (es2, len(labels), l0)
    elif t == 2: v = (es2, len(new), c0)
    else: v = (es2, ec2, off2 - secs[3][2] + m0)
    struct.pack_into('<IIII', out, 32 + k2 * 16, t, *v)
struct.pack_into('<I', out, 4, struct.unpack_from('<I', d, 4)[0] - secs[3][2] + m0)
open(dst, 'wb').write(out)
print('words', len(words), '->', len(new), 'labels', L0, '->', len(labels), 'size', len(d), '->', len(out))
