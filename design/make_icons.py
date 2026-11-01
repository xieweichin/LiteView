# Builds the app icon, README logo and installer header images from the
# Codex logo artwork (design/LiteView-logo-concept-01.png).
#
#   python design/make_icons.py        (run from the repo root; needs Pillow)
#
# Outputs: src/res/LiteView.ico, docs/logo.png, installer/WizardSmall-55.bmp,
# installer/WizardSmall-110.bmp.
from PIL import Image, ImageFilter

SRC = 'design/LiteView-logo-concept-01.png'
ICON_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

im = Image.open(SRC).convert('RGBA')
r, g, b, a = im.split()
a = a.point(lambda v: 0 if v < 12 else v)  # drop faint specks around the glow
im = Image.merge('RGBA', (r, g, b, a)).crop(a.getbbox())
w, h = im.size
side = int(max(w, h) * 1.02)
square = Image.new('RGBA', (side, side), (0, 0, 0, 0))
square.paste(im, ((side - w) // 2, (side - h) // 2))

icons = []
for n in ICON_SIZES:
    x = square.resize((n, n), Image.LANCZOS)
    if n <= 48:
        x = x.filter(ImageFilter.UnsharpMask(radius=0.6, percent=60, threshold=0))
    icons.append(x)
icons[-1].save('src/res/LiteView.ico', format='ICO',
               sizes=[(n, n) for n in ICON_SIZES], append_images=icons[:-1])

square.resize((512, 512), Image.LANCZOS).save('docs/logo.png', optimize=True)

for w, h in [(55, 58), (110, 116)]:
    n = min(w, h)
    bg = Image.new('RGB', (w, h), (255, 255, 255))
    x = square.resize((n, n), Image.LANCZOS)
    bg.paste(x, ((w - n) // 2, (h - n) // 2), x)
    bg.save(f'installer/WizardSmall-{w}.bmp')
