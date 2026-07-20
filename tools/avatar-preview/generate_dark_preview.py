#!/usr/bin/env python3
"""Create a preview-only dark StackChan theme from the approved aligned SVGs."""

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


def remove_element(root: ET.Element, target: ET.Element) -> None:
    for parent in root.iter():
        if target in list(parent):
            parent.remove(target)
            return
    raise ValueError("SVG element has no parent")


def darken_svg(source: Path, destination: Path, firmware_base: bool = False) -> None:
    tree = ET.parse(source)
    root = tree.getroot()
    elements = [element for element in root.iter() if element.tag in DRAWABLE_TAGS]

    # Invert only the expression ink and its tiny eye highlights. Tears,
    # hearts, tongues, halo, sweat, and thought cloud keep their accent colors.
    for style in root.findall(f".//{{{SVG_NS}}}style"):
        text = style.text or ""
        text = text.replace("#00305f", "__PRIMARY_INK__")
        text = text.replace("#ffffff", "#000000")
        style.text = text.replace("__PRIMARY_INK__", "#ffffff")

    # Put the thought cloud fully outside the right eye. The cloud is reduced
    # slightly so it fits between the eye and the screen edge; its tail remains
    # directed back toward the face.
    if source.stem == "thinking":
        cx, cy = 1394, 590
        dx, dy = 412, 115
        for index in (5, 6, 7, 8):
            elements[index].set(
                "transform",
                f"translate({dx} {dy}) translate({cx} {cy}) "
                f"scale(.65) translate(-{cx} -{cy})",
            )

    # Firmware draws the kiss heart independently so it can float. Wink keeps
    # its original curved closed eye in the base artwork; runtime only covers
    # it while drawing the temporary surprised/open eye.
    if firmware_base and source.stem == "kiss":
        remove_element(root, elements[5])

    background = ET.Element(
        f"{{{SVG_NS}}}rect",
        {"x": "0", "y": "256.25", "width": "2050", "height": "1537.5", "fill": "#000000"},
    )
    root.insert(0, background)
    destination.parent.mkdir(parents=True, exist_ok=True)
    tree.write(destination, encoding="utf-8", xml_declaration=True)


def write_gallery(png_dir: Path, destination: Path) -> None:
    names = sorted(path.stem for path in png_dir.glob("*.png"))
    card_w, card_h, cols = 240, 220, 4
    rows = (len(names) + cols - 1) // cols
    canvas = max(cols * card_w, rows * card_h)
    parts = [
        f'<svg xmlns="{SVG_NS}" width="{canvas}" height="{canvas}" '
        f'viewBox="0 0 {canvas} {canvas}">',
        '<rect width="100%" height="100%" fill="#17191d"/>',
    ]
    for index, name in enumerate(names):
        x = (index % cols) * card_w
        y = (index // cols) * card_h
        encoded = base64.b64encode((png_dir / f"{name}.png").read_bytes()).decode()
        label = "OMG" if name == "omg" else name
        parts.extend(
            [
                f'<rect x="{x + 8}" y="{y + 8}" width="224" height="204" rx="12" '
                f'fill="#000000" stroke="#343a43"/>',
                f'<image x="{x + 8}" y="{y + 8}" width="224" height="168" '
                f'href="data:image/png;base64,{encoded}" preserveAspectRatio="xMidYMin slice"/>',
                f'<text x="{x + 120}" y="{y + 196}" text-anchor="middle" '
                f'font-family="Arial,sans-serif" font-size="18" fill="#ffffff">'
                f'{html.escape(label)}</text>',
            ]
        )
    parts.append("</svg>")
    destination.write_text("\n".join(parts), encoding="utf-8")


def main() -> None:
    project = Path(__file__).resolve().parent
    output = project / "dark-preview"
    if len(sys.argv) > 1 and sys.argv[1] == "gallery":
        write_gallery(output / "png", output / "gallery.svg")
        return

    source_dir = project / "aligned-preview" / "svg"
    if len(sys.argv) > 1 and sys.argv[1] == "firmware":
        for source in sorted(source_dir.glob("*.svg")):
            darken_svg(source, output / "firmware-svg" / source.name, firmware_base=True)
        return

    for source in sorted(source_dir.glob("*.svg")):
        darken_svg(source, output / "svg" / source.name)


if __name__ == "__main__":
    main()
