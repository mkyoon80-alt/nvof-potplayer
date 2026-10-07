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
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+[-.a-zA-Z0-9]*", args.version):
        raise SystemExit("Invalid version")
    site = args.site_dir.resolve()
    html = (site / "index.html").read_text(encoding="utf-8")
    if args.version not in html:
        raise SystemExit("Manual version does not match the requested release")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"NvofPotPlayer-{args.version}-Manual-ko"
    standalone, archive = output / (name + ".html"), output / (name + ".zip")
    if standalone.exists() or archive.exists():
        raise SystemExit("Manual output already exists; existing assets are preserved")
    html = html.replace('<link rel="stylesheet" href="manual.css">',
                        '<style>' + (site / "manual.css").read_text(encoding="utf-8") + '</style>')
    script = (site / "manual.js").read_text(encoding="utf-8")
    html = html.replace('<script src="manual.js" defer></script>', '')
    html = html.replace('</body>', '<script>' + script + '</script></body>')
    def inline_image(match):
        image = (site / match.group(1)).resolve()
        if not image.is_relative_to(site) or not image.is_file():
            raise SystemExit("Image is outside the manual site or missing")
        mime = mimetypes.guess_type(image.name)[0]
        data = base64.b64encode(image.read_bytes()).decode("ascii")
        return f'src="data:{mime};base64,{data}"'
    html = re.sub(r'src="(images/[^"]+)"', inline_image, html)
    # The guide itself is fully offline; supplementary vendor documents remain
    # linked to the same release source. The ZIP also carries those documents.
    source = f"https://github.com/mkyoon80-alt/nvof-potplayer/blob/v{args.version}/"
    html = html.replace('href="THIRD_PARTY_NOTICES.md"', f'href="{source}THIRD_PARTY_NOTICES.md"')
    html = html.replace('href="licenses/', f'href="{source}licenses/')
    standalone.write_text(html, encoding="utf-8")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for file in sorted(site.rglob("*")):
            if file.is_file() and file.name != ".nojekyll":
                bundle.write(file, file.relative_to(site).as_posix())
    print(standalone)
    print(archive)


if __name__ == "__main__":
    main()
