# Renders tests/dump output with PIL (approximation of the Windows renderer) for visual checks.
import sys, math
from PIL import Image, ImageDraw, ImageFont
src, dst = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]) if len(sys.argv) > 3 else 900
lines = open(src, encoding="utf-8").read().splitlines()
ex = list(map(float, lines[0].split()[1:]))
H = W
img = Image.new("RGB", (W, H), (33, 40, 48)); d = ImageDraw.Draw(img)
dx, dy = ex[2]-ex[0], ex[3]-ex[1]
s = min((W*0.94)/max(dx,1e-9), (H*0.94)/max(dy,1e-9))
cx, cy = (ex[0]+ex[2])/2, (ex[1]+ex[3])/2
P = lambda x, y: ((x-cx)*s + W/2, (cy-y)*s + H/2)
def col(c, a=255):
    c = int(c)
    if c == 0xFF000000: return (255,255,255)
    return ((c>>16)&255, (c>>8)&255, c&255)
font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
for ln in lines[1:]:
    t = ln.split("\t")
    f = t[0].split()
    if f[0] == "P":
        pts = [P(float(f[i]), float(f[i+1])) for i in range(3, len(f), 2)]
        if int(f[2]): pts.append(pts[0])
        d.line(pts, fill=col(f[1]), width=1)
    elif f[0] == "F":
        rings = ln.split("|")[1:]
        c = col(f[1])
        if int(f[2]) < 255: c = tuple(int(v*0.25 + b*0.75) for v, b in zip(c, (33,40,48)))
        mask = Image.new("1", (W, H), 0); md = ImageDraw.Draw(mask)
        for r in rings:
            v = list(map(float, r.split()))
            pts = [P(v[i], v[i+1]) for i in range(0, len(v), 2)]
            tmp = Image.new("1", (W, H), 0); ImageDraw.Draw(tmp).polygon(pts, fill=1)
            from PIL import ImageChops
            mask = ImageChops.logical_xor(mask, tmp)
        img.paste(c, mask=mask); d = ImageDraw.Draw(img)
    elif f[0] == "T":
        colr, x, y, h, rot, wf, fit, baseY, step, ha, fx, fy = f[1:13]
        h = float(h)*s
        if h < 2: continue
        em = h/0.716
        fnt = ImageFont.truetype(font_path, max(1, int(em)))
        for i, text in enumerate(t[1:]):
            tw = fnt.getlength(text)
            ox = {0:0, 1:-tw/2, 2:-tw}[int(ha)]
            by = -float(baseY)*s + i*float(step)*s   # screen-down baseline offset in px
            im = Image.new("RGBA", (int(tw)+4, int(em*1.6)+4), (0,0,0,0))
            ImageDraw.Draw(im).text((0, 0), text, font=fnt, fill=col(colr)+(255,), anchor="ls" if False else None)
            asc = fnt.getmetrics()[0]
            r = float(rot)
            # place: rotate around anchor; approximate by positioning text box then rotating
            big = Image.new("RGBA", (W, H), (0,0,0,0))
            ax, ay = P(float(x), float(y))
            big.paste(im, (int(ax+ox), int(ay+by-asc)), im)
            big = big.rotate(math.degrees(r), center=(ax, ay))
            img.paste(big, (0,0), big)
img.save(dst)
