#!/usr/bin/env python3
"""Read-only MusicXML validation against a local XSD, using optional lxml.

Usage:
  python3 scripts/check_musicxml_schema.py --schema PATH/musicxml.xsd FILES...

Place the official MusicXML 4.0 musicxml.xsd, xml.xsd, and xlink.xsd in one
directory first. This script never downloads schemas. Known MusicXML imports
are resolved from those local files; all other schema resolution is blocked.
Network access, DTD loading, and entity expansion are disabled.

Each source produces one JSON result with pass/fail and SHA-256 hashes of the
source, main schema, and schema dependencies. Hashes describe the exact byte
snapshots parsed, including locally resolved dependencies. Exit status is 0
for all valid files, 1 for any invalid/unreadable source, and 2 for setup errors.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parser_for(etree):
    return etree.XMLParser(
        no_network=True,
        resolve_entities=False,
        load_dtd=False,
        dtd_validation=False,
        attribute_defaults=False,
        huge_tree=False,
        recover=False,
    )


def build_schema(etree, schema_path):
    schema_path = schema_path.resolve(strict=True)
    schema_bytes = schema_path.read_bytes()
    dependencies = {}
    locations = {}
    for name in ("xml.xsd", "xlink.xsd"):
        path = (schema_path.parent / name).resolve(strict=True)
        data = path.read_bytes()
        dependencies[name] = digest(data)
        # Support the locations in the official 4.0 schema, plus equivalent
        # local references when an XML catalog has already rewritten them.
        for location in (
            f"http://www.musicxml.org/xsd/{name}",
            f"https://www.musicxml.org/xsd/{name}",
            name,
            str(path),
            path.as_uri(),
        ):
            locations[location] = (data, path.as_uri())

    class LocalSchemaResolver(etree.Resolver):
        def resolve(self, url, public_id, context):
            snapshot = locations.get(url)
            if snapshot is None:
                raise OSError(f"Schema resolution blocked for unsupported location: {url}")
            data, base_url = snapshot
            return self.resolve_string(data, context, base_url=base_url)

    parser = parser_for(etree)
    parser.resolvers.add(LocalSchemaResolver())
    root = etree.fromstring(schema_bytes, parser=parser, base_url=schema_path.as_uri())
    schema = etree.XMLSchema(root.getroottree())
    metadata = {
        "schema": str(schema_path),
        "schema_sha256": digest(schema_bytes),
        "schema_dependencies_sha256": dependencies,
    }
    return schema, metadata


def error_details(log):
    # A malformed document can produce many repeated diagnostics. Keep the
    # terminal readable, while retaining the total count in the JSON result.
    entries = list(log)
    return {
        "error_count": len(entries),
        "errors": [
            {"line": entry.line, "column": entry.column, "message": entry.message}
            for entry in entries[:20]
        ],
    }


def validate_file(etree, schema, metadata, source):
    result = {"source": str(source), "source_sha256": None, "pass": False, **metadata}
    try:
        path = source.resolve(strict=True)
        result["source"] = str(path)
        data = path.read_bytes()
        result["source_sha256"] = digest(data)
    except OSError as error:
        result.update(stage="read", error=str(error))
        return result

    parser = parser_for(etree)
    try:
        root = etree.fromstring(data, parser=parser, base_url=path.as_uri())
    except etree.XMLSyntaxError as error:
        result.update(stage="parse", error=str(error), **error_details(parser.error_log))
        return result
    except (OSError, ValueError) as error:
        result.update(stage="parse", error=str(error))
        return result

    try:
        schema.assertValid(root.getroottree())
    except etree.DocumentInvalid as error:
        result.update(stage="validate", error=str(error), **error_details(schema.error_log))
        return result
    except (etree.XMLSchemaValidateError, OSError, ValueError) as error:
        result.update(stage="validate", error=str(error))
        return result

    result["pass"] = True
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, required=True, help="Local musicxml.xsd path")
    parser.add_argument("files", type=Path, nargs="+", help="MusicXML files to validate without modifying")
    args = parser.parse_args()

    try:
        from lxml import etree
    except ImportError:
        print("MusicXML validation requires optional developer dependency lxml; "
              "run this script in a Python environment containing lxml.", file=sys.stderr)
        return 2

    try:
        schema, metadata = build_schema(etree, args.schema)
    except (OSError, ValueError, etree.XMLSyntaxError, etree.XMLSchemaParseError) as error:
        print(f"Local schema setup failed: {error}", file=sys.stderr)
        return 2

    failed = False
    for source in args.files:
        result = validate_file(etree, schema, metadata, source)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True))
        failed = failed or not result["pass"]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
