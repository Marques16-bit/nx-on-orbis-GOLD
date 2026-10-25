# Regenerates frontend/menu/menu_font_latin.h: the accented letters and the few symbols the menu
# uses beyond ASCII, from DejaVu Sans Mono, in the same 20x40 cells as menu_font.h.
# Usage: python3 tools/make-menu-font-latin.py <DejaVuSansMono.ttf>
import sys
from PIL import Image, ImageDraw, ImageFont

W, H, SIZE = 20, 40, 32
CHARS = ("áàâãäéèêëíìîïóòôõöúùûüçñ" "ÁÀÂÃÄÉÈÊËÍÌÎÏÓÒÔÕÖÚÙÛÜÇÑ" "¿¡°·•…←→↑↓◀▶▲▼✓✗●○")
font = ImageFont.truetype(sys.argv[1], SIZE)
ascent, descent = font.getmetrics()
top = (H - (ascent + descent)) // 2
glyphs = []
for ch in CHARS:
    image = Image.new("L", (W, H), 0)
    ImageDraw.Draw(image).text((0, top), ch, font=font, fill=255)
    data = list(image.getdata())
    if max(data) == 0:
        sys.exit("the font has no glyph for U+%04X %s" % (ord(ch), ch))
    glyphs.append((ord(ch), data))
out = ["// Generated from DejaVu Sans Mono (Bitstream Vera / DejaVu license, see menu/FONT-LICENSE.txt)",
       "// by tools/make-menu-font-latin.py: accented letters and menu symbols, %dx%d cells, 8-bit coverage." % (W, H),
       "#pragma once", "#include <cstdint>", "namespace MenuFont {",
       "inline constexpr int LatinCount = %d;" % len(glyphs),
       "inline constexpr std::uint32_t LatinCodepoints[%d] = {%s};" % (len(glyphs), ",".join(str(c) for c, _ in glyphs)),
       "inline constexpr std::uint8_t LatinGlyphs[%d][%d] = {" % (len(glyphs), W * H)]
for _, data in glyphs:
    out.append("{" + ",".join(str(v) for v in data) + "},")
out += ["};", "} // namespace MenuFont", ""]
open("frontend/menu/menu_font_latin.h", "w", encoding="utf-8", newline="\n").write("\n".join(out))
print("%d glyphs" % len(glyphs))
