#!/usr/bin/env python3
"""Draw Natter's Haiku vector icon: a speech bubble with three dots.

    python3 tools/make-icon.py resources/Natter.hvif [preview.png]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hvif  # noqa: E402

K = 0.5523


def rounded_rect(l, t, r, b, radius):
    k = radius * K
    return {'closed': True, 'points': [
        ((l + radius, t), (l + radius - k, t), (l + radius, t)),
        ((r - radius, t), (r - radius, t), (r - radius + k, t)),
        ((r, t + radius), (r, t + radius - k), (r, t + radius)),
        ((r, b - radius), (r, b - radius), (r, b - radius + k)),
        ((r - radius, b), (r - radius + k, b), (r - radius, b)),
        ((l + radius, b), (l + radius, b), (l + radius - k, b)),
        ((l, b - radius), (l, b - radius + k), (l, b - radius)),
        ((l, t + radius), (l, t + radius), (l, t + radius - k)),
    ]}


def circle(cx, cy, r):
    k = r * K
    return {'closed': True, 'points': [
        ((cx + r, cy), (cx + r, cy - k), (cx + r, cy + k)),
        ((cx, cy + r), (cx + k, cy + r), (cx - k, cy + r)),
        ((cx - r, cy), (cx - r, cy + k), (cx - r, cy - k)),
        ((cx, cy - r), (cx - k, cy - r), (cx + k, cy - r)),
    ]}


def build():
    styles = [
        hvif.linear_gradient((10, 8), (54, 52), [(0.0, (74, 196, 176, 255)), (1.0, (28, 120, 150, 255))]),
        {'color': (16, 70, 92, 255)},
        {'color': (255, 255, 255, 255)},
        hvif.linear_gradient((12, 30), (52, 60), [(0.0, (250, 176, 60, 255)), (1.0, (226, 110, 40, 255))]),
    ]
    paths = [
        rounded_rect(4, 6, 50, 40, 11),                                     # 0 bubble
        {'closed': True, 'points': [(12, 38), (10, 50), (24, 39)]},         # 1 tail
        circle(16, 23, 3.6), circle(27, 23, 3.6), circle(38, 23, 3.6),     # 2-4 dots
        rounded_rect(28, 32, 60, 56, 8),                                    # 5 reply bubble
        {'closed': True, 'points': [(50, 54), (56, 62), (54, 53)]},         # 6 reply tail
    ]
    shapes = [
        {'style': 3, 'paths': [5, 6]},
        {'style': 1, 'paths': [0, 1], 'transformers': [{'type': 'contour', 'width': 1.4}]},
        {'style': 0, 'paths': [0, 1]},
        {'style': 2, 'paths': [2, 3, 4]},
    ]
    return {'styles': styles, 'paths': paths, 'shapes': shapes}


def main():
    icon = build()
    data = hvif.encode(icon)
    with open(sys.argv[1], 'wb') as out:
        out.write(data)
    print(f'{sys.argv[1]}: {len(data)} bytes')
    if len(sys.argv) > 2:
        hvif.preview(icon, 256).save(sys.argv[2])


if __name__ == '__main__':
    main()
