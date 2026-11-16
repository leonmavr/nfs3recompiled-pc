"""Every picture of the game as a PNG, to look at; never changes the game's files.

    python3 tools/export_textures.py GAME_DIR OUT_DIR [FOLDER ...]

GAME_DIR is the folder holding fedata/ and gamedata/.  Every .fsh and .qfs
under it (and every one inside a .viv, the cars' archives among them, with
the cars' paint, car00.tga) comes out under OUT_DIR at the same path, a
folder per file and a PNG per picture, by the picture's own four-letter
name.  FOLDER, e.g. gamedata/tracks, limits it to those.

A .qfs is an .fsh compressed with EA's RefPack.  An .fsh ("SHPI") is a
directory of pictures, each a 16-byte header -- its format, the offset of
what is attached to it, width, height -- and its pixels: 0x7d 32-bit BGRA,
0x7e 16-bit 1555, 0x78 16-bit 565, 0x6d 16-bit 4444, 0x7f 24-bit, 0x7b
8-bit with a palette attached (0x24, 0x22, 0x2a or 0x2d).  Only the full-size
level is written; the smaller ones the game makes itself.  What could not be
read is listed in not_converted_*.txt.
"""
import sys, struct, os, io
def unref(d):
    p=0
    flags=d[0]<<8|d[1]; p=2
    if flags&0x8000: size=int.from_bytes(d[p:p+4],'big'); p+=4
    else: size=int.from_bytes(d[p:p+3],'big'); p+=3
    if flags&0x100: p+= 4 if flags&0x8000 else 3
    out=bytearray()
    while p<len(d):
        b0=d[p]
        if b0<0x80:
            b1=d[p+1]; p+=2
            n=b0&3; out+=d[p:p+n]; p+=n
            cnt=((b0>>2)&7)+3; off=((b0&0x60)<<3)+b1+1
        elif b0<0xc0:
            b1,b2=d[p+1],d[p+2]; p+=3
            n=b1>>6; out+=d[p:p+n]; p+=n
            cnt=(b0&0x3f)+4; off=((b1&0x3f)<<8)+b2+1
        elif b0<0xe0:
            b1,b2,b3=d[p+1],d[p+2],d[p+3]; p+=4
            n=b0&3; out+=d[p:p+n]; p+=n
            cnt=((b0&0x0c)<<6)+b3+5; off=((b0&0x10)<<12)+(b1<<8)+b2+1
        elif b0<0xfc:
            n=((b0&0x1f)<<2)+4; p+=1; out+=d[p:p+n]; p+=n; continue
        else:
            n=b0&3; p+=1; out+=d[p:p+n]; p+=n; break
        for _ in range(cnt): out.append(out[-off])
    return bytes(out)
def fsh(data):
    if data[:4]!=b'SHPI': data=unref(data)
    n=struct.unpack_from('<i',data,8)[0]
    recs=[]
    for k in range(n):
        name=data[16+8*k:20+8*k].decode('latin-1'); off=struct.unpack_from('<i',data,20+8*k)[0]
        code=data[off]; w,h=struct.unpack_from('<HH',data,off+4)
        recs.append((name,code,w,h,off))
    return data,recs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_viv import entries
from PIL import Image

def palette_of(data, off, nxt_end):
    # attachments after the picture: walk the chain of "next" offsets
    pos = off
    while True:
        nxt = data[pos+1] | data[pos+2] << 8 | data[pos+3] << 16
        if not nxt: return None
        pos += nxt
        if pos >= nxt_end: return None
        code = data[pos]
        w = struct.unpack_from('<H', data, pos+4)[0]
        p = pos + 16
        if code == 0x24:   # 24-bit RGB
            return [tuple(data[p+i*3:p+i*3+3]) + (255,) for i in range(w)]
        if code == 0x22:   # 24-bit, 6 bits a channel
            return [tuple(min(255, data[p+i*3+k] * 4) for k in range(3)) + (255,) for i in range(w)]
        if code == 0x2a:   # 32-bit BGRA
            return [(data[p+i*4+2], data[p+i*4+1], data[p+i*4], data[p+i*4+3]) for i in range(w)]
        if code == 0x2d:   # 16-bit 1555
            out = []
            for i in range(w):
                v = struct.unpack_from('<H', data, p+i*2)[0]
                out.append(((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0))
            return out

def picture(data, off, end):
    code = data[off] & 0x7f
    w, h = struct.unpack_from('<HH', data, off + 4)
    p = off + 16
    if w == 0 or h == 0 or w > 4096 or h > 4096: return None
    if code == 0x7d:      # 32-bit BGRA
        raw = data[p:p + w*h*4]
        return Image.frombytes('RGBA', (w, h), bytes(raw), 'raw', 'BGRA')
    if code == 0x7f:      # 24-bit BGR
        return Image.frombytes('RGB', (w, h), bytes(data[p:p + w*h*3]), 'raw', 'BGR')
    if code in (0x7e, 0x78, 0x6d):
        vals = struct.unpack_from('<%dH' % (w*h), data, p)
        px = bytearray()
        for v in vals:
            if code == 0x7e:
                px += bytes(((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0))
            elif code == 0x78:
                px += bytes(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
            else:
                px += bytes(((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12) * 17))
        return Image.frombytes('RGBA', (w, h), bytes(px))
    if code == 0x7b:      # 8-bit with a palette
        pal = palette_of(data, off, end) or [(i, i, i, 255) for i in range(256)]
        pal = (pal + [(0, 0, 0, 0)] * 256)[:256]
        idx = data[p:p + w*h]
        px = bytearray()
        for i in idx: px += bytes(pal[i])
        return Image.frombytes('RGBA', (w, h), bytes(px))
    return None

def export(blob, outdir, label, log):
    try:
        data, recs = fsh(blob)
    except Exception as e:
        log.append('%s: not a picture file (%s)' % (label, e)); return 0
    n = 0
    offs = sorted(r[4] for r in recs) + [len(data)]
    for name, code, w, h, off in recs:
        end = offs[offs.index(off) + 1]
        try:
            img = picture(data, off, len(data))
        except Exception as e:
            img = None
        safe = ''.join(c if c.isalnum() or c in '-_' else '_' for c in name) or 'x'
        if img is None:
            log.append('%s/%s: format 0x%02x not converted' % (label, name, code)); continue
        os.makedirs(outdir, exist_ok=True)
        # Two pictures of one name in a file are told apart by a number;
        # a picture written before by another run is written over.
        key = os.path.join(outdir, safe.lower()); k = written.get(key, 0) + 1; written[key] = k
        fn = os.path.join(outdir, safe + ('.png' if k == 1 else '_%d.png' % k))
        img.save(fn); n += 1
    return n

if __name__ == "__main__":   # importing this file reads pictures, it does not export them
    written = {}
    root, out = sys.argv[1], sys.argv[2]
    only = sys.argv[3:]  # top folders under root to do, all when none
    log = []; total = 0
    for dp, dn, fns in os.walk(root):
        if only and not any(os.path.relpath(dp, root).replace(os.sep, '/').startswith(o) for o in only): continue
        for fn in sorted(fns):
            path = os.path.join(dp, fn); rel = os.path.relpath(path, root)
            low = fn.lower()
            if low.endswith(('.qfs', '.fsh')):
                total += export(open(path, 'rb').read(), os.path.join(out, rel), rel, log)
            elif low.endswith('.viv'):
                data = open(path, 'rb').read()
                try: es = list(entries(data))
                except Exception: continue
                for n, o, s in es:
                    if n.lower().endswith(('.fsh', '.qfs')):
                        total += export(data[o:o+s], os.path.join(out, rel, n), rel + '/' + n, log)
                    elif n.lower().endswith('.tga'):
                        try:
                            img = Image.open(io.BytesIO(bytes(data[o:o+s])))
                            os.makedirs(os.path.join(out, rel), exist_ok=True)
                            img.save(os.path.join(out, rel, n[:-4] + '.png')); total += 1
                        except Exception as e:
                            log.append('%s/%s: %s' % (rel, n, e))
    print(total, 'pictures'); open(os.path.join(out, 'not_converted_%s.txt' % ('_'.join(o.replace('/', '-') for o in only) or 'all')), 'w').write('\n'.join(log) + '\n'); print(len(log), 'skipped')
