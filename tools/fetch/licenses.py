"""
licenses.py: writes LICENSES.txt for the built fetch_pb, the licence of
everything PyInstaller puts inside it: Python, the packages requirements.txt
names and the ones they need, and PyInstaller's bootloader. Run it in the
environment the program is built from:

    python licenses.py LICENSES.txt
"""

import re
import sys
from importlib import metadata
from pathlib import Path

HERE = Path(__file__).resolve().parent
LICENCE_NAME = re.compile(r"(LICEN[CS]E|COPYING|NOTICE)", re.I)


def requirement_names(lines):
    """The package names in requirement lines, leaving out extras'."""
    for line in lines:
        line = line.split("#")[0].strip()
        if line and "extra" not in line.partition(";")[2]:
            yield re.match(r"[A-Za-z0-9._-]+", line).group(0)


def bundled():
    """requirements.txt's packages and everything they need, installed."""
    seen, todo = {}, list(requirement_names((HERE / "requirements.txt").read_text().splitlines()))
    while todo:
        name = todo.pop()
        key = name.lower().replace("_", "-")
        if key in seen:
            continue
        try:
            dist = metadata.distribution(name)
        except metadata.PackageNotFoundError:
            continue                      # a dependency for another platform
        seen[key] = dist
        todo.extend(requirement_names(dist.requires or []))
    return sorted(seen.values(), key=lambda d: d.metadata["Name"].lower())


def licence_texts(dist):
    files = [f for f in dist.files or [] if LICENCE_NAME.search(f.name)]
    if not files:
        sys.exit(f"licenses.py: {dist.metadata['Name']} has no licence file")
    return [Path(f.locate()).read_text(encoding="utf-8", errors="replace") for f in sorted(files, key=str)]


def python_licence():
    for place in (Path(sys.base_prefix), Path(sys.base_prefix) / "share/doc/python3",
                  Path(re.__file__).parent.parent):
        if (place / "LICENSE.txt").is_file():
            return (place / "LICENSE.txt").read_text(encoding="utf-8", errors="replace")
    sys.exit("licenses.py: Python's LICENSE.txt not found")


def section(title, texts):
    return f"{'=' * 72}\n{title}\n{'=' * 72}\n\n" + "\n\n".join(t.strip() for t in texts) + "\n\n"


def main():
    out = ["fetch_pb is licensed under the GNU General Public License v2.0, as PodBox is.\n"
           "It contains the following, under their own licences.\n\n"]
    out.append(section(f"Python {sys.version.split()[0]}", [python_licence()]))
    for dist in bundled() + [metadata.distribution("pyinstaller")]:
        title = f"{dist.metadata['Name']} {dist.version}"
        if dist.metadata["Name"].lower() == "pyinstaller":
            title += " (its bootloader, which starts the program)"
        out.append(section(title, licence_texts(dist)))
    Path(sys.argv[1]).write_text("".join(out), encoding="utf-8")


if __name__ == "__main__":
    main()
