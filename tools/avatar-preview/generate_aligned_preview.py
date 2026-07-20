#!/usr/bin/env python3
"""Create preview-only emotions aligned to the official default Face anchors."""

from __future__ import annotations

import base64
import html
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)
DRAWABLE_TAGS = {
    f"{{{SVG_NS}}}path",
    f"{{{SVG_NS}}}ellipse",
    f"{{{SVG_NS}}}circle",
    f"{{{SVG_NS}}}rect",
    f"{{{SVG_NS}}}polygon",
}

# Official default Face anchors converted from its 320x240 screen coordinates
# into the downloaded SVG's 2050-unit coordinate system.
LEFT_EYE = (576.56, 852.03)   # screen (90, 93)
RIGHT_EYE = (1473.44, 871.25) # screen (230, 96)
MOUTH = (1044.69, 1204.38)    # screen (163, 148)

# Each entry is (element indexes, original anchor, target anchor). Accessories
# are grouped with the feature they visually belong to.
LAYOUT = {
    "angry": [([1, 2, 3], (793, 956), LEFT_EYE), ([4, 5, 6], (1257, 956), RIGHT_EYE), ([0], (1025, 1250), MOUTH)],
    "awkward": [([0, 1, 4, 7], (793, 933), LEFT_EYE), ([2, 3, 5], (1257, 933), RIGHT_EYE), ([6], (1025, 1190), MOUTH)],
    "cool": [([0, 2], (791, 950), LEFT_EYE), ([1, 3], (1259, 950), RIGHT_EYE), ([4], (946, 1300), MOUTH)],
    "crying": [([1, 3], (826, 960), LEFT_EYE), ([0], (1224, 960), RIGHT_EYE), ([2], (1025, 1175), MOUTH)],
    "kiss": [([0, 1], (822, 930), LEFT_EYE), ([4], (1245, 920), RIGHT_EYE), ([2, 3, 5], (1030, 1205), MOUTH)],
    "omg": [([2], (826, 1130), LEFT_EYE), ([3], (1225, 1130), RIGHT_EYE), ([1], (1024, 1390), MOUTH)],
    "playful": [([3], (826, 840), LEFT_EYE), ([2], (1224, 840), RIGHT_EYE), ([0, 1], (1025, 1200), MOUTH)],
    "pout": [([0, 1, 4], (822, 930), LEFT_EYE), ([2, 3, 5], (1324, 930), RIGHT_EYE), ([6, 7], (1130, 1205), MOUTH)],
    "shocked": [([4], (771, 920), LEFT_EYE), ([0, 1, 2, 5], (1279, 920), RIGHT_EYE), ([3, 6], (1025, 1300), MOUTH)],
    "shy": [([0, 1, 4], (793, 950), LEFT_EYE), ([2, 3, 5], (1257, 950), RIGHT_EYE), ([6, 7], (1025, 1260), MOUTH)],
    "silent": [([0, 1], (793, 931), LEFT_EYE), ([2, 3], (1257, 931), RIGHT_EYE), ([4, 5, 6, 7, 8, 9, 10], (1025, 1250), MOUTH)],
    "sobbing": [([0, 1, 3], (791, 880), LEFT_EYE), ([4, 5, 6], (1259, 880), RIGHT_EYE), ([2], (1025, 1110), MOUTH)],
    "surprised": [([0, 1], (793, 931), LEFT_EYE), ([2, 3], (1257, 931), RIGHT_EYE), ([4], (1025, 1249), MOUTH)],
    "thinking": [([0, 1], (678, 1025), LEFT_EYE), ([2, 3], (1056, 1027), RIGHT_EYE), ([4], (850, 1285), MOUTH)],
    "whine": [([0], (820, 930), LEFT_EYE), ([1], (1230, 930), RIGHT_EYE), ([2, 3], (1014, 1296), MOUTH)],
    "wink": [([0, 1], (810, 931), LEFT_EYE), ([2], (1217, 900), RIGHT_EYE), ([3], (1040, 1220), MOUTH)],
    "worried": [([0, 1], (793, 951), LEFT_EYE), ([2, 3], (1257, 951), RIGHT_EYE), ([4, 5], (995, 1280), MOUTH)],
}


def translate(element: ET.Element, dx: float, dy: float) -> None:
    old = element.get("transform", "")
    element.set("transform", f"translate({dx:.2f} {dy:.2f}) {old}".strip())


def align_svg(source: Path, destination: Path) -> None:
    tree = ET.parse(source)
    root = tree.getroot()
    elements = [element for element in root.iter() if element.tag in DRAWABLE_TAGS]

    for indexes, source_anchor, target_anchor in LAYOUT[source.stem]:
        dx = target_anchor[0] - source_anchor[0]
        dy = target_anchor[1] - source_anchor[1]
        for index in indexes:
            translate(elements[index], dx, dy)

    # Keep the thought cloud above the standardized right eye instead of
    # covering it; its tail still points toward the face.
    if source.stem == "thinking":
        for index in (5, 6, 7, 8):
            translate(elements[index], 0, -100)

    # The sunglasses bridge must span the newly standardized eye positions.
    if source.stem == "cool":
        bridge = elements[5]
        old = bridge.get("transform", "")
        bridge.set(
            "transform",
            f"translate(1025 862) scale(1.85 1) translate(-1025 -835) {old}".strip(),
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)


def write_gallery(png_dir: Path, destination: Path) -> None:
    names = list(LAYOUT)
    card_w, card_h = 240, 220
    cols = 4
    rows = (len(names) + cols - 1) // cols
    canvas = max(cols * card_w, rows * card_h)
    parts = [
        f'<svg xmlns="{SVG_NS}" width="{canvas}" height="{canvas}" '
        f'viewBox="0 0 {canvas} {canvas}">',
        '<rect width="100%" height="100%" fill="#e9edf2"/>',
    ]
    for index, name in enumerate(names):
        x = (index % cols) * card_w
        y = (index // cols) * card_h
        encoded = base64.b64encode((png_dir / f"{name}.png").read_bytes()).decode()
        label = "OMG" if name == "omg" else name
        parts.extend(
            [
                f'<rect x="{x + 8}" y="{y + 8}" width="224" height="204" rx="12" fill="white"/>',
                f'<image x="{x + 8}" y="{y + 8}" width="224" height="168" '
                f'href="data:image/png;base64,{encoded}" preserveAspectRatio="xMidYMin slice"/>',
                f'<text x="{x + 120}" y="{y + 196}" text-anchor="middle" '
                f'font-family="Arial,sans-serif" font-size="18" fill="#00305f">{html.escape(label)}</text>',
            ]
        )
    parts.append("</svg>")
    destination.write_text("\n".join(parts), encoding="utf-8")


def main() -> None:
    project = Path(__file__).resolve().parent
    source_dir = project / "assets" / "emotions"
    output_dir = project / "aligned-preview"
    if len(sys.argv) > 1 and sys.argv[1] == "gallery":
        write_gallery(output_dir / "png", output_dir / "gallery.svg")
        return
    svg_dir = output_dir / "svg"
    for source in sorted(source_dir.glob("*.svg")):
        align_svg(source, svg_dir / source.name)


if __name__ == "__main__":
    main()
