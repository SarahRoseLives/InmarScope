"""Stage only distributable files, collect runtimes, and verify the archive."""
import argparse
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

ROOT = Path(__file__).resolve().parents[2]

def run(*args):
    return subprocess.check_output(args, text=True).strip()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def windows_runtime(dest, sdk):
    cache = (ROOT / "build/CMakeCache.txt").read_text()
    compiler = Path(re.search(r"CMAKE_CXX_COMPILER:FILEPATH=(.+)", cache)[1].strip()).parent
    search = [ROOT / "build", compiler, sdk / "bin"]
    shutil.copytree(sdk / "lib/SoapySDR", dest / "soapy/lib/SoapySDR")
    shutil.copytree(sdk / "licenses", dest / "licenses/Soapy")
    todo = list(dest.rglob("*.exe")) + list(dest.rglob("*.dll"))
    seen = set()
    system = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"
    while todo:
        binary = todo.pop()
        if binary in seen:
            continue
        seen.add(binary)
        output = run(str(compiler / "objdump.exe"), "-p", str(binary))
        for name in re.findall(r"DLL Name:\s*(\S+)", output):
            # Installed by the vendor API/service installer, not redistributed.
            if name.lower() == "sdrplay_api.dll":
                continue
            if (dest / name).exists():
                continue
            source = next((d / name for d in search if (d / name).is_file()), None)
            if source:
                target = dest / name
                shutil.copy2(source, target)
                todo.append(target)
            elif name.lower().startswith(("api-ms-", "ext-ms-")) or (system / name).exists():
                continue
            else:
                raise RuntimeError(f"Unresolved runtime dependency: {binary.name}: {name}")

def mac_runtime(dest):
    libdir = dest / "lib"
    libdir.mkdir()
    todo = [dest / "InmarScope", dest / "sdrplay_probe"]
    seen = set()
    while todo:
        binary = todo.pop()
        if binary in seen:
            continue
        seen.add(binary)
        binary.chmod(binary.stat().st_mode | 0o200)
        for line in run("otool", "-L", str(binary)).splitlines()[1:]:
            dep = line.strip().split(" (", 1)[0]
            if dep.startswith(("/usr/lib/", "/System/", "@loader_path/")):
                continue
            if dep.startswith("@rpath/"):
                # Homebrew's relocatable dylibs live under its lib/opt trees.
                prefix = Path(run("brew", "--prefix"))
                found = list((prefix / "lib").glob(Path(dep).name))
                if not found:
                    raise RuntimeError(f"Cannot resolve {dep} in {binary}")
                source = found[0]
            else:
                source = Path(dep)
            target = libdir / source.name
            if binary == target:  # dylib's own install ID
                continue
            if not target.exists():
                shutil.copy2(source.resolve(), target)
                todo.append(target)
            replacement = "@loader_path/" + ("" if binary.parent == libdir else "lib/") + target.name
            subprocess.check_call(["install_name_tool", "-change", dep, replacement, str(binary)])
        if binary.parent == libdir:
            subprocess.check_call(["install_name_tool", "-id", "@rpath/" + binary.name, str(binary)])
    for binary in sorted(seen, key=lambda p: p.parent != libdir):
        subprocess.check_call(["codesign", "--force", "--sign", "-", str(binary)])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", required=True, choices=["windows-x64", "linux-x64", "macos-x64", "macos-arm64"])
    parser.add_argument("--sdk", type=Path)
    args = parser.parse_args()
    version = re.search(r'INMARSCOPE_VERSION "([^"]+)"', (ROOT / "src/version.h").read_text())[1]
    name = f"InmarScope-{version}-{args.platform}"
    dest = ROOT / "out" / name
    dest.mkdir(parents=True, exist_ok=False)
    windows = args.platform.startswith("windows")
    for binary in ("InmarScope", "sdrplay_probe"):
        filename = binary + (".exe" if windows else "")
        shutil.copy2(ROOT / "build" / filename, dest / filename)
    docs = ["README.md", "COMPILE.md", "CI-SETUP.md", "SDRPLAY.md", "TESTING.md", "LICENSE"]
    for doc in docs:
        shutil.copy2(ROOT / doc, dest / doc)
    font = Path("third_party/imgui/misc/fonts/Roboto-Medium.ttf")
    (dest / font).parent.mkdir(parents=True)
    shutil.copy2(ROOT / font, dest / font)
    if windows:
        if not args.sdk:
            raise RuntimeError("Windows requires --sdk")
        shutil.copytree(ROOT / "assets/flight-map", dest / "flight-map")
        for dll in (ROOT / "build").glob("*.dll"):
            shutil.copy2(dll, dest)
        windows_runtime(dest, args.sdk.resolve())
    elif args.platform.startswith("macos"):
        mac_runtime(dest)
    else:
        setup = dest / "install-dependencies.sh"
        setup.write_text("#!/bin/sh\nset -eu\nsudo apt-get update\nsudo apt-get install -y libglfw3 libgl1 librtlsdr0 libhackrf0 libairspy0 libusb-1.0-0 libzstd1 zlib1g libogg0 libvorbis0a libvorbisenc2 libsqlite3-0 libxml2 libjansson4 libsoapysdr0.8\n")
        setup.chmod(0o755)
    if not windows:
        launcher = dest / ("Start-InmarScope.command" if args.platform.startswith("macos") else "start-inmarscope.sh")
        launcher.write_text('#!/bin/sh\nset -eu\ncd "$(dirname "$0")"\nexec ./InmarScope "$@"\n')
        launcher.chmod(0o755)
    executable = dest / ("InmarScope.exe" if windows else "InmarScope")
    command = [str(executable), "--smoke-test"]
    if args.platform.startswith("linux"):
        command = ["xvfb-run", "-a", *command]
        dependencies = run("ldd", str(executable))
        if "not found" in dependencies:
            raise RuntimeError(dependencies)
    with tempfile.TemporaryDirectory() as temporary:
        startup = None
        if windows:
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = subprocess.SW_HIDE
        result = subprocess.run(command, cwd=temporary, capture_output=True, text=True,
                                errors="replace", timeout=60, startupinfo=startup)
    status = "passed" if result.returncode == 0 else "unavailable-opengl" if result.returncode == 77 else "failed"
    smoke = {"status": status, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
    print("Packaged startup check:", json.dumps(smoke))
    if status == "failed" or (status == "unavailable-opengl" and args.platform.startswith("linux")):
        raise RuntimeError("Packaged startup check failed")
    (dest / "SMOKE-RESULT.json").write_text(json.dumps(smoke, indent=2) + "\n")
    shutil.copy2(ROOT / "build/Testing/Temporary/LastTest.log", dest / "TEST-RESULTS.txt")
    info = {"version": version, "platform": args.platform, "commit": run("git", "rev-parse", "HEAD"),
            "run": os.environ.get("GITHUB_RUN_ID"), "hardware_tested": False,
            "files": {p.relative_to(dest).as_posix(): sha(p) for p in sorted(dest.rglob("*")) if p.is_file()}}
    (dest / "BUILD-INFO.json").write_text(json.dumps(info, indent=2) + "\n")
    if windows:
        archive = dest.with_suffix(".zip") if "." not in name else dest.parent / (name + ".zip")
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for p in dest.rglob("*"):
                if p.is_file():
                    z.write(p, p.relative_to(dest.parent))
        with zipfile.ZipFile(archive) as z:
            if z.testzip():
                raise RuntimeError("Corrupt zip")
    else:
        archive = dest.parent / (name + ".tar.gz")
        with tarfile.open(archive, "w:gz") as t:
            t.add(dest, arcname=name)
    print(f"Packaged {archive.name}: {sha(archive)}")

if __name__ == "__main__":
    main()
