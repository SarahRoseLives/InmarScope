# SDRplay tester checklist

Use the Windows x64 ZIP from this fork's GitHub release. Extract the entire ZIP
to a writable folder; do not run the executable from inside the ZIP. Install
the SDRplay API/service from SDRplay's website, connect the radio, and close
other SDR programs. Run `sdrplay_probe.exe` from a terminal in the extracted
folder if discovery fails. See `SDRPLAY.md` for driver/model requirements.

1. Launch InmarScope, select SDRplay, find devices and select the correct serial.
2. Load device controls. Check that the displayed antennas match your model.
3. Select the connected antenna, a known signal and a supported sample rate.
   Start. Confirm spectrum/waterfall activity and decoding of a known signal.
4. Stop and change antenna, sample rate, bandwidth, manual gain and AGC. Restart
   after each change. Confirm the actual rate and signal level are sensible.
5. Test the model-specific notch, bias tee and HDR controls that your setup
   supports. Use bias power only with compatible connected equipment.
6. Close/reopen the app and confirm serial, antenna, rate and settings persist.
7. Repeat start/stop and switch to another source and back. Confirm the radio
   remains available. Unplug during reception: the app should report failure
   without hanging; reconnect, refresh devices and start again.
8. If you have two SDRplays, select two devices and different frequencies.
   Confirm both spectra and decoders work, and retuning B leaves A unchanged.
9. If you have an RSPduo, select two independent tuners. Confirm both tune
   independently, share the sample rate, and stop/restart without locking the
   API. B's model controls appear after the first start; stop to edit them.
10. Check voice following on B while A continues signalling. IQ recording
    captures A only; confirm WAV playback has the correct rate and duration.

Report the release version, Windows/macOS/Linux version, radio model, API and
Soapy driver versions, exact steps, expected/actual behaviour, and any displayed
error. Include a screenshot and the probe output if helpful. Do not include
private received message content unless you intend to share it.

Automated CI verifies compilation, mock backend tests, package manifests and
GUI startup. It does not verify RF performance or real-device API behaviour.
