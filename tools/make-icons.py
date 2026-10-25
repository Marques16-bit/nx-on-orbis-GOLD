# Regenerates assets/icon0.png (512x512) and assets/pic1.png (1920x1080), the package's icon and
# the background of its page on the PS4 home screen. Original artwork: a planet with its orbit.
# Usage: python3 tools/make-icons.py [path to DejaVuSans-Bold.ttf]
import math
import sys
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
NAVY, BLUE, TEAL, WHITE = (11, 16, 32), (74, 134, 255), (45, 212, 196), (234, 240, 255)
SS = 4  # supersampling for clean edges


def gradient(size, top, bottom, diagonal=True):
    w, h = size
    img = Image.new("RGB", size)
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = (x / w * 0.5 + y / h * 0.5) if diagonal else y / h
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return img


def orbit_layer(size, cx, cy, rx, ry, angle, width, color, front):
    """The orbit: an ellipse ring, tilted. `front` keeps the half in front of the planet (the lower
    half before tilting), otherwise the half behind it. Drawn upright and then rotated, so the
    edge is smooth."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).ellipse([cx - rx - width / 2, cy - ry - width / 2, cx + rx + width / 2, cy + ry + width / 2],
                                  outline=color + (255,), width=int(width))
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rectangle([0, cy if front else 0, size[0], size[1] if front else cy], fill=255)
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))
    return layer.rotate(-math.degrees(angle), resample=Image.BICUBIC, center=(cx, cy))


def scene(size, planet_r, cx, cy, text, ring_w, caption=None):
    w, h = size
    base = gradient(size, NAVY, (28, 52, 130)).convert("RGBA")
    # soft glow behind the planet
    glow = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([cx - planet_r * 1.9, cy - planet_r * 1.9, cx + planet_r * 1.9, cy + planet_r * 1.9], fill=BLUE + (70,))
    base.alpha_composite(glow.filter(ImageFilter.GaussianBlur(planet_r * 0.55)))
    angle = math.radians(-24)
    rx, ry = planet_r * 1.75, planet_r * 0.62
    base.alpha_composite(orbit_layer(size, cx, cy, rx, ry, angle, ring_w, TEAL, front=False))  # behind
    # planet: a lit sphere
    sphere = Image.new("RGBA", size, (0, 0, 0, 0))
    sp = sphere.load()
    for y in range(int(cy - planet_r) - 1, int(cy + planet_r) + 2):
        for x in range(int(cx - planet_r) - 1, int(cx + planet_r) + 2):
            dx, dy = (x - cx) / planet_r, (y - cy) / planet_r
            r2 = dx * dx + dy * dy
            if r2 <= 1.0:
                light = max(0.0, 1.0 - math.hypot(dx + 0.35, dy + 0.4) / 1.6)
                c = [int(BLUE[i] * (0.45 + 0.75 * light) + 255 * 0.12 * light ** 3) for i in range(3)]
                sp[x, y] = (min(c[0], 255), min(c[1], 255), min(c[2], 255), 255)
    base.alpha_composite(sphere)
    # the letters
    font = ImageFont.truetype(FONT, int(planet_r * 0.95))
    tw = ImageDraw.Draw(base).textlength(text, font=font)
    shadow = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((cx - tw / 2 + planet_r * 0.03, cy - planet_r * 0.62 + planet_r * 0.04), text, font=font, fill=(5, 10, 40, 120))
    base.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(planet_r * 0.03)))
    ImageDraw.Draw(base).text((cx - tw / 2, cy - planet_r * 0.62), text, font=font, fill=WHITE + (255,))
    base.alpha_composite(orbit_layer(size, cx, cy, rx, ry, angle, ring_w, TEAL, front=True))  # in front
    # the satellite, on the orbit's upper right
    a = math.radians(-58)
    x, y = rx * math.cos(a), ry * math.sin(a)
    sx, sy = cx + x * math.cos(angle) - y * math.sin(angle), cy + x * math.sin(angle) + y * math.cos(angle)
    sr = planet_r * 0.13
    ImageDraw.Draw(base).ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=WHITE + (255,))
    if caption:
        cf = ImageFont.truetype(FONT, int(planet_r * 0.30))
        spaced = " ".join(caption)
        cw = ImageDraw.Draw(base).textlength(spaced, font=cf)
        ImageDraw.Draw(base).text(((w - cw) / 2, h - planet_r * 0.62), spaced, font=cf, fill=TEAL + (255,))
    return base


def render(size, planet_r, cx, cy, text, ring_w, caption, out):
    big = (size[0] * SS, size[1] * SS)
    img = scene(big, planet_r * SS, cx * SS, cy * SS, text, ring_w * SS, caption)
    img.resize(size, Image.LANCZOS).convert("RGB").save(out)


# icon0.png: planet and orbit centred, a caption at the foot.
render((512, 512), 128, 256, 232, "NX", 11, "ON ORBIS", "assets/icon0.png")

# pic1.png: the same scene wide, dimmer, off to the right, so the home screen's text stays readable.
wide = scene((1920 * SS // 2, 1080 * SS // 2), 250 * SS // 2, 1330 * SS // 2, 520 * SS // 2, "NX", 20 * SS // 2, None)
wide = wide.resize((1920, 1080), Image.LANCZOS).convert("RGB")
wide = Image.blend(wide, Image.new("RGB", (1920, 1080), NAVY), 0.35)
wide.save("assets/pic1.png")
print("assets/icon0.png assets/pic1.png")
