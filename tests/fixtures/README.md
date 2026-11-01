# Image fixtures

Run `python generate_fixtures.py` with Pillow installed to regenerate these
small, original test images. Pillow is only used to author fixtures; LiteView
itself uses Windows Imaging Component and has no Pillow dependency.

The eight JPEGs share the same four colored quadrants and differ in EXIF
orientation. They also contain known camera and exposure tags. The PNG tests
premultiplied alpha, the GIF has two different frames, and the damaged JPEG
tests failure handling.
