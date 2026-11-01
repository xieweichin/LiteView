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


def write_3mf(path):
    """Write a deflated 3MF package: a 10 mm cube in red, placed twice by a component object."""
    import zipfile

    v = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0), (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)]
    t = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    vertices = "".join(f'<vertex x="{x}" y="{y}" z="{z}"/>' for x, y, z in v)
    triangles = "".join(f'<triangle v1="{a}" v2="{b}" v3="{c}"/>' for a, b, c in t)
    model = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
        '<resources><basematerials id="1"><base name="Red" displaycolor="#FF0000"/></basematerials>'
        f'<object id="2" type="model" pid="1" pindex="0"><mesh><vertices>{vertices}</vertices>'
        f'<triangles>{triangles}</triangles></mesh></object>'
        '<object id="3" type="model"><components><component objectid="2"/>'
        '<component objectid="2" transform="1 0 0 0 1 0 0 0 1 20 0 0"/></components></object>'
        '</resources><build><item objectid="3"/></build></model>')
    rels = ('<?xml version="1.0" encoding="UTF-8"?>'
            '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
            'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
    types = ('<?xml version="1.0" encoding="UTF-8"?>'
             '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
             '</Types>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as package:
        for name, text in (("[Content_Types].xml", types), ("_rels/.rels", rels), ("3D/3dmodel.model", model)):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            package.writestr(info, text)


write_3mf(DEST / "two-cubes.3mf")
