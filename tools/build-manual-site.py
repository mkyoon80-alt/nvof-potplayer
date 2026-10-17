"""Stage the offline manual for GitHub Pages, keeping every local link valid."""
from html.parser import HTMLParser
from pathlib import Path
import argparse
import shutil
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
NOTES = ("HDR-NATIVE-TRIAL.md", "CPU-10BIT.md", "SW-DECODING.md", "VP9-HDR.md", "DOLBY-VISION-HLG.md", "PLAYBACK-PERFORMANCE.md", "SOFTWARE-HDR-TRIAL.md", "THIN-OVERLAY-TRIAL.md", "ADAPTER-SWITCH-TRIAL.md", "SEEK-PREVIEW.ko.md", "TENBIT-SCAN-TRIAL.md")

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
    for note in NOTES:
        shutil.copy2(ROOT / "docs" / note, output)
    count = 0
    for html_path in output.glob("*.html"):
        html = html_path.read_text(encoding="utf-8-sig")
        html = html.replace('href="../../THIRD_PARTY_NOTICES.md"', 'href="THIRD_PARTY_NOTICES.md"')
        html = html.replace('href="../../licenses/', 'href="licenses/')
        for note in NOTES:
            html = html.replace(f'href="../{note}"', f'href="{note}"')
        html_path.write_text(html, encoding="utf-8")
        links = Links()
        links.feed(html)
        for url in links.urls:
            part = urlsplit(url)
            if part.scheme or part.netloc:
                continue
            if not part.path:
                if part.fragment and part.fragment not in links.ids:
                    raise SystemExit(f"Missing anchor in {html_path.name}: {url}")
                continue
            target = (output / unquote(part.path)).resolve()
            if not target.is_relative_to(output) or not target.is_file():
                raise SystemExit(f"Missing or invalid local resource: {url}")
        count += len(links.urls)
    (output / ".nojekyll").touch()
    print(f"Staged bilingual manual and verified {count} links/resources: {output}")



if __name__ == "__main__":
    main()
