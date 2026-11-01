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


def write_pdf(path, pages):
    """Write a minimal PDF: each page is (width, height) in points with a red box."""
    objects = ["<< /Type /Catalog /Pages 2 0 R >>"]
    kids = " ".join(f"{3 + 2 * i} 0 R" for i in range(len(pages)))
    objects.append(f"<< /Type /Pages /Kids [{kids}] /Count {len(pages)} >>")
    for i, (w, h) in enumerate(pages):
        content = f"1 0 0 rg {w // 4} {h // 4} {w // 2} {h // 2} re f".encode()
        objects.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {w} {h}] /Contents {4 + 2 * i} 0 R >>")
        objects.append(f"<< /Length {len(content)} >>\nstream\n{content.decode()}\nendstream")
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n{body}\nendobj\n".encode()
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    path.write_bytes(bytes(out))


write_pdf(DEST / "two-pages.pdf", [(200, 100), (100, 200)])
