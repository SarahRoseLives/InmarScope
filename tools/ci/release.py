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
    for path in ["bandplans/README.md", "bandplans/CATALOGUE.md", "bandplans/SATELLITES.md", "recordings/README.md"]:
        if not files.get(path):
            raise RuntimeError(f"Missing band plan/recording folder documentation: {path}")
    plans = [p for p in files if p.startswith("bandplans/") and p.endswith(".json")]
    if len(plans) != 27:
        raise RuntimeError("Expected the complete 27-plan bundled catalogue")
    satellites = [json.loads(files[p]) for p in plans if p.startswith("bandplans/satellite/")]
    if len(satellites) != 6 or {p["designator"] for p in satellites} != {"I4A", "4F2", "3F5", "4F3", "6F1", "4F1"}:
        raise RuntimeError("Missing or duplicate Inmarsat satellite plans")
    if platform == "windows-x64":
        for path in ["flight-map/index.html", "flight-map/map.js", "flight-map/wheel_zoom.js", "flight-map/map.css", "flight-map/leaflet/leaflet.js", "flight-map/leaflet/leaflet.css", "flight-map/leaflet/LICENSE"]:
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
    "Fixes acquisition blockers: dual plots no longer repeatedly retune and recreate decoders while idle. "
    "Satellite channel groups now create the correct Aero baud/EGC decoders on selection and Start, "
    "independently for A/B. All supplied satellite channels carry explicit decoder modes. "
    "Groups respect SDRplay IF bandwidth and avoid placing carriers on the DC notch. "
    "Regression tests instantiate every channel group and check stable decoder IDs. Live RF lock still requires tester confirmation.\n\n"
    "Fixes stale spectrum/waterfall frequency ranges on Start. Selecting a satellite band plan now tunes "
    "to its Aero data group; Aero voice/STD-C and individual frequencies are selectable independently for A/B. "
    "Channel groups fit the chosen sample rate, without zooming to tiny channel markers. "
    "Adds actual startup/ImPlot regression tests and channel-group coverage tests.\n\n"
    "Makes Record voice calls the first control in both Decoders and Voice Calls, sharing the existing "
    "A/B recording state. Existing WAV/OGG and output-folder settings are preserved. Adds recording-file "
    "tests for PCM frames, WAV/OGG finalization and empty-file cleanup.\n\n"
    "Fixes delayed mouse-wheel zoom with frame-paced fractional zoom, cursor anchoring and accumulated wheel input. "
    "Removes redundant WebView2 resize calls. Adds wheel regression tests covering direction changes, zoom limits "
    "and drag/Fit interruption; received-aircraft membership and position lookup behaviour are unchanged.\n\n"
    "Includes upstream updates, antenna/model controls, two-device reception and RSPduo independent tuners.\n\n"
    "Corrects satellite names: I4A/Alphasat (25E), 4F2/APAC (143.5E), 3F5/AORE (54W), "
    "4F3/AMER (98W), and 6F1/IOE (83.5E). 4F2 now has 33 published APAC channel presets, "
    "replacing the broad allocation bar. Sources and survey dates are retained; active channels need local confirmation. "
    "The old 4F1 survey is explicitly historical. "
    "Plan notes now appear below the selector. See bandplans/SATELLITES.md for sources and coverage.\n\n"
    "Adds a visible Reset pane layout button and Ctrl+Shift+R. Bundles 27 band plans (17 countries, "
    "generic international/QO-100 plans and six Inmarsat entries), plus a recordings output folder. "
    "Plans are searchable by country/region and independently selected for A/B, with strict format validation. "
    "This is not an exhaustive worldwide catalogue; see bandplans/CATALOGUE.md for sources, historical survey limits "
    "and 13 excluded malformed source entries. Upstream's original packaged folders were not available in its public repo.\n\n"
    "All four platform builds and mock SDRplay tests passed in GitHub Actions. "
    "Packaged startup results are below; unavailable-opengl means the hosted VM has no usable graphics context, "
    "so rendering remains a tester check on that platform. Crashes and other startup failures block publication. "
    "RF hardware testing is still required. See TESTING.md for the tester checklist.\n\n"
    "RF gain now adjusts during reception, independently for A and B, including with AGC enabled. "
    "Fixes the map regression for received identities without decoded coordinates: optional ADSB.lol lookups "
    "supply positions only for received ICAO IDs. Unrelated online aircraft are discarded; decoded ADS-C coordinates "
    "take priority. Blue markers are decoded, orange are online, and lookup failures are shown. "
    "Aircraft without any known coordinates are listed separately. Incoming updates preserve pan/zoom; "
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
