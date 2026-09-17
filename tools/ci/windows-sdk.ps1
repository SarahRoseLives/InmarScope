$ErrorActionPreference = 'Stop'
$archive = Join-Path $env:RUNNER_TEMP 'pothos.exe'
Invoke-WebRequest 'https://downloads.myriadrf.org/builds/PothosSDR/PothosSDR-2021.07.25-vc16-x64.exe' -OutFile $archive
$expected = '705D962F578A9595A31D00E71AC68EB5CA951F3D3CB6091BF99BD13368960DA9'
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne $expected) { throw 'Pothos SDK checksum mismatch' }
& 7z x $archive '-ovendor/pothos' 'include/SoapySDR/*' 'lib/SoapySDR.lib' 'bin/SoapySDR.dll' 'bin/sdrplay_api.dll' 'bin/msvcp140*.dll' 'bin/vcruntime140*.dll' 'bin/concrt140.dll' 'lib/SoapySDR/modules0.8/sdrPlaySupport.dll' 'licenses/SoapySDR/*' 'licenses/SoapySDRPlay3/*' -y
if ($LASTEXITCODE -ne 0) { throw 'SDK extraction failed' }
