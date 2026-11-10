# S1: makes amiga/s1_font.h from DejaVuSans-Bold.ttf:  python3 gen_s1_font.py 44  (needs Pillow)
import sys
from PIL import Image, ImageDraw, ImageFont
SIZE=int(sys.argv[1]) if len(sys.argv)>1 else 44
f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',SIZE)
asc,desc=f.getmetrics()
codes=list(range(32,127))+list(range(160,256))
glyphs=[];data=bytearray()
for c in codes:
    ch=bytes([c]).decode('latin-1')
    adv=int(round(f.getlength(ch)))
    bb=f.getbbox(ch)
    w=max(0,bb[2]-bb[0]); h=max(0,bb[3]-bb[1])
    off=len(data)
    if w and h:
        im=Image.new('L',(w,h),0)
        ImageDraw.Draw(im).text((-bb[0],-bb[1]),ch,font=f,fill=255)
        px=im.tobytes()
        # 4-bit packed, row by row, rows padded to byte
        for y in range(h):
            row=px[y*w:(y+1)*w]
            for x in range(0,w,2):
                a=(row[x]+8)//17; b=(row[x+1]+8)//17 if x+1<w else 0
                data.append((a<<4)|b)
    glyphs.append((c,w,h,bb[0],bb[1],adv,off))
out=open('s1_font.h','w')
out.write('/* S1 subtitle font: DejaVu Sans Bold %dpx, Latin-1, 4-bit alpha.\n'%SIZE)
out.write(' * Generated from DejaVuSans-Bold.ttf (DejaVu fonts: Bitstream Vera\n * license - free to use, embed and redistribute). */\n')
out.write('#define S1F_ASCENT %d\n#define S1F_DESCENT %d\n#define S1F_SIZE %d\n'%(asc,desc,SIZE))
out.write('typedef struct { unsigned char w, h; signed char xo, yo; unsigned char adv; unsigned long off; } s1f_glyph;\n')
out.write('static const s1f_glyph s1f_glyphs[256] = {\n')
gm={g[0]:g for g in glyphs}
for c in range(256):
    g=gm.get(c)
    if g: out.write(' {%d,%d,%d,%d,%d,%d}, /* %d */\n'%(g[1],g[2],g[3],g[4],g[5],g[6],c))
    else: out.write(' {0,0,0,0,0,0},\n')
out.write('};\nstatic const unsigned char s1f_data[%d] = {\n'%len(data))
for i in range(0,len(data),24):
    out.write(' '+','.join(str(b) for b in data[i:i+24])+',\n')
out.write('};\n')
print('asc',asc,'desc',desc,'bytes',len(data), 'maxyo', max(g[4]+g[2] for g in glyphs), 'minyo',min(g[4] for g in glyphs if g[2]))
