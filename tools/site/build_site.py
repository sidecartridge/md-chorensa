#!/usr/bin/env python3
"""The project's website, built from site/, its pictures and the web converter.

Usage: build_site.py [--out DIR] [--converter DIR] [--converter-only] [--check]

Copies site/ (the pages, their style, scripts and fonts) into tools/dev/builds/site/
(git-ignored), or DIR, with the pictures of docs/images/ and the app's icon in images/, and the
converter's files beside the first page: from --converter (web/build.sh's web/dist/site, the
default) its script and worker, the WebAssembly and the release's image pack, when there (the
script and worker from web/site otherwise). Nothing is fetched: open index.html from a local
server.

--converter-only: one release's converter alone (what chorensha.sidecartridge.com/<tag>/
serves): web/site/index.html as the page, with the site's style, fonts, cookie banner, logo and
icon and the converter's files; its links to the article (../) leave the folder, and the check
lets them.

--check, after the build: every local link, image, script and style resolves, every #anchor
names an id of the page, the page loads nothing from another site (it links to other sites, never
loads from them), and no file of the site names a planning item or the planning notes' folder.
Standard library only.
"""

import argparse
import html.parser
import re
import shutil
import sys
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SITE = REPO / "site"
IMAGES = REPO / "docs" / "images"
CONVERTER = REPO / "web" / "dist" / "site"
OUT = REPO / "tools" / "dev" / "builds" / "site"
# The converter's own files: the page's script and its worker, from web/site; the gate in
# WebAssembly and the pack, from web/build.sh's output.
SCRIPTS = ("app.js", "worker.js")
BUILT = ("crinstall.mjs", "crinstall.wasm", "pack")
# A planning item's identifier, or the planning notes' folder (spelt in two pieces, so that this
# file passes the repository's own check for it).
PLANNING = re.compile(r"\b(epic|story|iteration)[ -]?[0-9]|docs/" + "epics", re.IGNORECASE)
# Attributes that make the browser load something, against those that only link to it.
LOADS = {("img", "src"), ("script", "src"), ("link", "href"), ("source", "src"),
         ("video", "src"), ("audio", "src"), ("iframe", "src")}
# A <link> that only names a page: the page's own address.
NAMES = {"canonical", "alternate"}


def build(out: Path, converter: Path, alone: bool) -> None:
    if out.exists():
        shutil.rmtree(out)
    if alone:
        out.mkdir(parents=True)
        for name in ("site.css", "consent.js", "fonts", "vendor"):
            src = SITE / name
            (shutil.copytree if src.is_dir() else shutil.copy2)(src, out / name)
        shutil.copy2(REPO / "web" / "site" / "index.html", out / "index.html")
        (out / "images").mkdir()
        shutil.copy2(IMAGES / "logo.png", out / "images" / "logo.png")
    else:
        shutil.copytree(SITE, out)
        (out / "images").mkdir()
        for f in sorted(IMAGES.iterdir()):
            if f.suffix in (".png", ".gif", ".jpg", ".mp4"):
                shutil.copy2(f, out / "images" / f.name)
    shutil.copy2(REPO / "icon.png", out / "images" / "icon.png")
    for name in SCRIPTS:
        src = converter / name
        shutil.copy2(src if src.exists() else REPO / "web" / "site" / name, out / name)
    for name in BUILT:
        src = converter / name
        if src.is_dir():
            shutil.copytree(src, out / name)
        elif src.exists():
            shutil.copy2(src, out / name)
        else:
            print(f"site: no {name} in {converter} (web/build.sh makes it)", file=sys.stderr)
    (out / ".nojekyll").touch()  # served as it is, without Jekyll


class Page(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: set[str] = set()
        self.refs: list[tuple[str, str, str]] = []  # (tag, attribute, value)

    def handle_starttag(self, tag, attrs):
        if tag == "link" and dict(attrs).get("rel") in NAMES:
            tag = "a"  # names an address, loads nothing
        for name, value in attrs:
            if value is None:
                continue
            if name == "id":
                self.ids.add(value)
            elif name in ("href", "src"):
                self.refs.append((tag, name, value))


def check(out: Path, alone: bool) -> list[str]:
    errors = []
    for f in sorted(out.rglob("*")):
        if f.suffix in (".html", ".css", ".js") and PLANNING.search(f.read_text()):
            errors.append(f"{f.relative_to(out)}: names the planning notes")
    pages = {f: Page() for f in sorted(out.rglob("*.html"))}
    for f, page in pages.items():
        page.feed(f.read_text())
    for f, page in pages.items():
        rel = f.relative_to(out)
        for tag, attr, value in page.refs:
            url = urllib.parse.urlparse(value)
            if url.scheme or value.startswith("//"):
                if (tag, attr) in LOADS:
                    errors.append(f"{rel}: <{tag} {attr}> loads {value} from another site")
                continue
            target = (f.parent / url.path) if url.path else f
            if alone and not target.resolve().is_relative_to(out):
                continue  # the article's, beside the release's folder once published
            if url.path and not target.exists():
                errors.append(f"{rel}: {value} does not exist")
            elif url.fragment and target.suffix == ".html":
                ids = pages[target.resolve()].ids if target.resolve() in pages else set()
                if url.fragment not in ids:
                    errors.append(f"{rel}: {value} names no id")
    for f in sorted(out.rglob("*.css")):
        for value in re.findall(r"url\(\s*['\"]?([^'\")]+)", f.read_text()):
            if value.startswith("data:"):
                continue
            if urllib.parse.urlparse(value).scheme:
                errors.append(f"{f.relative_to(out)}: loads {value} from another site")
            elif not (f.parent / value).exists():
                errors.append(f"{f.relative_to(out)}: {value} does not exist")
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--converter", type=Path, default=CONVERTER)
    ap.add_argument("--converter-only", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    out = args.out.resolve()
    build(out, args.converter.resolve(), args.converter_only)
    print(f"site: {out}")
    if args.check:
        errors = check(out, args.converter_only)
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        if errors:
            return 1
        print("site: checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
