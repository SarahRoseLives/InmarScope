"""Publish only a complete, verified set; draft remains private on failure."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

def gh(*args):
    return subprocess.check_output(["gh", *args], text=True)

def sha(data):
    return hashlib.sha256(data).hexdigest()

tag = os.environ["RELEASE_TAG"]
version = re.search(r'INMARSCOPE_VERSION "([^"]+)"', Path("src/version.h").read_text())[1]
if tag != "v" + version:
    raise RuntimeError("Tag must match src/version.h")
commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
directory = Path("release")
smoke_summary = []
for platform in ["windows-x64", "linux-x64", "macos-x64", "macos-arm64"]:
    name = f"InmarScope-{version}-{platform}"
    archive = directory / (name + (".zip" if platform.startswith("windows") else ".tar.gz"))
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            files = {n[len(name)+1:]: z.read(n) for n in z.namelist() if not n.endswith("/")}
    else:
        with tarfile.open(archive) as t:
            files = {m.name[len(name)+1:]: t.extractfile(m).read() for m in t if m.isfile()}
    info = json.loads(files["BUILD-INFO.json"])
    smoke = json.loads(files["SMOKE-RESULT.json"])
    if smoke["status"] not in ("passed", "unavailable-opengl"):
        raise RuntimeError("Invalid startup test result")
    smoke_summary.append(f"- {platform}: {smoke['status']}")
    if info["commit"] != commit or info["platform"] != platform or info["version"] != version:
        raise RuntimeError(f"Wrong build provenance: {archive}")
    for path, digest in info["files"].items():
        if sha(files[path]) != digest:
            raise RuntimeError(f"Package checksum mismatch: {archive}: {path}")
    for path in ["CI-SETUP.md", "SDRPLAY.md", "TESTING.md", "TEST-RESULTS.txt"]:
        if not files.get(path):
            raise RuntimeError(f"Missing release documentation: {path}")
    if platform == "windows-x64":
        for path in ["flight-map/index.html", "flight-map/map.js", "flight-map/map.css", "flight-map/leaflet/leaflet.js", "flight-map/leaflet/leaflet.css", "flight-map/leaflet/LICENSE"]:
            if not files.get(path):
                raise RuntimeError(f"Missing local flight map asset: {path}")
    if any(p.endswith((".ini", ".pem", ".key")) for p in files):
        raise RuntimeError("Unexpected local configuration in release")
for doc in ["CI-SETUP.md", "SDRPLAY.md", "TESTING.md"]:
    shutil.copy2(doc, directory)
assets = sorted(directory.iterdir())
(directory / "SHA256SUMS.txt").write_text("".join(f"{sha(p.read_bytes())}  {p.name}\n" for p in assets))
assets.append(directory / "SHA256SUMS.txt")
notes = Path("release-notes.md")
notes.write_text(f"SDRplay testing release {tag}\n\n"
    "Includes upstream updates, antenna/model controls, two-device reception and RSPduo independent tuners.\n\n"
    "All four platform builds and mock SDRplay tests passed in GitHub Actions. "
    "Packaged startup results are below; unavailable-opengl means the hosted VM has no usable graphics context, "
    "so rendering remains a tester check on that platform. Crashes and other startup failures block publication. "
    "RF hardware testing is still required. See TESTING.md for the tester checklist.\n\n"
    "RF gain now adjusts during reception, independently for A and B, including with AGC enabled. "
    "The Flight Map now plots only locally decoded aircraft from receivers A/B, with no airplanes.live traffic feed. "
    "Aircraft without decoded coordinates are listed separately. Incoming updates preserve pan/zoom; "
    "Fit received aircraft frames the markers only when clicked. OpenStreetMap supplies background tiles only.\n\n"
    "Fixes non-RSPduo devices being rejected by RSPduo-only open arguments. Windows includes a rebuilt "
    "SoapySDRPlay3 driver supporting RSP1B and RSPdx-R2 as well as earlier models.\n\n"
    "Windows: extract the ZIP and install SDRplay API/service 3.15 or newer. "
    "See SDRPLAY.md for setup. Linux: Ubuntu 22.04+ x64, run install-dependencies.sh. "
    "macOS: choose Intel or Apple Silicon; binaries are ad-hoc signed, not Apple notarized. "
    "Linux/macOS SDRplay reception additionally needs SoapySDRPlay3 and the vendor API.\n\n"
    "CI-SETUP.md explains reproducing this pipeline in another fork. SHA256SUMS.txt covers all assets.\n\n"
    + "\n".join(smoke_summary) + "\n")
existing = subprocess.run(["gh", "release", "view", tag, "--json", "isDraft"], text=True, capture_output=True)
if existing.returncode == 0:
    if not json.loads(existing.stdout)["isDraft"]:
        raise RuntimeError("Refusing to replace an already published release; use a new version")
else:
    gh("release", "create", tag, "--verify-tag", "--draft", "--title", f"InmarScope {version}", "--notes-file", str(notes))
gh("release", "upload", tag, *(str(p) for p in assets), "--clobber")
with tempfile.TemporaryDirectory() as temporary:
    gh("release", "download", tag, "--dir", temporary)
    for p in assets:
        if sha((Path(temporary) / p.name).read_bytes()) != sha(p.read_bytes()):
            raise RuntimeError(f"Uploaded asset differs: {p.name}")
gh("release", "edit", tag, "--draft=false", "--prerelease=" + str("-" in version).lower())
print(gh("release", "view", tag, "--json", "url,assets"))
