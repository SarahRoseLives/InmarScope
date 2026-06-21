# Building InmarScope on Windows

InmarScope is built with the **MSYS2 / MinGW-w64** toolchain (GCC 15.x) using
**CMake** and **Ninja**. The build is reproducible from a clean MSYS2 install.

> Important: GCC must be invoked from inside the **MINGW64** environment. If you
> run `g++.exe` directly from PowerShell/cmd without that environment set up,
> `cc1plus` dies silently with exit code 1 and no diagnostics. Always build from
> the **MSYS2 MINGW64** shell (or set `MSYSTEM=MINGW64` before launching bash).

## 1. Install MSYS2

Download and install from <https://www.msys2.org>, then open the
**"MSYS2 MINGW64"** shell (not the plain "MSYS2 MSYS" shell) from the Start menu.

Update the package database once:

```bash
pacman -Syu
# close and reopen the MINGW64 shell if it asks you to, then:
pacman -Su
```

## 2. Install build tools and dependencies

```bash
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-glfw \
  mingw-w64-x86_64-rtl-sdr \
  mingw-w64-x86_64-libusb \
  mingw-w64-x86_64-zstd
```

These provide: GCC/G++, CMake, Ninja, pkg-config, GLFW (windowing), librtlsdr +
libusb (RTL-SDR), and zstd (SDR++ server compression). OpenGL ships with the
toolchain. Dear ImGui, ImPlot, the JAERO DSP, mbelib and miniaudio are vendored
in `third_party/` and need no separate install.

## 3. Configure and build

From the **MINGW64** shell, in the project root:

```bash
cmake -S . -B build -G Ninja
ninja -C build
```

The executable and the runtime DLLs it needs are placed in `build/`:

```
build/InmarScope.exe
build/libgcc_s_seh-1.dll, libwinpthread-1.dll, libstdc++-6.dll,
      glfw3.dll, librtlsdr.dll, libusb-1.0.dll, libzstd.dll
```

The DLLs are copied next to the `.exe` automatically (POST_BUILD step), so it
runs standalone from a double-click or from PyCharm without MSYS2 on `PATH`.

Run it:

```bash
./build/InmarScope.exe
```

## 4. Building from PowerShell (optional)

If you prefer to drive the build from PowerShell, you must enter the MINGW64
environment first. For example:

```powershell
$env:MSYSTEM = "MINGW64"
$env:CHERE_INVOKING = "1"
& C:\msys64\usr\bin\bash.exe -lc "cd /c/path/to/InmarScope && ninja -C build"
```

The `-l` (login) shell with `MSYSTEM=MINGW64` sources `/etc/profile`, which puts
`/mingw64/bin` on `PATH` so `cc1plus` can find its runtime. `CHERE_INVOKING=1`
keeps the current directory.

## Notes / troubleshooting

- **First build is slow (~1 min on `implot_items.cpp`).** ImPlot's
  `implot_items.cpp` is extremely template-heavy; at `-O2` it takes ~5 minutes
  to compile. CMakeLists.txt overrides it to `-O1` (last `-O` flag wins) so it
  builds in about a minute. This is intentional, not a hang.
- **`cc1plus` exits 1 with no output.** You are not in the MINGW64 environment.
  Build from the MSYS2 MINGW64 shell (see the warning above).
- **Link fails with "Access is denied" on `InmarScope.exe`.** A previous instance
  is still running and is locking the file. Close it, then rebuild.
- **Incremental builds.** Re-run `ninja -C build`. Only changed files recompile.
  After editing `CMakeLists.txt`, re-run `cmake -S . -B build` first.
- **Clean rebuild.** Delete the `build/` directory and re-run the configure +
  build steps. (Avoid `--clean-first`; it wipes the vendored objects and forces
  the slow `implot_items.cpp` recompile.)
```
