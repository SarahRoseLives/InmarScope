# Build and release CI

The workflow is `.github/workflows/build-release.yml`. It builds Windows x64,
Linux x64 (Ubuntu 22.04), macOS Intel and macOS Apple Silicon. Builds run on
pushes to `master` and `codex/**`, pull requests, tags starting with `v`, and
manual workflow dispatches. All four platforms must pass before a tag publishes.

## Set up your own fork

1. Fork the repository on GitHub and keep the workflow and `tools/ci/` files.
2. In **Settings → Actions → General**, enable Actions and allow the actions used
   by this repository (`actions/*` and `msys2/setup-msys2`). On a new fork, open
   **Actions** and enable workflows if GitHub asks.
3. The normal jobs use `contents: read`. The tag release job requests
   `contents: write` and uses the automatically supplied `GITHUB_TOKEN`.
   **No personal access token or stored repository secret is needed.** If your
   organization restricts workflow permissions, allow the release job to write
   repository contents. Do not add a token to source files.
4. Push a branch named `codex/test-ci` or open a pull request. Check all four
   matrix jobs in Actions. Every job must build, run `sdrplay_selftest`, create a
   package, then launch the packaged executable with `--smoke-test`. Rendering
   is checked where the hosted runner supplies a usable OpenGL context.
5. Download `package-*` artifacts from that run to test without publishing.
   `diagnostics-*` artifacts contain the CMake cache, test log and build manifest.
6. Merge the verified code into your default branch, set `INMARSCOPE_VERSION`
   in `src/version.h`, commit it, and tag the **same commit**. For example:

   ```sh
   git tag -a v1.0.19-sdrplay.2 -m "SDRplay model detection fix"
   git push origin master
   git push origin v1.0.19-sdrplay.2
   ```

   Choose a new version for later releases. The tag must equal `v` plus the
   source version. A hyphenated version is published as a prerelease.

## What a release contains

- Windows ZIP with executable, runtime DLLs, font, rebuilt SoapySDRPlay3 driver,
  SDRplay probe, documentation and build manifest.
- Linux tar.gz with executable, probe, font, launcher and Ubuntu dependency
  installation script. This is a distro-linked build, not an AppImage.
- Separate macOS Intel and Apple Silicon tar.gz packages with copied non-system
  dylibs, relative library paths and ad-hoc signatures. These are not notarized.
- `CI-SETUP.md`, `SDRPLAY.md`, `TESTING.md` and `SHA256SUMS.txt` as standalone assets.
- GitHub also generates source ZIP/tar archives for the tag.

Every package includes its startup result in `SMOKE-RESULT.json`. Some hosted
Windows/macOS VMs have no usable OpenGL pixel format. Only this specific GLFW
condition returns 77 and is recorded as `unavailable-opengl`, not a passed render
test. Actual crashes, timeouts, missing libraries and other startup errors fail
the build. Linux uses Xvfb/software graphics and must complete rendering. The
release notes list the result for every platform; native rendering still needs
testing on a real machine where the runner could not provide it.

Every package includes its commit SHA, version, platform and per-file hashes in
`BUILD-INFO.json`. The release job checks all four packages and their manifests,
creates a draft, uploads every asset, downloads the uploads again and compares
their hashes. Only then does it publish. It refuses to overwrite a published
release. If upload/verification fails, fix the problem and rerun the release job;
the existing draft can be completed safely.

Windows uses MSYS2 MINGW64 and an extracted, SHA-256-pinned PothosSDR SDK. Its
Soapy ABI is compatible with the shipped plugin; mixing an unrelated MinGW
Soapy plugin with an MSVC Soapy DLL is not supported. `windows-sdk.ps1` builds
SoapySDRPlay3 revision `48bd8b41072534018de1d74deb3dea5874d9e0e0` with Visual
Studio 2022, against API 3.15 development files from the checksum-pinned SDR++
SDK mirror. It extracts headers/import libraries only and does not install a
vendor service on CI. The user's vendor API DLL remains an external dependency.
The matching Visual C++ redistributable runtime is included in the package.
The script records SDK download URLs/hashes and the plugin revision; update
these deliberately and rerun the entire matrix when upgrading dependencies.

Linux uses apt dependencies. macOS uses Homebrew and bundles its non-system
runtime libraries. CI also tests decoder-only map snapshots and marker updates
without contacting a traffic provider. The Windows package includes the local
`flight-map/` page and pinned Leaflet 1.9.4 assets; release validation requires
these files. Keep the full folder with the executable when distributing it.
Every OS package also contains `bandplans/` and a clean `recordings/` output
folder. CI tests all bundled plans with the native parser and release validation
checks the complete 27-plan inventory and all six Inmarsat identities. `tools/import_bandplans.py` reproduces
the pinned source conversion; update catalogue counts/tests deliberately when
adding plans. Never package personal recordings.

Hosted images and OS packages evolve; every tagged release
is rebuilt and tested rather than reusing a previous binary. No signing secrets
are needed; Windows is unsigned and macOS is ad-hoc signed. Public signing and
Apple notarization are separate future setup steps.

## Troubleshooting

- A missing Soapy SDK is a hard failure for CI (`REQUIRE_SDRPLAY=ON`), preventing
  a release that silently omits SDRplay.
- A missing test is a hard failure (`ctest --no-tests=error`).
- Failed jobs prevent the release job from running. Inspect the failed step and
  diagnostics, fix it, push, and wait for all platforms to pass before tagging.
- A tag/version mismatch fails publication. Use a new correctly versioned tag;
  do not move an already published tag.
- SDRplay hardware and its vendor service are not present on GitHub runners.
  Mock tests and GUI startup are automated; real RF testing is done by the
  hardware tester using `TESTING.md`.

References: [GitHub Actions workflows](https://docs.github.com/en/actions/writing-workflows),
[hosted runners](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job),
[MSYS2 setup action](https://github.com/msys2/setup-msys2).
