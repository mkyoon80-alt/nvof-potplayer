"""Stage the offline manual for GitHub Pages, keeping every local link valid."""
from html.parser import HTMLParser
from pathlib import Path
import argparse
import shutil
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]

class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.urls = []
        self.ids = set()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.add(attrs["id"])
        for key in ("src", "href"):
            if key in attrs:
                self.urls.append(attrs[key])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "manual-site")
    args = parser.parse_args()
    output = args.output.resolve()
    # Always stage in a fresh directory; never mix arbitrary files into a deployment.
    if output.exists():
        raise SystemExit(f"Output already exists: {output}")
    output.mkdir(parents=True)
    shutil.copytree(ROOT / "docs" / "manual", output, dirs_exist_ok=True)
    shutil.copytree(ROOT / "licenses", output / "licenses")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES.md", output)
    shutil.copy2(ROOT / "docs" / "HDR-NATIVE-TRIAL.md", output)
    html_path = output / "index.html"
    html = html_path.read_text(encoding="utf-8")
    html = html.replace('href="../../THIRD_PARTY_NOTICES.md"', 'href="THIRD_PARTY_NOTICES.md"')
    html = html.replace('href="../../licenses/', 'href="licenses/')
    html = html.replace('href="../HDR-NATIVE-TRIAL.md"', 'href="HDR-NATIVE-TRIAL.md"')
    html_path.write_text(html, encoding="utf-8")
    (output / ".nojekyll").touch()
    links = Links()
    links.feed(html)
    for url in links.urls:
        part = urlsplit(url)
        if part.scheme or part.netloc:
            continue
        if not part.path:
            if part.fragment and part.fragment not in links.ids:
                raise SystemExit(f"Missing anchor: {url}")
            continue
        target = (output / unquote(part.path)).resolve()
        if not target.is_relative_to(output) or not target.is_file():
            raise SystemExit(f"Missing or invalid local resource: {url}")
    print(f"Staged manual and verified {len(links.urls)} links/resources: {output}")


if __name__ == "__main__":
    main()
