"""Build a self-contained HTML guide and an offline ZIP from a staged manual site."""
import argparse
import base64
import mimetypes
from pathlib import Path
import re
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site-dir", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--language", choices=("ko", "en"), default="ko")
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+[-.a-zA-Z0-9]*", args.version):
        raise SystemExit("Invalid version")
    site = args.site_dir.resolve()
    entry = "en.html" if args.language == "en" else "index.html"
    html = (site / entry).read_text(encoding="utf-8-sig")
    if args.version not in html:
        raise SystemExit("Manual version does not match the requested release")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"NvofPotPlayer-{args.version}-Manual-{args.language}"
    standalone, archive = output / (name + ".html"), output / (name + ".zip")
    if standalone.exists() or archive.exists():
        raise SystemExit("Manual output already exists; existing assets are preserved")
    html = html.replace('<link rel="stylesheet" href="manual.css">',
                        '<style>' + (site / "manual.css").read_text(encoding="utf-8") + '</style>')
    script = (site / "manual.js").read_text(encoding="utf-8")
    html = html.replace('<script src="manual.js" defer></script>', '')
    html = html.replace('</body>', '<script>' + script + '</script></body>')
    def inline_image(match):
        image = (site / match.group(2)).resolve()
        if not image.is_relative_to(site) or not image.is_file():
            raise SystemExit("Image is outside the manual site or missing")
        mime = mimetypes.guess_type(image.name)[0]
        data = base64.b64encode(image.read_bytes()).decode("ascii")
        return f'{match.group(1)}="data:{mime};base64,{data}"'
    html = re.sub(r'(src|href)="(images/[^"]+)"', inline_image, html)
    # The guide itself is fully offline; supplementary vendor documents remain
    # linked to the same release source. The ZIP also carries those documents.
    source = f"https://github.com/mkyoon80-alt/nvof-potplayer/blob/v{args.version}/"
    html = html.replace('href="THIRD_PARTY_NOTICES.md"', f'href="{source}THIRD_PARTY_NOTICES.md"')
    html = html.replace('href="licenses/', f'href="{source}licenses/')
    for note in ("HDR-NATIVE-TRIAL.md", "CPU-10BIT.md", "VP9-HDR.md", "DOLBY-VISION-HLG.md"):
        html = html.replace(f'href="{note}"', f'href="{source}docs/{note}"')
    html = html.replace('href="en.html"', 'href="https://mkyoon80-alt.github.io/nvof-potplayer/en.html"')
    html = html.replace('href="index.html"', 'href="https://mkyoon80-alt.github.io/nvof-potplayer/"')
    standalone.write_text(html, encoding="utf-8")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for file in sorted(site.rglob("*")):
            if file.is_file() and file.name != ".nojekyll":
                relative = file.relative_to(site).as_posix()
                if args.language == "en" and relative in ("index.html", "en.html"):
                    page = file.read_text(encoding="utf-8-sig")
                    if relative == "en.html":
                        relative = "index.html"
                        page = page.replace('href="index.html"', 'href="ko.html"')
                    else:
                        relative = "ko.html"
                        page = page.replace('href="en.html"', 'href="index.html"')
                    bundle.writestr(relative, page)
                else:
                    bundle.write(file, relative)
    print(standalone)
    print(archive)


if __name__ == "__main__":
    main()
