import math
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
REG = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
TEAL, INDIGO = "#14B8A6", "#6366F1"

def text_path(text, font_file, size, x, y, letter=0.0):
    """Text as one SVG path (baseline at y), so the logo needs no installed font."""
    font = TTFont(font_file)
    gs, cmap, hmtx = font.getGlyphSet(), font.getBestCmap(), font["hmtx"]
    scale = size / font["head"].unitsPerEm
    pen = SVGPathPen(gs)
    cx = x
    for ch in text:
        g = cmap[ord(ch)]
        gs[g].draw(TransformPen(pen, (scale, 0, 0, -scale, cx, y)))
        cx += hmtx[g][0] * scale + letter
    return pen.getCommands(), cx - x

def mark(cx, cy, r, node_fill):
    """An open C of six linked nodes around a central node."""
    angles = [math.radians(a) for a in (40, 96, 152, 208, 264, 320)]
    pts = [(cx + r * math.cos(a), cy - r * math.sin(a)) for a in angles]
    out = []
    # the arc of the C
    a0, a1 = angles[0], angles[-1]
    x0, y0 = cx + r * math.cos(a0), cy - r * math.sin(a0)
    x1, y1 = cx + r * math.cos(a1), cy - r * math.sin(a1)
    out.append(f'<path d="M{x0:.1f} {y0:.1f} A{r} {r} 0 1 0 {x1:.1f} {y1:.1f}" fill="none" '
               f'stroke="url(#cc-grad)" stroke-width="{r*0.12:.1f}" stroke-linecap="round"/>')
    # spokes from the centre to every node
    for x, y in pts:
        out.append(f'<line x1="{cx}" y1="{cy}" x2="{x:.1f}" y2="{y:.1f}" stroke="url(#cc-grad)" '
                   f'stroke-width="{r*0.035:.1f}" stroke-linecap="round" opacity="0.55"/>')
    for x, y in pts:
        out.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r*0.15:.1f}" fill="{node_fill}" '
                   f'stroke="url(#cc-grad)" stroke-width="{r*0.07:.1f}"/>')
    out.append(f'<circle cx="{cx}" cy="{cy}" r="{r*0.22:.1f}" fill="url(#cc-grad)"/>')
    return "\n  ".join(out)

def defs(x1, y1, x2, y2):
    return (f'<defs><linearGradient id="cc-grad" gradientUnits="userSpaceOnUse" x1="{x1}" y1="{y1}" '
            f'x2="{x2}" y2="{y2}"><stop offset="0" stop-color="{TEAL}"/>'
            f'<stop offset="1" stop-color="{INDIGO}"/></linearGradient></defs>')

def icon(node_fill="#FFFFFF"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" role="img" aria-label="Credits Community">\n'
            f'  {defs(96, 96, 416, 416)}\n  {mark(256, 256, 176, node_fill)}\n</svg>\n')

def horizontal(ink, node_fill):
    h = 300
    m = mark(150, 150, 104, node_fill)
    p1, w1 = text_path("Credits", BOLD, 112, 300, 148, letter=-2)
    p2, w2 = text_path("Community", REG, 70, 304, 236, letter=1)
    width = int(300 + max(w1, w2) + 40)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {h}" role="img" aria-label="Credits Community">\n'
            f'  {defs(46, 46, 254, 254)}\n  {m}\n'
            f'  <path d="{p1}" fill="{ink}"/>\n  <path d="{p2}" fill="{TEAL}"/>\n</svg>\n')

open("credits-community-icon.svg", "w").write(icon())
open("credits-community-logo.svg", "w").write(horizontal("#0F172A", "#FFFFFF"))
open("credits-community-logo-dark.svg", "w").write(horizontal("#F8FAFC", "#0F172A"))
print("ok")
