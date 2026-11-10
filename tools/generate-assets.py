#!/usr/bin/env python3
"""Generates Zalie's artwork: the logo (vector + raster sizes) and the sprite sheet of the dog.

Everything is drawn procedurally (SVG), from a few design constants inspired by Zalie herself: a white, fluffy
Coton de Tulear with long floppy ears, dark round eyes, a dark brown-pink nose, a pink tongue and a plumed tail curled
over her back. The SVG files are rasterized with Inkscape, the sprite sheet is assembled with Pillow.

Usage: tools/generate-assets.py [--out assets] [--inkscape inkscape]
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

# --------------------------------------------------------------------------------------------------------------------
# Palette ("Coton" theme)
# --------------------------------------------------------------------------------------------------------------------
FUR = "#FFFDF8"
FUR_SHADE = "#EFE4D6"
FUR_DEEP = "#E2D3C1"
OUTLINE = "#5A463E"
EYE = "#2A1C19"
NOSE = "#4A3033"
TONGUE = "#F2899A"
TONGUE_DARK = "#D9677B"
BADGE_TOP = "#F7A982"
BADGE_BOTTOM = "#DB6544"
SHADOW = "#3A2A24"
MOUTH = "#6B3A3F"
MEAT = "#C65A3A"
MEAT_LIGHT = "#DC7B55"
MEAT_DARK = "#8E3A26"
MEAT_INNER = "#F2A58C"
BONE = "#FFF4E2"


def circle(cx: float, cy: float, r: float, fill: str, stroke: str | None = None, width: float = 0.0,
           extra: str = "") -> str:
    stroke_attr = f' stroke="{stroke}" stroke-width="{width:.2f}"' if stroke else ""
    return f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{r:.2f}" fill="{fill}"{stroke_attr} {extra}/>'


def ellipse(cx: float, cy: float, rx: float, ry: float, fill: str, extra: str = "") -> str:
    return f'<ellipse cx="{cx:.2f}" cy="{cy:.2f}" rx="{rx:.2f}" ry="{ry:.2f}" fill="{fill}" {extra}/>'


@dataclass
class Blob:
    """A fluffy shape: the union of circles. Drawn outline first (all circles, enlarged), then fill, so that the
    outline only follows the outer contour of the union - the trick that makes the fur look like a cloud."""

    circles: list[tuple[float, float, float]]
    fill: str = FUR

    def outline(self, width: float) -> str:
        return "".join(circle(x, y, r + width, OUTLINE) for x, y, r in self.circles)

    def body(self) -> str:
        return "".join(circle(x, y, r, self.fill) for x, y, r in self.circles)

    def svg(self, width: float) -> str:
        return self.outline(width) + self.body()


def fluffy_ellipse(cx: float, cy: float, rx: float, ry: float, bump: float, count: int,
                   start: float = 0.0, jitter: float = 0.18) -> list[tuple[float, float, float]]:
    """Circles along an ellipse (scalloped border) plus inner circles filling it."""
    circles = []
    for i in range(count):
        a = start + 2 * math.pi * i / count
        r = bump * (1.0 + jitter * math.sin(3.7 * i + 1.3))
        circles.append((cx + (rx - bump * 0.55) * math.cos(a), cy + (ry - bump * 0.55) * math.sin(a), r))
    # Core
    circles.append((cx, cy, min(rx, ry) * 0.92))
    circles.append((cx - rx * 0.35, cy, min(rx, ry) * 0.8))
    circles.append((cx + rx * 0.35, cy, min(rx, ry) * 0.8))
    return circles


def chain(points: list[tuple[float, float]], r0: float, r1: float, steps: int) -> list[tuple[float, float, float]]:
    """Circles along a polyline, radius interpolated from r0 to r1."""
    total = sum(math.dist(points[i], points[i + 1]) for i in range(len(points) - 1))
    out = []
    for s in range(steps + 1):
        t = s / steps
        target = t * total
        acc = 0.0
        for i in range(len(points) - 1):
            seg = math.dist(points[i], points[i + 1])
            if acc + seg >= target or i == len(points) - 2:
                u = 0.0 if seg == 0 else min(1.0, (target - acc) / seg)
                x = points[i][0] + (points[i + 1][0] - points[i][0]) * u
                y = points[i][1] + (points[i + 1][1] - points[i][1]) * u
                out.append((x, y, r0 + (r1 - r0) * t))
                break
            acc += seg
    return out


def bezier(p0, p1, p2, steps: int = 16) -> list[tuple[float, float]]:
    pts = []
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0]
        y = (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]
        pts.append((x, y))
    return pts


def rotate(points, cx, cy, angle):
    c, s = math.cos(angle), math.sin(angle)
    return [(cx + (x - cx) * c - (y - cy) * s, cy + (x - cx) * s + (y - cy) * c, *rest) for x, y, *rest in points]


def svg_document(width: int, height: int, content: str, defs: str = "") -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}"><defs>{defs}</defs>{content}</svg>')


# --------------------------------------------------------------------------------------------------------------------
# Logo: Zalie's face, front view
# --------------------------------------------------------------------------------------------------------------------
def logo_face(cx: float, cy: float, scale: float, outline: float) -> str:
    s = scale
    parts = []
    # Long floppy ears, behind the head, a bit darker.
    for side in (-1, 1):
        ear_path = bezier((cx + side * 92 * s, cy - 70 * s), (cx + side * 150 * s, cy + 10 * s),
                          (cx + side * 120 * s, cy + 120 * s), 10)
        ear = Blob(chain(ear_path, 44 * s, 30 * s, 10), FUR_SHADE)
        # A few extra tufts at the tip.
        ear.circles += [(cx + side * 132 * s, cy + 112 * s, 22 * s), (cx + side * 104 * s, cy + 130 * s, 22 * s)]
        parts.append(ear.svg(outline))
    # Head: a cloud of fur, with a fringe on top.
    head = Blob(fluffy_ellipse(cx, cy, 128 * s, 116 * s, 30 * s, 22, start=0.1))
    head.circles += [(cx - 46 * s, cy - 112 * s, 30 * s), (cx + 6 * s, cy - 124 * s, 32 * s),
                     (cx + 56 * s, cy - 108 * s, 28 * s)]
    parts.append(head.svg(outline))
    # Soft volume: shade under the eyes and around the muzzle.
    parts.append(ellipse(cx, cy + 58 * s, 70 * s, 46 * s, FUR_SHADE, 'opacity="0.55"'))
    parts.append(ellipse(cx, cy + 50 * s, 58 * s, 38 * s, FUR))
    # Fur strokes on the forehead.
    for dx, rot in ((-40, -18), (0, 0), (40, 18)):
        x = cx + dx * s
        parts.append(f'<path d="M {x - 10 * s:.1f} {cy - 70 * s:.1f} q {10 * s:.1f} {-16 * s:.1f} {20 * s:.1f} 0" '
                     f'fill="none" stroke="{FUR_DEEP}" stroke-width="{5 * s:.1f}" stroke-linecap="round" '
                     f'transform="rotate({rot} {x:.1f} {cy - 72 * s:.1f})"/>')
    # Eyes: dark, round, shiny.
    for side in (-1, 1):
        ex, ey = cx + side * 52 * s, cy - 6 * s
        parts.append(circle(ex, ey, 25 * s, FUR_SHADE))
        parts.append(circle(ex, ey, 20 * s, EYE))
        parts.append(circle(ex - 6 * s, ey - 7 * s, 6.5 * s, "#FFFFFF"))
        parts.append(circle(ex + 7 * s, ey + 6 * s, 2.8 * s, "#FFFFFF", extra='opacity="0.7"'))
    # Nose.
    nx, ny = cx, cy + 40 * s
    parts.append(f'<path d="M {nx - 26 * s:.1f} {ny - 8 * s:.1f} '
                 f'C {nx - 26 * s:.1f} {ny - 26 * s:.1f} {nx + 26 * s:.1f} {ny - 26 * s:.1f} {nx + 26 * s:.1f} '
                 f'{ny - 8 * s:.1f} C {nx + 26 * s:.1f} {ny + 8 * s:.1f} {nx + 8 * s:.1f} {ny + 18 * s:.1f} '
                 f'{nx:.1f} {ny + 18 * s:.1f} C {nx - 8 * s:.1f} {ny + 18 * s:.1f} {nx - 26 * s:.1f} '
                 f'{ny + 8 * s:.1f} {nx - 26 * s:.1f} {ny - 8 * s:.1f} Z" fill="{NOSE}"/>')
    parts.append(ellipse(nx - 8 * s, ny - 12 * s, 8 * s, 4.5 * s, "#FFFFFF", 'opacity="0.45"'))
    # Tongue, then the smile over it.
    tx, ty = cx, cy + 74 * s
    parts.append(f'<path d="M {tx - 18 * s:.1f} {ty - 6 * s:.1f} L {tx + 18 * s:.1f} {ty - 6 * s:.1f} '
                 f'L {tx + 18 * s:.1f} {ty + 16 * s:.1f} A {18 * s:.1f} {18 * s:.1f} 0 0 1 {tx - 18 * s:.1f} '
                 f'{ty + 16 * s:.1f} Z" fill="{TONGUE}" stroke="{OUTLINE}" stroke-width="{outline * 0.6:.1f}"/>')
    parts.append(f'<path d="M {tx:.1f} {ty - 2 * s:.1f} L {tx:.1f} {ty + 18 * s:.1f}" stroke="{TONGUE_DARK}" '
                 f'stroke-width="{3.5 * s:.1f}" stroke-linecap="round"/>')
    parts.append(f'<path d="M {nx:.1f} {ny + 16 * s:.1f} L {nx:.1f} {ny + 26 * s:.1f} '
                 f'Q {nx - 12 * s:.1f} {ny + 40 * s:.1f} {nx - 30 * s:.1f} {ny + 30 * s:.1f} M {nx:.1f} '
                 f'{ny + 26 * s:.1f} Q {nx + 12 * s:.1f} {ny + 40 * s:.1f} {nx + 30 * s:.1f} {ny + 30 * s:.1f}" '
                 f'fill="none" stroke="{OUTLINE}" stroke-width="{outline * 0.6:.1f}" stroke-linecap="round" '
                 f'stroke-linejoin="round"/>')
    return "".join(parts)


def logo_svg(size: int = 512, badge: bool = True) -> str:
    defs = (f'<linearGradient id="badge" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0" stop-color="{BADGE_TOP}"/><stop offset="1" stop-color="{BADGE_BOTTOM}"/>'
            f'</linearGradient>')
    content = ""
    if badge:
        content += f'<rect x="0" y="0" width="{size}" height="{size}" rx="{size * 0.22:.1f}" fill="url(#badge)"/>'
        content += ellipse(size / 2, size * 0.86, size * 0.3, size * 0.05, SHADOW, 'opacity="0.18"')
    scale = size / 512 * (0.95 if badge else 1.12)
    content += logo_face(size / 2, size * (0.50 if badge else 0.48), scale, 9 * scale)
    return svg_document(size, size, content, defs)


# --------------------------------------------------------------------------------------------------------------------
# Running dog (side view, facing right)
# --------------------------------------------------------------------------------------------------------------------
FRAME_W, FRAME_H = 160, 120
RUN_FRAMES = 8


def leg(joint, upper_angle, lower_angle, upper_len, lower_len, width, fill, outline) -> str:
    """A fluffy two-segment leg with a paw, from 'joint' (angles in radians from the vertical, positive forward)."""
    jx, jy = joint
    kx = jx + upper_len * math.sin(upper_angle)
    ky = jy + upper_len * math.cos(upper_angle)
    px = kx + lower_len * math.sin(lower_angle)
    py = ky + lower_len * math.cos(lower_angle)
    blob = Blob(chain([(jx, jy), (kx, ky), (px, py)], width, width * 0.8, 6), fill)
    blob.circles.append((px + 2.5 * math.cos(lower_angle), py - 1.0, width * 0.95))
    return blob.svg(outline)


def running_dog(t: float, outline: float = 2.6) -> str:
    """One frame of the gallop, t in [0, 1): extended (legs reaching out) then gathered (legs under the belly)."""
    p = 2 * math.pi * t
    stretch = math.sin(p)  # +1 extended, -1 gathered
    bob = -6.0 * max(0.0, -stretch) - 2.0 * abs(math.cos(p))  # suspension when gathered
    tilt = -5.0 * math.cos(p)
    parts = []
    # Ground shadow, smaller when the dog is up.
    shadow_scale = 1.0 + bob / 30.0
    parts.append(ellipse(82, 111, 44 * shadow_scale, 4.5 * shadow_scale, SHADOW, 'opacity="0.16"'))

    body_cy = 68 + bob
    rx = 36 * (1.0 + 0.07 * stretch)
    dog = []

    def front(phase):
        s = math.sin(phase)
        a = 0.95 * s
        # Folds (paw curled back) while swinging back under the chest.
        return a, a - 1.3 * max(0.0, -math.sin(phase + 0.5))

    def back(phase):
        s = math.sin(phase)
        a = -0.85 * s
        return a, a + 1.1 * max(0.0, -math.sin(phase - 0.4)) - 0.35

    front_x, back_x = 76 + rx * 0.72, 76 - rx * 0.72
    # Far legs (behind the body), slightly darker, a bit out of phase.
    fa, fb = front(p + 0.45)
    dog.append(leg((front_x - 4, body_cy + 8), fa, fb, 13, 12, 6.0, FUR_SHADE, outline))
    ba, bb = back(p + 0.45)
    dog.append(leg((back_x + 4, body_cy + 8), ba, bb, 13, 13, 6.5, FUR_SHADE, outline))

    # Tail: a plume curled over the back, wagging.
    wag = 0.3 * math.sin(2 * p)
    tail_pts = bezier((back_x + 4, body_cy - 12), (back_x - 22, body_cy - 30), (back_x + 4, body_cy - 42), 8)
    tail = Blob([(x, y, 8.5 - 2.0 * i / 8) for i, (x, y) in enumerate(tail_pts)])
    tail.circles += [(back_x + 9, body_cy - 42, 6.0)]
    tail.circles = rotate(tail.circles, back_x + 4, body_cy - 10, wag)
    dog.append(tail.svg(outline))

    # Body: a fluffy ellipse, stretching and compressing with the stride.
    body = Blob(fluffy_ellipse(76, body_cy, rx, 21, 9.0, 16, start=0.3, jitter=0.25))
    dog.append(body.svg(outline))

    # Near legs.
    na, nb = front(p)
    dog.append(leg((front_x, body_cy + 9), na, nb, 13, 12, 6.5, FUR, outline))
    ba2, bb2 = back(p)
    dog.append(leg((back_x, body_cy + 9), ba2, bb2, 13, 13, 7.0, FUR, outline))

    # Head, following the body with a little delay.
    hx, hy = 76 + rx + 4, body_cy - 20 + 2.0 * math.sin(p - 0.9)
    head = Blob(fluffy_ellipse(hx, hy, 20, 18, 7.0, 12, start=0.2, jitter=0.2))
    head.circles += [(hx + 16, hy + 6, 9.0), (hx + 22, hy + 6, 7.5),  # muzzle
                     (hx - 6, hy - 17, 6.5), (hx + 4, hy - 19, 6.0)]  # tousled fringe
    # Floppy ear on the back of the head, swinging with the gallop.
    swing = 0.45 * math.sin(p - 1.2)
    ear = Blob(chain(bezier((hx - 7, hy - 9), (hx - 15, hy + 2), (hx - 11, hy + 15), 6), 6.2, 4.8, 6), FUR_SHADE)
    ear.circles = rotate(ear.circles, hx - 7, hy - 9, swing)
    # Tongue, panting.
    tongue_y = hy + 15 + 2.0 * max(0.0, stretch)
    dog.append(f'<path d="M {hx + 15:.1f} {tongue_y - 4:.1f} l 8 0 l 0 6 a 4 4 0 0 1 -8 0 Z" fill="{TONGUE}" '
               f'stroke="{OUTLINE}" stroke-width="{outline * 0.7:.2f}"/>')
    dog.append(head.svg(outline))
    dog.append(ear.svg(outline))
    # Eye, nose, mouth.
    dog.append(circle(hx + 9, hy - 3, 3.8, EYE))
    dog.append(circle(hx + 8, hy - 4.4, 1.3, "#FFFFFF"))
    dog.append(ellipse(hx + 30.5, hy + 3.5, 4.2, 3.6, NOSE))
    dog.append(f'<path d="M {hx + 30:.1f} {hy + 7:.1f} q -4 6 -12 5" fill="none" stroke="{OUTLINE}" '
               f'stroke-width="{outline * 0.7:.2f}" stroke-linecap="round"/>')

    parts.append(f'<g transform="rotate({tilt:.2f} 80 {body_cy:.1f})">{"".join(dog)}</g>')
    return "".join(parts)


def sleeping_dog(t: float, outline: float = 2.6) -> str:
    """Zalie asleep, curled up (idle screens). t in [0, 1): breathing."""
    breath = 1.0 + 0.035 * math.sin(2 * math.pi * t)
    parts = [ellipse(80, 104, 50, 5, SHADOW, 'opacity="0.16"')]
    body = Blob(fluffy_ellipse(74, 86, 40 * breath, 18 * breath, 8.5, 16, start=0.3, jitter=0.25))
    tail = Blob([(x, y, 8.0 - 2.0 * i / 8) for i, (x, y) in enumerate(bezier((36, 92), (36, 108), (66, 105), 8))])
    head = Blob(fluffy_ellipse(114, 86, 18, 15, 6.5, 12, start=0.2, jitter=0.2))
    head.circles += [(128, 93, 8.0), (133, 94, 6.5), (108, 72, 5.5), (117, 71, 5.0)]
    ear = Blob(chain(bezier((106, 80), (98, 88), (103, 98), 6), 5.8, 4.6, 6), FUR_SHADE)
    paws = Blob([(118, 102, 5.5), (128, 103, 5.5)])
    parts += [body.svg(outline), tail.svg(outline), paws.svg(outline), head.svg(outline), ear.svg(outline)]
    # Closed eye, nose.
    parts.append(f'<path d="M 117 84 q 4 3 8 0" fill="none" stroke="{EYE}" stroke-width="2.2" '
                 f'stroke-linecap="round"/>')
    parts.append(ellipse(139.5, 91, 3.6, 3.1, NOSE))
    # "z z" floating up.
    for i, (dx, dy, size) in enumerate(((0, 0, 9), (10, -14, 12))):
        rise = (t * 6 + i * 3) % 6
        x, y = 128 + dx + rise, 54 + dy - rise
        opacity = 0.85 - rise * 0.12
        parts.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="DejaVu Sans" font-weight="bold" '
                     f'font-size="{size}" fill="{OUTLINE}" opacity="{opacity:.2f}">z</text>')
    return "".join(parts)


# --------------------------------------------------------------------------------------------------------------------
# Eating dog (Performance tab): Zalie wolfs down a big piece of meat on the bone
# --------------------------------------------------------------------------------------------------------------------
EAT_FRAMES = 4
MEAT_STAGES = 4
MEAT_CENTER = (121.0, 95.0)
MEAT_RADII = (22.0, 15.0)
MEAT_TILT = -12.0
# Bites taken from the side facing Zalie: (x, y, radius) relative to the center of the meat, before its tilt. Each
# stage keeps the bites of the previous ones.
MEAT_BITES = [
    [],
    [(-20, -10, 9.0), (-25, 3, 8.5)],
    [(-9, -14, 9.5), (-12, 1, 10.0), (-15, 14, 8.5)],
    [(3, -13, 11.0), (1, 2, 10.5), (-2, 15, 9.5), (13, -14, 7.0)],
]
# Per stage, everything left of this abscissa is eaten too (the bites alone leave slivers of the outline between them).
# The edge of this half-plane stays under the bites of the stage.
MEAT_CLEARED = [-99.0, -99.0, -19.0, -2.5]
# Width of the juicy flesh along the bitten edges.
MEAT_CUT = 2.8


def bone(outline: float) -> str:
    """The bone through the meat, sticking out on both sides (relative to the center of the meat)."""
    left, right = (-31.0, 3.0), (28.0, -1.0)
    rod = Blob(chain([left, right], 3.8, 3.8, 16), BONE)
    for (x, y), side in ((left, -1), (right, 1)):
        rod.circles += [(x + side * 1.5, y - 3.8, 4.5), (x + side * 1.5, y + 3.8, 4.5)]
    return rod.svg(outline)


def meat(stage: int, outline: float = 2.6) -> str:
    """The piece of meat after 'stage' bites (0 = whole, MEAT_STAGES - 1 = nearly a bare bone). Drawn behind the eating
    dog, it does not move with her. The bites are cut with masks, with the trick of the fur: an outline layer cut by
    bites shrunk by half the outline, then the flesh cut by the bites, then the roasted surface cut by enlarged bites,
    which leaves an outline and a band of juicy flesh along the bitten edges only."""
    mx, my = MEAT_CENTER
    rx, ry = MEAT_RADII
    half = outline / 2
    bites = [bite for previous in MEAT_BITES[:stage + 1] for bite in previous]

    cleared = MEAT_CLEARED[stage]

    def mask(name: str, grow: float) -> str:
        return (f'<mask id="{name}" maskUnits="userSpaceOnUse" x="-60" y="-60" width="120" height="120">'
                f'<rect x="-60" y="-60" width="120" height="120" fill="white"/>'
                f'<rect x="-60" y="-60" width="{max(0.0, cleared + grow + 60):.2f}" height="120" fill="black"/>'
                + "".join(circle(x, y, r + grow, "black") for x, y, r in bites) + "</mask>")

    defs = (f'<clipPath id="meat-surface"><ellipse cx="0" cy="0" rx="{rx - half:.2f}" ry="{ry - half:.2f}"/>'
            f'</clipPath>' + mask("bites-outline", -half) + mask("bites-flesh", half) +
            mask("bites-surface", half + MEAT_CUT))
    surface = [
        ellipse(0, 0, rx, ry, MEAT),
        # Roasted top, lighter, and a shine.
        ellipse(-2, -5, rx * 0.86, ry * 0.62, MEAT_LIGHT),
        ellipse(10, -7, 5.5, 2.6, "#FFFFFF", 'opacity="0.55" transform="rotate(-18 10 -7)"'),
    ]
    # Grill marks.
    for x in (-9.0, 1.0, 11.0):
        surface.append(f'<path d="M {x - 3:.1f} {-ry * 0.45 + 8:.1f} l 6 -10" stroke="{MEAT_DARK}" '
                       f'stroke-width="2.6" stroke-linecap="round" opacity="0.8"/>')
    parts = [
        ellipse(mx + 2, 110, 32, 4.0, SHADOW, 'opacity="0.16"'),
        f'<g transform="translate({mx:.1f} {my:.1f}) rotate({MEAT_TILT:.1f})"><defs>{defs}</defs>',
        bone(outline),
        f'<g mask="url(#bites-outline)">{ellipse(0, 0, rx + half, ry + half, OUTLINE)}</g>',
        f'<g mask="url(#bites-flesh)">{ellipse(0, 0, rx - half, ry - half, MEAT_INNER)}</g>',
        f'<g mask="url(#bites-surface)"><g clip-path="url(#meat-surface)">{"".join(surface)}</g></g>',
        "</g>",
    ]
    return "".join(parts)


# One pose per frame of the chomping cycle: the head comes down on the meat, tears a bite off, chews and licks her
# chops. Head center offset, tilt (degrees, nose down), jaw opening (degrees), eyes closed with pleasure, tail wag.
EAT_POSES = [
    dict(head=(0.0, -3.0), tilt=26.0, jaw=26.0, happy=False, wag=0.40, ear=0.30),
    dict(head=(3.0, 3.0), tilt=40.0, jaw=4.0, happy=True, wag=-0.30, ear=-0.35),
    dict(head=(-1.0, -4.0), tilt=24.0, jaw=12.0, happy=True, wag=0.40, ear=0.45),
    dict(head=(-2.0, -6.0), tilt=16.0, jaw=0.0, happy=True, wag=-0.30, ear=0.05),
]
# Bits of meat flying from the meat (frame coordinates), per frame.
EAT_CRUMBS = [
    [(150, 106, 1.6)],
    [(124, 74, 1.8), (138, 76, 2.0), (148, 84, 1.5)],
    [(128, 60, 2.0), (142, 64, 1.7), (152, 74, 1.5), (116, 64, 1.4)],
    [(136, 80, 1.8), (150, 88, 1.6), (148, 104, 1.5)],
]
# Zalie is a bit smaller than in the other animations, to leave room for the meat.
EAT_DOG_SCALE = 0.9


def eating_dog(frame: int, outline: float = 2.6) -> str:
    """One frame of Zalie eating as fast as she can, standing over the meat (drawn separately, see meat())."""
    pose = EAT_POSES[frame]
    parts = [ellipse(54, 111, 42, 4.5, SHADOW, 'opacity="0.16"')]
    body_cy = 69 + (1.0 if frame == 1 else 0.0)
    cx, rx = 50.0, 33.0
    front_x, back_x = cx + rx * 0.72, cx - rx * 0.72

    # Far legs, then the tail wagging fast, the body, the neck reaching down, the near legs.
    parts.append(leg((front_x - 4, body_cy + 8), 0.12, 0.05, 13, 12, 6.0, FUR_SHADE, outline))
    parts.append(leg((back_x + 4, body_cy + 8), -0.08, -0.02, 13, 13, 6.5, FUR_SHADE, outline))
    tail_pts = bezier((back_x + 4, body_cy - 12), (back_x - 22, body_cy - 30), (back_x + 4, body_cy - 42), 8)
    tail = Blob([(x, y, 8.5 - 2.0 * i / 8) for i, (x, y) in enumerate(tail_pts)])
    tail.circles += [(back_x + 9, body_cy - 42, 6.0)]
    tail.circles = rotate(tail.circles, back_x + 4, body_cy - 10, pose["wag"])
    parts.append(tail.svg(outline))
    parts.append(Blob(fluffy_ellipse(cx, body_cy, rx, 20, 9.0, 16, start=0.3, jitter=0.25)).svg(outline))

    hx, hy = 99.0 + pose["head"][0], 72.0 + pose["head"][1]
    neck = Blob(chain([(front_x - 2, body_cy - 6), (hx - 8, hy + 2)], 12.0, 11.0, 6))
    parts.append(neck.svg(outline))
    parts.append(leg((front_x, body_cy + 9), 0.18, 0.08, 13, 12, 6.5, FUR, outline))
    parts.append(leg((back_x, body_cy + 9), -0.10, -0.04, 13, 13, 7.0, FUR, outline))

    # The head, built facing right like the running one, then tilted down to the meat.
    head = []
    jaw = math.radians(pose["jaw"])
    hinge = (hx + 6, hy + 9)
    jaw_tip = rotate([(hx + 27, hy + 12)], *hinge, jaw)[0]
    if pose["jaw"] > 1.0:
        head.append(f'<path d="M {hinge[0]:.1f} {hinge[1]:.1f} L {hx + 31:.1f} {hy + 8:.1f} '
                    f'L {jaw_tip[0]:.1f} {jaw_tip[1]:.1f} Z" fill="{MOUTH}"/>')
        tongue_tip = rotate([(hx + 22, hy + 13)], *hinge, jaw * 0.6)[0]
        head.append(circle(*tongue_tip, 4.0, TONGUE))
    lower = Blob(chain([(hx + 8, hy + 12), (hx + 26, hy + 12)], 5.2, 4.2, 6))
    lower.circles = rotate(lower.circles, *hinge, jaw)
    head.append(lower.svg(outline))
    skull = Blob(fluffy_ellipse(hx, hy, 20, 18, 7.0, 12, start=0.2, jitter=0.2))
    skull.circles += [(hx + 16, hy + 6, 9.0), (hx + 22, hy + 6, 7.5),  # muzzle
                      (hx - 6, hy - 17, 6.5), (hx + 4, hy - 19, 6.0)]  # tousled fringe
    head.append(skull.svg(outline))
    ear = Blob(chain(bezier((hx - 7, hy - 9), (hx - 15, hy + 2), (hx - 11, hy + 15), 6), 6.2, 4.8, 6), FUR_SHADE)
    ear.circles = rotate(ear.circles, hx - 7, hy - 9, pose["ear"])
    head.append(ear.svg(outline))
    if pose["happy"]:
        head.append(f'<path d="M {hx + 5:.1f} {hy - 2:.1f} q 4 -5 8 0" fill="none" stroke="{EYE}" '
                    f'stroke-width="2.2" stroke-linecap="round"/>')
    else:
        head.append(circle(hx + 9, hy - 3, 3.8, EYE))
        head.append(circle(hx + 8, hy - 4.4, 1.3, "#FFFFFF"))
    head.append(ellipse(hx + 30.5, hy + 3.5, 4.2, 3.6, NOSE))
    if frame == 2:
        # A shred of meat torn off, hanging from the mouth.
        shred = Blob(chain([(hx + 24, hy + 12), (hx + 27, hy + 20)], 3.6, 3.0, 4), MEAT)
        head.append(shred.svg(outline * 0.8))
    if frame == 3:
        # Licking her chops.
        head.append(f'<path d="M {hx + 25:.1f} {hy + 9:.1f} q 6 -1 7 -6 q 2 4 -1 8 q -3 2 -6 -2 Z" fill="{TONGUE}" '
                    f'stroke="{OUTLINE}" stroke-width="{outline * 0.6:.2f}" stroke-linejoin="round"/>')
    parts.append(f'<g transform="rotate({pose["tilt"]:.1f} {hx:.1f} {hy:.1f})">{"".join(head)}</g>')

    # Speed marks over the head when she dives in, and the crumbs.
    if frame in (1, 2):
        for dx, dy, angle in ((-10, -27, -0.5), (2, -31, 0.0), (14, -28, 0.5)):
            x0, y0 = hx + dx, hy + dy
            parts.append(f'<path d="M {x0:.1f} {y0:.1f} l {5 * math.sin(angle):.1f} {-5 * math.cos(angle):.1f}" '
                         f'stroke="{OUTLINE}" stroke-width="1.8" stroke-linecap="round" opacity="0.55"/>')
    dog = (f'<g transform="translate(0 111) scale({EAT_DOG_SCALE}) translate(0 -111)">{"".join(parts)}</g>')
    crumbs = "".join(circle(x, y, r, MEAT, OUTLINE, outline * 0.45) for x, y, r in EAT_CRUMBS[frame])
    return dog + crumbs


# --------------------------------------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------------------------------------
def render(inkscape: str, svg: str, png: Path, width: int, height: int) -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as tmp:
        tmp.write(svg)
        svg_path = tmp.name
    try:
        subprocess.run([inkscape, svg_path, "--export-type=png", f"--export-filename={png}",
                        f"--export-width={width}", f"--export-height={height}", "--export-text-to-path"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    finally:
        Path(svg_path).unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "assets"))
    parser.add_argument("--inkscape", default="inkscape")
    parser.add_argument("--scale", type=float, default=2.0, help="sprite resolution factor (HiDPI)")
    args = parser.parse_args()
    if shutil.which(args.inkscape) is None:
        print(f"error: '{args.inkscape}' not found (Inkscape is needed to rasterize the SVG files)", file=sys.stderr)
        return 1

    out = Path(args.out)
    (out / "logo").mkdir(parents=True, exist_ok=True)
    (out / "sprites").mkdir(parents=True, exist_ok=True)

    # Logo: vector files for the docs / README, rasters for the window icon and the app.
    (out / "logo" / "zalie-logo.svg").write_text(logo_svg(512, badge=True))
    (out / "logo" / "zalie-face.svg").write_text(logo_svg(512, badge=False))
    for size in (16, 32, 48, 64, 128, 256, 512):
        render(args.inkscape, logo_svg(512, badge=True), out / "logo" / f"zalie-logo-{size}.png", size, size)
    render(args.inkscape, logo_svg(512, badge=False), out / "logo" / "zalie-face-256.png", 256, 256)

    # Sprite sheet: row 0 = gallop (8 frames), row 1 = sleeping (4 frames), row 2 = eating (4 frames of the dog, then
    # the 4 stages of the meat, drawn behind her).
    fw, fh = int(FRAME_W * args.scale), int(FRAME_H * args.scale)
    animations = [("run", 0, 0, [running_dog(i / RUN_FRAMES) for i in range(RUN_FRAMES)]),
                  ("sleep", 1, 0, [sleeping_dog(i / 4) for i in range(4)]),
                  ("eat", 2, 0, [eating_dog(i) for i in range(EAT_FRAMES)]),
                  ("meat", 2, EAT_FRAMES, [meat(i) for i in range(MEAT_STAGES)])]
    sheet = Image.new("RGBA", (fw * RUN_FRAMES, fh * (1 + max(row for _, row, _, _ in animations))), (0, 0, 0, 0))
    meta = {"frameWidth": fw, "frameHeight": fh, "animations": {}}
    with tempfile.TemporaryDirectory() as tmpdir:
        for name, row, first, frames in animations:
            meta["animations"][name] = {"row": row, "column": first, "frames": len(frames)}
            for i, content in enumerate(frames):
                png = Path(tmpdir) / f"{name}-{i}.png"
                render(args.inkscape, svg_document(FRAME_W, FRAME_H, content), png, fw, fh)
                with Image.open(png) as frame:
                    sheet.paste(frame.convert("RGBA"), ((first + i) * fw, row * fh))
        # Vector version of the first frame of each animation, for the docs.
        for name, first_frame in (("run", animations[0][3][0]), ("sleep", animations[1][3][0]),
                                  ("eat", meat(0) + eating_dog(0))):
            (out / "sprites" / f"zalie-{name}.svg").write_text(svg_document(FRAME_W, FRAME_H, first_frame))
    sheet.save(out / "sprites" / "zalie-sprites.png", optimize=True)
    (out / "sprites" / "zalie-sprites.json").write_text(json.dumps(meta, indent=2) + "\n")

    # Animated previews (docs), on the light theme background (GIF transparency is binary: ugly edges).
    background = (251, 246, 238, 255)

    def gif_frame(*cells: tuple[int, int]) -> Image.Image:
        frame = Image.new("RGBA", (fw, fh), background)
        for col, row in cells:
            frame.alpha_composite(sheet.crop((col * fw, row * fh, (col + 1) * fw, (row + 1) * fh)))
        return frame.convert("RGB")

    run_frames = [gif_frame((i, 0)).convert("P", palette=Image.Palette.ADAPTIVE, colors=64)
                  for i in range(RUN_FRAMES)]
    run_frames[0].save(out / "sprites" / "zalie-run.gif", save_all=True, append_images=run_frames[1:],
                       duration=80, loop=0)
    # The meal: two chomping cycles per stage of the meat. One palette for all the frames, so that the GIF only stores
    # what changes from a frame to the next.
    eat_frames = [gif_frame((EAT_FRAMES + stage, 2), (i % EAT_FRAMES, 2))
                  for stage in range(MEAT_STAGES) for i in range(2 * EAT_FRAMES)]
    strip = Image.new("RGB", (fw, fh * len(eat_frames)))
    for i, frame in enumerate(eat_frames):
        strip.paste(frame, (0, i * fh))
    strip = strip.quantize(colors=64)
    eat_frames = [strip.crop((0, i * fh, fw, (i + 1) * fh)) for i in range(len(eat_frames))]
    eat_frames[0].save(out / "sprites" / "zalie-eat.gif", save_all=True, append_images=eat_frames[1:],
                       duration=100, loop=0, optimize=True)
    print(f"assets written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
