import struct
import os

png_files = [
    (16, 16, r"app\rPlayHub\Assets.xcassets\AppIcon.appiconset\icon_16x16.png"),
    (32, 32, r"app\rPlayHub\Assets.xcassets\AppIcon.appiconset\icon_32x32.png"),
    (64, 64, r"app\rPlayHub\Assets.xcassets\AppIcon.appiconset\icon_32x32@2x.png"),
    (128, 128, r"app\rPlayHub\Assets.xcassets\AppIcon.appiconset\icon_128x128.png"),
    (256, 256, r"app\rPlayHub\Assets.xcassets\AppIcon.appiconset\icon_256x256.png"),
]

images = []
for w, h, path in png_files:
    if os.path.exists(path):
        with open(path, "rb") as f:
            data = f.read()
            images.append((w, h, data))
            print(f"Loaded {path} ({w}x{h}, {len(data)} bytes)")

# Write ICO
num_images = len(images)
header = struct.pack("<HHH", 0, 1, num_images)
offset = 6 + 16 * num_images

entries = bytearray()
payload = bytearray()

for w, h, data in images:
    bw = 0 if w >= 256 else w
    bh = 0 if h >= 256 else h
    size = len(data)
    entry = struct.pack("<BBBBHHII", bw, bh, 0, 0, 1, 32, size, offset)
    entries += entry
    payload += data
    offset += size

out_path = r"client-c\rplay-gui.ico"
with open(out_path, "wb") as f:
    f.write(header + entries + payload)
print(f"Wrote {out_path} ({os.path.getsize(out_path)} bytes)")
