#!/usr/bin/env python3
"""Re-add the build settings projectGenerator discards.

openFrameworks' projectGenerator rewrites project.pbxproj on every update and
writes HEADER_SEARCH_PATHS, LIBRARY_SEARCH_PATHS and OTHER_LDFLAGS at the target
level, where they override anything in Project.xcconfig. This puts the Rubber
Band entries back.

Run from the project folder, after any projectGenerator update:

    python3 tools/fix-xcode-settings.py

Safe to run repeatedly: entries already present are left alone.
"""

import re
import shutil
import sys
from pathlib import Path

PROJECT = Path("vocalyx.xcodeproj/project.pbxproj")

# setting name -> entry that must appear in its list
REQUIRED = {
    "HEADER_SEARCH_PATHS": '"/opt/homebrew/include"',
    "LIBRARY_SEARCH_PATHS": '"/opt/homebrew/lib"',
    "OTHER_LDFLAGS": '"-lrubberband"',
}


def patch(text, setting, entry):
    """Add entry to setting's list, or create the setting if it is absent."""
    added = 0

    # Case 1: the setting exists as a parenthesised list.
    pattern = re.compile(
        r"(\b" + setting + r"\s*=\s*\()(.*?)(\n\s*\);)",
        re.DOTALL,
    )

    def insert(match):
        nonlocal added
        head, body, tail = match.groups()

        if entry in body:
            return match.group(0)

        added += 1
        return head + "\n\t\t\t\t\t" + entry + "," + body + tail

    text, list_count = pattern.subn(insert, text)

    if list_count:
        return text, added

    # Case 2: the setting is absent. Add it to every buildSettings block.
    def create(match):
        nonlocal added
        added += 1
        return match.group(0) + "\n\t\t\t\t" + setting + " = (\n\t\t\t\t\t" + entry + ",\n\t\t\t\t);"

    text = re.sub(r"buildSettings = \{", create, text)
    return text, added


def main():
    if not PROJECT.exists():
        sys.exit(f"{PROJECT} not found — run this from the project folder")

    original = PROJECT.read_text()
    text = original
    total = 0

    for setting, entry in REQUIRED.items():
        text, added = patch(text, setting, entry)
        total += added
        print(f"{setting}: {'added ' + str(added) if added else 'already present'}")

    if total == 0:
        print("nothing to change")
        return

    shutil.copy(PROJECT, PROJECT.with_suffix(".pbxproj.backup"))
    PROJECT.write_text(text)
    print(f"patched {total} entries (backup at {PROJECT.name}.backup)")


if __name__ == "__main__":
    main()
