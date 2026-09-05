#!/usr/bin/env python3
"""
Hornea las cifras del numero de reduccion como mapas de ALFA de 8 bits.

Por que no la fuente de la libreria: la de Adafruit_GFX es de 5x7 y al agrandarla
solo se puede hacer con bloques, asi que SIEMPRE se ve pixelada. Aqui hace falta
lo contrario -limpia con poca reduccion, tosca con mucha-, y para eso el punto de
partida tiene que tener resolucion de sobra.

Con estos mapas, el aparato dibuja el numero en bloques de k x k pixeles, con k
creciendo con la profundidad: k=1 es la cifra entera y suave, k=8 son ocho pasos
de escalera. Es la misma reduccion que le pasa a la imagen, aplicada al numero
que la nombra.

Uso:  python3 tools/make_glyphs.py > src146/glyphs.h
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
CHARS = "0123456789."
CW, CH = 34, 56          # celda por cifra
SS = 4                   # se dibuja x4 y se reduce: bordes limpios de verdad

def glyph(ch):
    f = ImageFont.truetype(FONT, int(CH * SS * 0.86))
    im = Image.new("L", (CW * SS, CH * SS), 0)
    d = ImageDraw.Draw(im)
    l, t, r, b = d.textbbox((0, 0), ch, font=f)
    d.text(((CW * SS - (r - l)) // 2 - l, (CH * SS - (b - t)) // 2 - t), ch, 255, font=f)
    return np.asarray(im.resize((CW, CH), Image.LANCZOS), dtype=np.uint8)

out = []
out.append("// GENERADO por tools/make_glyphs.py - no editar a mano.")
out.append("// Cifras en alfa de 8 bits, %dx%d, para dibujarlas en bloques de k x k." % (CW, CH))
out.append("#pragma once")
out.append("#include <stdint.h>")
out.append("")
out.append("#define GLYPH_W %d" % CW)
out.append("#define GLYPH_H %d" % CH)
out.append('#define GLYPH_CHARS "%s"' % CHARS)
out.append("#define GLYPH_COUNT %d" % len(CHARS))
out.append("")
out.append("#ifdef GLYPHS_DEFINE")
out.append("const uint8_t GLYPHS[GLYPH_COUNT][GLYPH_W * GLYPH_H] = {")
for ch in CHARS:
    a = glyph(ch).reshape(-1)
    out.append("  { // '%s'" % ch)
    for i in range(0, a.size, 32):
        out.append("    " + ",".join(str(int(v)) for v in a[i:i+32]) + ",")
    out.append("  },")
out.append("};")
out.append("#else")
out.append("extern const uint8_t GLYPHS[GLYPH_COUNT][GLYPH_W * GLYPH_H];")
out.append("#endif")
print("\n".join(out))
