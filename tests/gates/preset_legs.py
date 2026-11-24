#!/usr/bin/env python3
"""List the ctest legs of a CMakePresets.json.

Usage: python3 -I tests/gates/preset_legs.py <CMakePresets.json>

A ctest leg is a non-hidden configure preset that scripts/check.sh runs. A preset marked
`"vendor": {"lws": {"hand-run": true}}` is configured and built by hand and never runs as a leg, so it
is not listed. The names are printed one per line, in file order.

Exit 0 with the names; exit 2 with a message on stderr when the file cannot be read, is not JSON, has no
`configurePresets` array, a preset has no name, or a `hand-run` value is present but is not a JSON
boolean. The last case fails closed: a typo in the marker must not silently drop a leg.
"""

import json
import sys


def fail(message):
    print(f"preset_legs: {message}", file=sys.stderr)
    sys.exit(2)


def hand_run(preset, name):
    vendor = preset.get("vendor", {})
    lws = vendor.get("lws", {}) if isinstance(vendor, dict) else {}
    if not isinstance(lws, dict) or "hand-run" not in lws:
        return False
    value = lws["hand-run"]
    if not isinstance(value, bool):
        fail(f"preset {name!r}: hand-run must be a JSON boolean, got {value!r}")
    return value


def main(argv):
    if len(argv) != 2:
        fail("usage: preset_legs.py <CMakePresets.json>")
    try:
        with open(argv[1], encoding="utf-8") as handle:
            document = json.load(handle)
    except (OSError, ValueError) as error:
        fail(f"cannot read {argv[1]}: {error}")
    presets = document.get("configurePresets") if isinstance(document, dict) else None
    if not isinstance(presets, list):
        fail(f"{argv[1]} has no configurePresets array")
    for preset in presets:
        name = preset.get("name") if isinstance(preset, dict) else None
        if not isinstance(name, str) or not name:
            fail("a configure preset has no name")
        if preset.get("hidden", False) or hand_run(preset, name):
            continue
        print(name)


main(sys.argv)
