"""Release helpers for .github/workflows/release.yml.

    release.py check-version vX.Y.Z              tag matches `version` in both firmware configs
    release.py site vX.Y.Z IMAGES_DIR OUT_DIR    install page, images and manifests for GitHub Pages

The manifests serve two readers: the browser installer on the install page (ESP Web Tools: `parts`)
and the bridges' update entity in Home Assistant (ESPHome's http_request update: `ota`, matched by
chipFamily). Paths are relative to the manifest.
"""

import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
REPO = "paramworks-git/param-key"
CONFIGS = [ROOT / "firmware" / "bridge.yaml", ROOT / "firmware" / "common" / "remote_core.yaml"]
BRIDGES = [("ESP32", "bridge-esp32"), ("ESP32-S3", "bridge-esp32-s3"), ("ESP32-C3", "bridge-esp32-c3")]
REMOTES = [("ESP32-S3", "remote-v1")]


def config_version(path: Path) -> str:
    m = re.search(r'^\s+version:\s*"([^"]+)"\s*$', path.read_text(), re.M)
    if not m:
        sys.exit(f"{path.relative_to(ROOT)}: no `version:` substitution")
    return m.group(1)


def check_version(tag: str) -> None:
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        sys.exit(f"tag {tag!r} isn't vX.Y.Z")
    wrong = [f"{c.relative_to(ROOT)} says {config_version(c)}" for c in CONFIGS if config_version(c) != tag[1:]]
    if wrong:
        sys.exit(f"tag {tag} doesn't match: " + "; ".join(wrong))
    print(f"{tag} matches both firmware configs")


def md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def site(tag: str, images: Path, out: Path) -> None:
    check_version(tag)
    version = tag[1:]
    release_url = f"https://github.com/{REPO}/releases/tag/{tag}"
    shutil.copytree(ROOT / "site", out)
    fw = out / "firmware"
    fw.mkdir()
    for target in [t for _, t in BRIDGES + REMOTES]:
        for kind in ("factory", "ota"):
            src = images / f"{target}.{kind}.bin"
            if not src.is_file():
                sys.exit(f"missing image {src}")
            shutil.copy2(src, fw / src.name)

    bridge = {
        "name": "PARAM KEY bridge",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": chip,
                "ota": {
                    "path": f"{t}.ota.bin",
                    "md5": md5(fw / f"{t}.ota.bin"),
                    "summary": f"PARAM KEY bridge {version}",
                    "release_url": release_url,
                },
                "parts": [{"path": f"{t}.factory.bin", "offset": 0}],
            }
            for chip, t in BRIDGES
        ],
    }
    # The remote has no Wi-Fi, so the installer can't recognise an update and treats
    # every install as new: ask before erasing (erasing unpairs it).
    remote = {
        "name": "PARAM KEY remote (v1)",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": chip, "parts": [{"path": f"{t}.factory.bin", "offset": 0}]} for chip, t in REMOTES],
    }
    (fw / "bridge.manifest.json").write_text(json.dumps(bridge, indent=2) + "\n")
    (fw / "remote-v1.manifest.json").write_text(json.dumps(remote, indent=2) + "\n")
    page = out / "index.html"
    page.write_text(page.read_text().replace("{{VERSION}}", version).replace("{{RELEASE_URL}}", release_url))
    for f in sorted(out.rglob("*")):
        if f.is_file():
            print(f"  {f.relative_to(out)}  {f.stat().st_size} B")


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "check-version":
        check_version(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "site":
        site(sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
