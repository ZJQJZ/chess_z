#!/usr/bin/env python3
"""Package the offline viewer as a portable HTML file (standard library only)."""

import argparse
from pathlib import Path
import re


def package(destination):
    source = Path(__file__).resolve().parent
    html = (source / "index.html").read_text(encoding="utf-8")
    for name in ("model.js", "viewer.js"):
        script = (source / name).read_text(encoding="utf-8")
        # A literal closing tag in a JS string/comment must not close the HTML tag.
        script = re.sub(r"</script", lambda _: r"<\/script", script, flags=re.IGNORECASE)
        tag = '<script src="' + name + '"></script>'
        if html.count(tag) != 1:
            raise ValueError("Expected exactly one script tag for " + name)
        html = html.replace(tag, "<script>\n" + script + "\n</script>")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(html, encoding="utf-8")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?",
                        default=Path(__file__).resolve().parents[2] / "build/search_sample_viewer.html")
    print(package(parser.parse_args().output))
