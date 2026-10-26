# Regenerates assets/icon0.png (512x512) and assets/pic1.png (1920x1080): the package's icon and the
# background of its page on the PS4 home screen. Original artwork: a generic hybrid handheld (a tablet
# with a grip on each side; own colours, no logos) showing a planet with its orbit, and the name.
# Usage: python3 tools/make-icons.py [path to DejaVuSans-Bold.ttf]
import math
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
NAVY, INDIGO = (9, 14, 32), (30, 54, 132)
BLUE, TEAL, CORAL, WHITE = (74, 134, 255), (45, 212, 196), (255, 107, 107), (234, 240, 255)
BODY, EDGE = (22, 28, 46), (58, 70, 104)
SS = 4  # supersampling


def gradient(size, top, bottom):
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = min(1.0, (x / w) * 0.45 + (y / h) * 0.55)
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return img


def shade(color, k):
    return tuple(max(0, min(255, int(c * k))) for c in color)


def grip(d, box, radius, color):
    """A side grip: a rounded body with a lighter top edge."""
    x0, y0, x1, y1 = box
    d.rounded_rectangle(box, radius, fill=shade(color, 0.78))
    d.rounded_rectangle((x0 + 3 * SS, y0 + 3 * SS, x1 - 3 * SS, y1 - 3 * SS), radius - 3 * SS, fill=color)
    d.rounded_rectangle((x0 + 3 * SS, y0 + 3 * SS, x1 - 3 * SS, y0 + (y1 - y0) * 0.35), radius - 3 * SS, fill=shade(color, 1.12))


def screen_scene(size):
    """What is on the console's screen: the planet, its orbit and a satellite."""
    w, h = size
    img = gradient(size, (10, 16, 40), (34, 62, 150)).convert("RGBA")
    cx, cy, r = w * 0.5, h * 0.52, h * 0.27
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([cx - r * 2, cy - r * 2, cx + r * 2, cy + r * 2], fill=BLUE + (80,))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(r * 0.6)))

    def orbit(front):
        layer = Image.new("RGBA", size, (0, 0, 0, 0))
        rx, ry, wd = r * 1.85, r * 0.62, max(2, int(r * 0.11))
        ImageDraw.Draw(layer).ellipse([cx - rx, cy - ry, cx + rx, cy + ry], outline=TEAL + (255,), width=wd)
        mask = Image.new("L", size, 0)
        ImageDraw.Draw(mask).rectangle([0, cy if front else 0, w, h if front else cy], fill=255)
        layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))
        return layer.rotate(24, resample=Image.BICUBIC, center=(cx, cy))

    img.alpha_composite(orbit(False))
    sphere = Image.new("RGBA", size, (0, 0, 0, 0))
    sp = sphere.load()
    for y in range(int(cy - r) - 1, int(cy + r) + 2):
        for x in range(int(cx - r) - 1, int(cx + r) + 2):
            dx, dy = (x - cx) / r, (y - cy) / r
            if dx * dx + dy * dy <= 1.0:
                light = max(0.0, 1.0 - math.hypot(dx + 0.35, dy + 0.4) / 1.6)
                c = [int(BLUE[i] * (0.45 + 0.75 * light) + 255 * 0.12 * light ** 3) for i in range(3)]
                sp[x, y] = (min(c[0], 255), min(c[1], 255), min(c[2], 255), 255)
    img.alpha_composite(sphere)
    font = ImageFont.truetype(FONT, int(r * 0.95))
    d = ImageDraw.Draw(img)
    tw = d.textlength("NX", font=font)
    d.text((cx - tw / 2, cy - r * 0.64), "NX", font=font, fill=WHITE + (255,))
    img.alpha_composite(orbit(True))
    a = math.radians(-58)
    rx, ry = r * 1.85, r * 0.62
    x, y = rx * math.cos(a), ry * math.sin(a)
    t = math.radians(-24)
    sx, sy = cx + x * math.cos(t) - y * math.sin(t), cy + x * math.sin(t) + y * math.cos(t)
    sr = r * 0.14
    d.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=WHITE + (255,))
    return img


def console(img, x, y, scale, title=True):
    """Draws the handheld with its top-left corner at (x, y) (in output pixels); returns its bottom."""
    s = scale * SS
    d = ImageDraw.Draw(img)
    W, H = 470, 210
    X0, Y0 = x * SS, y * SS

    def box(a, b, c, e):
        return (X0 + a * s, Y0 + b * s, X0 + c * s, Y0 + e * s)

    # soft shadow
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(box(14, 22, W - 14, H + 18), 50 * s, fill=(0, 0, 0, 150))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14 * s)))
    grip(d, box(0, 0, 118, H), int(40 * s), TEAL)
    grip(d, box(W - 118, 0, W, H), int(40 * s), CORAL)
    d.rounded_rectangle(box(104, 0, W - 104, H), int(22 * s), fill=EDGE)
    d.rounded_rectangle(box(108, 4, W - 108, H - 4), int(20 * s), fill=BODY)
    # the screen
    sx0, sy0, sx1, sy1 = 128, 20, W - 128, H - 20
    scr_w, scr_h = int((sx1 - sx0) * s), int((sy1 - sy0) * s)
    scene = screen_scene((scr_w, scr_h))
    mask = Image.new("L", (scr_w, scr_h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, scr_w - 1, scr_h - 1), int(9 * s), fill=255)
    img.paste(scene.convert("RGB"), (int(X0 + sx0 * s), int(Y0 + sy0 * s)), mask)
    d.rounded_rectangle(box(sx0, sy0, sx1, sy1), int(9 * s), outline=(8, 12, 24), width=int(2.2 * s))

    def dot(cx, cy, r, fill, ring=None):
        if ring:
            d.ellipse((X0 + (cx - r - 3) * s, Y0 + (cy - r - 3) * s, X0 + (cx + r + 3) * s, Y0 + (cy + r + 3) * s), fill=ring)
        d.ellipse((X0 + (cx - r) * s, Y0 + (cy - r) * s, X0 + (cx + r) * s, Y0 + (cy + r) * s), fill=fill)

    dot(59, 62, 21, (30, 38, 60), shade(TEAL, 0.62))             # left stick
    for ox, oy in ((0, -17), (0, 17), (-17, 0), (17, 0)):       # left buttons
        dot(59 + ox, 150 + oy, 8, (30, 38, 60), shade(TEAL, 0.62))
    for ox, oy in ((0, -17), (0, 17), (-17, 0), (17, 0)):       # right buttons
        dot(W - 59 + ox, 62 + oy, 8, (30, 38, 60), shade(CORAL, 0.62))
    dot(W - 59, 150, 21, (30, 38, 60), shade(CORAL, 0.62))       # right stick
    d.rounded_rectangle(box(112, 14, 120, 18), int(2 * s), fill=EDGE)  # shoulder lines
    bottom = y + H * scale
    if title:
        font = ImageFont.truetype(FONT, int(60 * s))
        text = "NX on Orbis"
        tw = d.textlength(text, font=font)
        tx = X0 + (W * s - tw) / 2
        ty = (bottom + 30 * scale) * SS
        d.text((tx + 2 * s, ty + 3 * s), text, font=font, fill=(0, 0, 10, 255))
        d.text((tx, ty), text, font=font, fill=WHITE + (255,))
        bottom += 100 * scale
    return bottom


def canvas(size, glow_center=None, glow_r=0):
    w, h = size
    img = gradient((w * SS, h * SS), NAVY, INDIGO).convert("RGBA")
    if glow_center:
        glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
        gx, gy = glow_center[0] * SS, glow_center[1] * SS
        ImageDraw.Draw(glow).ellipse([gx - glow_r * SS, gy - glow_r * SS, gx + glow_r * SS, gy + glow_r * SS], fill=BLUE + (95,))
        img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(glow_r * SS * 0.45)))
    return img


# icon0.png: the handheld and the name, centred.
icon = canvas((512, 512), (256, 215), 190)
console(icon, 21, 92, 1.0)
icon.resize((512, 512), Image.LANCZOS).convert("RGB").save("assets/icon0.png")

# pic1.png: the same, large, on the right, dimmed so the home screen's text stays readable.
bg = canvas((1920, 1080), (1330, 520), 520)
console(bg, 880, 280, 2.05, title=False)
final = bg.resize((1920, 1080), Image.LANCZOS).convert("RGB")
final = Image.blend(final, Image.new("RGB", (1920, 1080), NAVY), 0.38)
final.save("assets/pic1.png")
print("assets/icon0.png assets/pic1.png")
