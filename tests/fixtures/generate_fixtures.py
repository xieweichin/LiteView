"""Regenerate the tiny test pictures (authoring dependency: Pillow)."""
from pathlib import Path

from PIL import Image, ImageDraw, TiffImagePlugin

DEST = Path(__file__).resolve().parent
image = Image.new("RGB", (64, 40))
draw = ImageDraw.Draw(image)
draw.rectangle((0, 0, 31, 19), fill=(240, 20, 20))
draw.rectangle((32, 0, 63, 19), fill=(20, 220, 20))
draw.rectangle((0, 20, 31, 39), fill=(20, 20, 240))
draw.rectangle((32, 20, 63, 39), fill=(240, 220, 20))

for orientation in range(1, 9):
    exif = Image.Exif()
    if orientation == 1:
        exif.endian = "<"  # Exercise little-endian EXIF as well as big-endian.
    exif[274] = orientation
    exif[271] = "LiteView Test"
    exif[272] = "Fixture Camera"
    exif[34665] = {
        36867: "2026:11:01 12:34:56",
        33434: TiffImagePlugin.IFDRational(1, 125),
        33437: TiffImagePlugin.IFDRational(28, 10),
        34855: 200,
        37386: TiffImagePlugin.IFDRational(35, 1),
    }
    image.save(DEST / f"orientation-{orientation}.jpg", quality=95, subsampling=0, exif=exif)

alpha = Image.new("RGBA", (2, 1))
alpha.putdata([(200, 100, 50, 128), (255, 255, 255, 0)])
alpha.save(DEST / "alpha.png")
image.save(DEST / "basic.bmp")
image.save(DEST / "basic.tiff")
with Image.open(DEST / "orientation-6.jpg") as original:
    tiff_exif = original.getexif()
    tiff_exif[271] = "LiteView TIFF"
    original.save(DEST / "oriented.tiff", exif=tiff_exif)
image.resize((32, 32)).convert("RGBA").save(DEST / "basic.ico", sizes=[(32, 32)])
first = Image.new("RGB", (8, 8), (240, 20, 20))
second = Image.new("RGB", (8, 8), (20, 20, 240))
first.save(DEST / "two-frames.gif", save_all=True, append_images=[second], duration=100, loop=0)
(DEST / "broken.jpg").write_bytes(b"not a JPEG")
