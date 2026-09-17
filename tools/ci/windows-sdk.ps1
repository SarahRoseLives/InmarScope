$ErrorActionPreference = 'Stop'
$archive = Join-Path $env:RUNNER_TEMP 'pothos.exe'
Invoke-WebRequest 'https://downloads.myriadrf.org/builds/PothosSDR/PothosSDR-2021.07.25-vc16-x64.exe' -OutFile $archive
$expected = '705D962F578A9595A31D00E71AC68EB5CA951F3D3CB6091BF99BD13368960DA9'
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $expected) { throw 'Pothos SDK checksum mismatch' }
& 7z x $archive '-ovendor/pothos' 'include/SoapySDR/*' 'cmake/Soapy*' 'lib/SoapySDR.lib' 'bin/SoapySDR.dll' 'licenses/SoapySDR/*' -y
if ($LASTEXITCODE -ne 0) { throw 'SDK extraction failed' }

# Use the same SDK mirror as SDR++ CI; extract development files only. Never
# install a USB driver/service on a runner or distribute the proprietary DLL.
$apiArchive = Join-Path $env:RUNNER_TEMP 'sdrplay-sdk.zip'
Invoke-WebRequest 'https://www.sdrpp.org/SDRplay.zip' -OutFile $apiArchive
if ((Get-FileHash $apiArchive -Algorithm SHA256).Hash -ne 'DDB9810B4708B9F53DD7DAD235A7C5AD6990D8D0A0C8A141548D01F80C8C92D0') {
    throw 'SDRplay API 3.15 SDK checksum mismatch'
}
& 7z x $apiArchive '-ovendor/sdrplay-sdk' 'SDRplay/API/inc/*' 'SDRplay/API/x64/*.lib' -y
if ($LASTEXITCODE -ne 0) { throw 'SDRplay development files extraction failed' }
$revision = '48bd8b41072534018de1d74deb3dea5874d9e0e0'
git clone https://github.com/pothosware/SoapySDRPlay3.git vendor/SoapySDRPlay3
if ($LASTEXITCODE -ne 0) { throw 'SoapySDRPlay3 clone failed' }
git -C vendor/SoapySDRPlay3 checkout --detach $revision
if ($LASTEXITCODE -ne 0) { throw 'SoapySDRPlay3 checkout failed' }
$sdk = (Resolve-Path vendor/pothos).Path
$api = (Resolve-Path vendor/sdrplay-sdk/SDRplay/API).Path
cmake -S vendor/SoapySDRPlay3 -B vendor/soapy-plugin -G 'Visual Studio 17 2022' -A x64 "-DSoapySDR_DIR=$sdk/cmake" "-DLIBSDRPLAY_INCLUDE_DIRS=$api/inc" "-DLIBSDRPLAY_LIBRARIES=$api/x64/sdrplay_api.lib"
if ($LASTEXITCODE -ne 0) { throw 'SoapySDRPlay3 configure failed' }
cmake --build vendor/soapy-plugin --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'SoapySDRPlay3 build failed' }
New-Item -ItemType Directory -Force "$sdk/lib/SoapySDR/modules0.8", "$sdk/licenses/SoapySDRPlay3" | Out-Null
Copy-Item vendor/soapy-plugin/Release/sdrPlaySupport.dll "$sdk/lib/SoapySDR/modules0.8/"
Copy-Item vendor/SoapySDRPlay3/LICENSE.txt "$sdk/licenses/SoapySDRPlay3/"
"SoapySDRPlay3 $revision; compiled against SDRplay API 3.15" | Set-Content "$sdk/licenses/SoapySDRPlay3/BUILD.txt"

# Bundle the runtime matching the compiler, not the obsolete Pothos CRT.
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt = Get-ChildItem "$vs/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (!$crt) { throw 'Visual C++ redistributable runtime not found' }
Copy-Item "$($crt.FullName)/*.dll" "$sdk/bin/"
