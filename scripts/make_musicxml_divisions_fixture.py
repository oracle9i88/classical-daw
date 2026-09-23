#!/usr/bin/env python3
"""Create equivalent mixed-division test files from our canonical 960-unit XML.

No source is modified. Only duration/offset units and leading divisions change.
This is a test-fixture generator, not a general MusicXML converter.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET


def generate(source, destination):
    raw = source.read_bytes()
    if any(marker in raw for marker in (b"<!DOCTYPE", b"<!ENTITY", b"<!--", b"<![CDATA[")):
        raise ValueError("expected canonical XML without declarations/comments/entities")
    root = ET.fromstring(raw)
    if root.tag != "score-partwise":
        raise ValueError("expected unnamespaced score-partwise")
    counts, units = 0, set()
    for part_index, part in enumerate(root.findall("part")):
        active = None
        for measure_index, measure in enumerate(part.findall("measure")):
            for declaration in measure.findall("./attributes/divisions"):
                active = int(declaration.text)
            if active != 960:
                raise ValueError("source must declare and retain 960 divisions in each part")
            nodes = list(measure.iter("duration")) + list(measure.iter("offset"))
            amounts = [int(node.text) for node in nodes]
            # Lowest exact integral resolution in alternate measures, a large
            # integral multiple elsewhere. Vary independently in every part.
            common = math.gcd(960, *amounts)
            divisions = 960 // common if (part_index + measure_index) % 2 == 0 else 20160
            units.add(divisions)
            for node, amount in zip(nodes, amounts):
                scaled, remainder = divmod(amount * divisions, 960)
                if remainder:
                    raise AssertionError("fixture generator would alter a musical position")
                node.text = str(scaled)
            attributes = measure.find("attributes")
            if attributes is None:
                attributes = ET.Element("attributes")
                measure.insert(0, attributes)
            declaration = attributes.find("divisions")
            if declaration is None:
                declaration = ET.Element("divisions")
                attributes.insert(0, declaration)
            declaration.text = str(divisions)
            counts += 1
    encoded = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    with destination.open("xb") as output:
        output.write(encoded)
    return {"source": str(source), "source_sha256": hashlib.sha256(raw).hexdigest(),
            "output": str(destination), "output_sha256": hashlib.sha256(encoded).hexdigest(),
            "measures": counts, "divisions": sorted(units)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("sources", type=Path, nargs="+")
    args = parser.parse_args()
    args.output_directory.mkdir()  # Refuse overwrite/reuse.
    for index, source in enumerate(args.sources, 1):
        result = generate(source, args.output_directory / f"mixed-{index}.musicxml")
        print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
