# SDRplay receivers

This backend uses the SoapySDR C API and the **SoapySDRPlay3** driver. Its target
device family is RSP1, RSP1A, RSP1B, RSP2/RSP2pro, RSPduo, RSPdx and RSPdx-R2.
Actual device recognition and controls depend on the installed driver and
SDRplay API version. Use current releases for the newer models. This is not an
implementation of every proprietary SDRplay API entry point.

## Setup

Install the [SDRplay API and service](https://www.sdrplay.com/api/) and a matching
[SoapySDRPlay3](https://github.com/pothosware/SoapySDRPlay3) installation.
On Windows, a SoapySDR distribution such as PothosSDR supplies the library and
plugins, but an old bundled plugin may need updating for newer receivers.
Installing a DLL alone does not install the SDRplay API service.

Check the installation with `SoapySDRUtil --find="driver=sdrplay"` before opening
InmarScope. Close other applications using the same receiver.

Select **SDRplay**, click **Find SDRplay devices**, choose a serial number, then
click **Load device controls**. Choose an antenna and settings, then **Start**.
Settings are saved separately for receivers A and B in `inmarscope.ini`.
Frequency can change while receiving. Other hardware controls are edited while
stopped and applied on the next Start. Serial selection survives enumeration
order changes. An unavailable serial fails explicitly instead of selecting a
different device.

## Controls

| Control | Availability |
| --- | --- |
| Antennas / tuner inputs | Names queried from the selected model and mode, including high-impedance inputs where exposed |
| Sample rate / decimation | Driver-advertised output rates; driver chooses the hardware clock, IF and decimation; actual output rate drives DSP |
| IF bandwidth | Driver-advertised bandwidths and automatic selection |
| RF/IF gain, AGC | Named gain stages and ranges, automatic gain and driver-exposed AGC setpoint |
| PPM | Frequency correction where supported; RSPduo slave uses the master clock |
| DC / IQ correction | Standard correction capabilities plus driver-specific settings |
| Bias tee, RF/DAB notch, external reference, HDR | Model-specific controls reported by the driver; unsupported controls are not invented |

Settings with restricted options or numeric ranges are validated before
starting. Hardware and firmware restrictions still apply (for example HDR
frequency ranges and which connector supplies bias power). Consult the device
manual. Gain ranges are checked again at the configured frequency.

## Two receive paths

**Two SDRplay devices** opens two distinct serial numbers with independent
frequencies, sample rates and settings. Each has a spectrum, waterfall and
decoder manager. The existing voice-follow workflow can tune B while A keeps
decoding signalling.

**RSPduo two independent tuners** opens A as a 6 MHz-clock master and B as its
slave. This uses two independently tunable driver instances, rather than the
driver's dual-channel mode with shared tuning controls. A's antenna selection
chooses its tuner; the API assigns the remaining tuner to B. B's controls become
available after the first successful start; stop to edit them. The two paths use
the same configured output rate and master PPM. The slave always stops before
the master. A B-start failure stops both paths and reports the failure.

Single-path operation also exposes RSPduo ST, MA, MA8 and SL modes for use with
an external master/slave application. Only select those modes when the other
application is configured accordingly; the underlying driver may wait for an
external slave to release its device during shutdown.

There are **two simultaneous receive paths**, not an arbitrary receiver count.
IQ WAV recording captures **A only**, avoiding corrupt mixed-rate/interleaved
recordings. Both decoder managers retain voice recording. Coherent diversity
combining, synchronized dual-channel IQ recording, raw vendor API debug/reset
controls and vendor features not exposed by SoapySDRPlay3 are not implemented.

## Verification and remaining hardware checks

`sdrplay_selftest` uses a fake Soapy C boundary to test persistence, capability
discovery, invalid settings, device/start failure cleanup, two independent
sample streams, overflow reporting, disconnect, stop and slave PPM handling.
`sdrplay_probe` uses the real installed driver to enumerate devices without
tuning or enabling antenna power.

The development machine's installed vendor API fails to open and has no working
SDRplay API service. RF reception and individual models have **not** been
hardware-validated. Before release, check each model's antennas, sample rates,
gain, bias tee, notch/HDR controls, repeated start/stop and unplug recovery, plus
two physical devices and RSPduo master/slave tuning and shutdown. Mock tests
cannot establish those hardware behaviours.
