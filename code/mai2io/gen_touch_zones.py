"""
Generate touch_zones.h: pixel-to-sensor lookup table for maimai DX touch panel.

Correct layout (from touch_area.png):
  Ring 1 (outer, 2x radial width): A1-A8 and D1-D8 share the same ring.
    - D zones sit at the divider angles (0, 45, 90, ...) = narrow (15 deg)
    - A zones fill the gaps between D zones (22.5, 67.5, ...) = wide (30 deg)
  Ring 2 (inner): B1-B8 and E1-E8 share the same ring.
    - E zones sit at the divider angles (0, 45, 90, ...) = narrow (15 deg)
    - B zones fill the gaps (22.5, 67.5, ...) = wide (30 deg)
  Center: C1 (right half), C2 (left half)
  Game buttons: outside ring 1, at divider angles (0, 45, 90, ...)

Angular widths (per 45-degree sector = 1 D + 1 A):
  D_width = 15 deg (half of A)
  A_width = 30 deg
  Same for E and B.

Sensor IDs:
  0-7:   A1-A8   (wide sectors on outer ring)
  8-15:  B1-B8   (wide sectors on inner ring)
  16:    C1      (center right)
  17:    C2      (center left)
  18-25: D1-D8   (narrow sectors on outer ring, at dividers)
  26-33: E1-E8   (narrow sectors on inner ring, at dividers)
  34-41: Game buttons 1-8 (outside, at divider angles)
  255:   nothing
"""

import math

WIDTH = 800
HEIGHT = 800
CENTER_X = 400
CENTER_Y = 400
RADIUS_OUTER = 350.0

# Radii (normalized to RADIUS_OUTER = 1.0)
R_C         = 0.20   # C zone: 0 to 0.20
R_RING3_IN  = 0.20   # Ring 3 (B) inner edge
R_RING3_OUT = 0.45   # Ring 3 (B) outer edge
R_RING2_IN  = 0.45   # Ring 2 (E) inner edge
R_RING2_OUT = 0.70   # Ring 2 (E) outer edge
R_RING1_IN  = 0.70   # Ring 1 (A+D) inner edge
R_RING1_OUT = 1.00   # Ring 1 (A+D) outer edge
R_BTN_IN    = 1.00   # Button zone inner
R_BTN_OUT   = 1.12   # Button zone outer

# Angular layout:
# D (narrow) = 15 deg, A (wide) = 30 deg.  (15 + 30 = 45 per pair)
# D1 centered at 0 deg, A1 centered at 22.5 deg, D2 at 45, A2 at 67.5, ...
# Same for E/B on ring 2.
# Buttons at D positions (0, 45, 90, ...).
D_HALF_WIDTH = 7.5   # half of 15 deg
A_HALF_WIDTH = 15.0   # half of 30 deg

SENSOR_NONE = 255

def angle_from_top_cw(x, y):
    """Angle in degrees, 0=top(north), increasing clockwise, range [0, 360)."""
    dx = x - CENTER_X
    dy = y - CENTER_Y
    a = math.degrees(math.atan2(dx, -dy))
    if a < 0:
        a += 360.0
    return a

def angle_diff(a, b):
    """Shortest signed angular difference, result in [-180, 180)."""
    d = (a - b + 180) % 360 - 180
    return d

def classify_pixel(x, y):
    dx = x - CENTER_X
    dy = y - CENTER_Y
    dist = math.sqrt(dx*dx + dy*dy)
    r = dist / RADIUS_OUTER

    if r > R_BTN_OUT:
        return SENSOR_NONE

    angle = angle_from_top_cw(x, y)

    # --- Button zone ---
    if r >= R_BTN_IN:
        for i in range(8):
            btn_center = i * 45.0 + 22.5
            if abs(angle_diff(angle, btn_center)) < A_HALF_WIDTH:
                return 34 + i
        return SENSOR_NONE

    # --- Ring 1 (outer): A + D ---
    if r >= R_RING1_IN:
        # Check D zones first (narrow, at 0, 45, 90, ...)
        for i in range(8):
            d_center = i * 45.0
            if abs(angle_diff(angle, d_center)) < D_HALF_WIDTH:
                return 18 + i  # D1-D8
        # Check A zones (wide, at 22.5, 67.5, ...)
        for i in range(8):
            a_center = i * 45.0 + 22.5
            if abs(angle_diff(angle, a_center)) < A_HALF_WIDTH:
                return 0 + i   # A1-A8
        return SENSOR_NONE

    # --- Ring 2 (second from outside): E ---
    if r >= R_RING2_IN:
        # E zones take the full 45 degrees, centered at 0, 45, 90...
        for i in range(8):
            e_center = i * 45.0
            if abs(angle_diff(angle, e_center)) <= 22.5:
                return 26 + i  # E1-E8
        return SENSOR_NONE

    # --- Ring 3 (third from outside): B ---
    if r >= R_RING3_IN:
        # B zones take the full 45 degrees, centered at 22.5, 67.5...
        for i in range(8):
            b_center = i * 45.0 + 22.5
            if abs(angle_diff(angle, b_center)) <= 22.5:
                return 8 + i   # B1-B8
        return SENSOR_NONE

    # --- Center: C1/C2 ---
    if r < R_C:
        if dx >= 0:
            return 16  # C1 (right)
        else:
            return 17  # C2 (left)

    return SENSOR_NONE

print("Generating touch zone lookup table...")
lookup = bytearray(WIDTH * HEIGHT)
for py in range(HEIGHT):
    for px in range(WIDTH):
        lookup[py * WIDTH + px] = classify_pixel(px, py)
    if py % 100 == 0:
        print(f"  Row {py}/{HEIGHT}...")

# Verify zone coverage
zone_counts = {}
for v in lookup:
    zone_counts[v] = zone_counts.get(v, 0) + 1
print("\nZone pixel counts:")
names = {255: "NONE"}
for i in range(8): names[i] = f"A{i+1}"
for i in range(8): names[8+i] = f"B{i+1}"
names[16] = "C1"; names[17] = "C2"
for i in range(8): names[18+i] = f"D{i+1}"
for i in range(8): names[26+i] = f"E{i+1}"
for i in range(8): names[34+i] = f"Btn{i+1}"
for k in sorted(zone_counts.keys()):
    print(f"  {names.get(k, k):6s}: {zone_counts[k]:>7d} px")

# print("\nWriting touch_zones.bin...")
# with open("touch_zones.bin", "wb") as f:
#     f.write(lookup)

print("Writing touch_zones.h...")
with open("touch_zones.h", "w") as f:
    f.write("#pragma once\n\n")
    f.write("// Auto-generated touch zone lookup table.\n")
    f.write("// Maps each pixel (x,y) in 800x800 GUI area to a sensor ID.\n")
    f.write("// Sensor IDs:\n")
    f.write("//   0-7:   A1-A8 (wide sectors, outer ring)\n")
    f.write("//   8-15:  B1-B8 (wide sectors, inner ring)\n")
    f.write("//   16:    C1 (center right)\n")
    f.write("//   17:    C2 (center left)\n")
    f.write("//   18-25: D1-D8 (narrow sectors, outer ring, at dividers)\n")
    f.write("//   26-33: E1-E8 (narrow sectors, inner ring, at dividers)\n")
    f.write("//   34-41: Game buttons 1-8\n")
    f.write("//   255:   No zone\n")
    f.write(f"#define TOUCH_ZONE_WIDTH {WIDTH}\n")
    f.write(f"#define TOUCH_ZONE_HEIGHT {HEIGHT}\n")
    f.write("#define TOUCH_ZONE_NONE 255\n")
    f.write("#define TOUCH_ZONE_BTN_BASE 34\n\n")
    f.write("// RLE-compressed lookup table.\n")
    f.write("// Format: triplets of (count_high, count_low, value).\n")
    f.write("// Decompress by repeating 'value' 'count' times.\n")

    # RLE encode
    rle = []
    i = 0
    while i < len(lookup):
        val = lookup[i]
        count = 1
        while i + count < len(lookup) and lookup[i + count] == val and count < 65535:
            count += 1
        rle.append((count, val))
        i += count

    f.write(f"\n#define TOUCH_ZONE_RLE_LEN {len(rle)}\n\n")
    f.write("static const unsigned char touch_zone_rle[][3] = {\n")
    for idx, (count, val) in enumerate(rle):
        hi = (count >> 8) & 0xFF
        lo = count & 0xFF
        if idx % 10 == 0:
            f.write("    ")
        f.write(f"{{{hi},{lo},{val}}},")
        if idx % 10 == 9:
            f.write("\n")
    f.write("\n};\n")

print(f"\nDone! RLE entries: {len(rle)}, original: {len(lookup)} bytes")
print(f"RLE size: {len(rle) * 3} bytes ({len(rle)*3*100//len(lookup)}% of original)")
