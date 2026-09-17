# SDRplay tester checklist

Use the Windows x64 ZIP from this fork's GitHub release. Extract the entire ZIP
to a writable folder; do not run the executable from inside the ZIP. Install
the SDRplay API/service from SDRplay's website, connect the radio, and close
other SDR programs. Run `sdrplay_probe.exe` from a terminal in the extracted
folder if discovery fails. See `SDRPLAY.md` for driver/model requirements.

1. Launch InmarScope, select SDRplay, find devices and select the correct serial.
   Each connected radio should appear once; RSPduo's alternate modes should not
   duplicate it. Only an RSPduo should show the RSPduo mode selector. Test each
   model available to you; RSP2pro can identify as RSP2.
2. Load device controls. Check that the displayed antennas match your model.
3. Select the connected antenna, a known signal and a supported sample rate.
   Start. Confirm spectrum/waterfall activity and decoding of a known signal.
4. While receiving, move RF gain and confirm the signal level changes without
   stopping. Repeat on receiver B and with AGC enabled; manual IF gain remains
   disabled under AGC. Stop and change antenna, sample rate, bandwidth and AGC. Restart
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
    **Record voice calls** is the first control in both **Decoders** and
    **Voice Calls**. Toggle either one and confirm both show the same state.
    Select WAV/OGG and the output folder in Decoders; record a call, stop recording,
    and play the resulting file. Repeat with receiver B. Check the toggle remains
    accessible in a short pane (scroll to the top if needed).
11. The Flight Map must start with no aircraft until your decoder receives
    them. Decode ADS-C positions and compare the markers with the Aircraft
    table. With **Online positions for received aircraft** enabled, identity-only
    aircraft may get orange ADSB.lol positions; decoded positions stay blue and
    take priority. Only IDs in the receiver table may appear. With the option
    off, identity-only aircraft remain in **Received without a known position**.
    Check the lookup status, disconnect the network, and confirm decoded markers
    still update. Check the option survives restart. Test A/B duplicates and
    clearing the Aircraft table, including while a lookup is pending.
    Pan/zoom, then decode another aircraft: the view must stay unchanged.
    **Fit received aircraft** frames the markers only when clicked.
    Scroll several small wheel increments, reverse direction, then drag or click
    Fit while zooming. Zoom should follow the cursor smoothly without delayed
    whole-level jumps or movement after Fit/drag takes over. Try a populated map.
12. Move/resize/detach several panes, then press **Reset pane layout** (or
    Ctrl+Shift+R). Check that default docking returns and radio settings remain.
13. Select different country or satellite plans for A/B. Restart and confirm
    both choices persist. Reload after editing a plan; malformed JSON/ranges
    must show an error and never a partially loaded overlay. Check the bundled
    catalogue and recordings folders are present after extraction.
    Select I4A or 4F2 before Start: the center frequency should change to its
    Aero data group. Start at 2 Msps: the spectrum should show approximately
    2 MHz, with the waterfall aligned, not a zoomed-in channel marker. Repeat
    after stopping and changing from a narrow sample rate/different frequency.
    Switch Aero data/voice/STD-C groups while running and confirm only the
    selected receiver tunes. Expand Channel frequencies to select one channel.
    Repeat with 250 ksps to check groups split into smaller capture windows.
    National allocation plans must not retune; WAV tuning must be disabled.

Report the release version, Windows/macOS/Linux version, radio model, API and
Soapy driver versions, exact steps, expected/actual behaviour, and any displayed
error. Include a screenshot and the probe output if helpful. Do not include
private received message content unless you intend to share it.

Automated CI verifies compilation, mock backend tests, package manifests and
startup. `SMOKE-RESULT.json` records whether rendering passed or the hosted VM
lacked a usable OpenGL context. Verify the GUI on your own machine in the latter
case. CI does not verify RF performance or real-device API behaviour.

`spectrum_view_selftest` exercises the actual Start and ImPlot drawing code
with a generated WAV and checks the first frame before any new FFT arrives.
It checks all 27 overlays, independent A/B tuning and fixed-frequency WAVs.
`band_plan_selftest` checks every satellite channel is assigned exactly once
to a service group inside the selected capture bandwidth (62.5 ksps–10 Msps).

The native map regression test now decodes a public libacars ADS-C fixture,
passes it through AircraftTable, and checks map JSON. It also covers received
identities with online-only positions, filtering unrelated response aircraft,
expiry, malformed data, source labels, and clearing while a response is pending.
The JavaScript test checks marker updates, removals, labels and unchanged view.
`node tools/flight_map_wheel_selftest.js` checks wheel input accumulation,
fractional frame updates, bounds and interruption. `tools/flight_map_browser_test.cjs`
also tests real-browser zoom with 500 markers and native WebView2 wheel handling.
For manual Windows integration, `flight_map_bridge_test.exe` exercises the real
WebView2 bridge with synthetic receiver records for 25 seconds; it is not bundled
with releases. Live provider availability and RF reception remain separate checks.
