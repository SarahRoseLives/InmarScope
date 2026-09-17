# Band plans

See [CATALOGUE.md](CATALOGUE.md) for included countries, satellite regions,
source revisions, exclusions and coverage limits. These are reception guides,
not a complete worldwide allocation database. Plans do not add decoders for
other radio modes or extend a receiver's tuning range.

See [SATELLITES.md](SATELLITES.md) for I4A, 4F2, 3F5, 4F3 and 6F1, their
regions/positions, and the separate historical 4F1 survey. Search **Inmarsat**
to show all six entries. 4F2 includes published APAC channel presets with
source/date notes; active channels still need confirmation from reception.

In Control, enable **Band Plan**, search **Region / country / plan**, and select
a plan. Receiver B has its own independent search and selection. Selections are
saved by file path so adding plans does not silently select a different one.
**Reload plans** refreshes the catalogue and the currently selected files.
Invalid files appear under **Band plan errors** and are never partially loaded.

The waterfall shows the selected plan name, frequency markers and active decoder
labels (channel ID plus MHz and baud/EGC). Green means locked; amber means acquiring.
Zoom or pan the spectrum to move the waterfall and labels together. Labels adapt
to pane width and UI scale; crowded labels become markers. Hover a marker or label
for its full details. National allocations use thin range strips, leaving the
waterfall visible. A and B use their own plans and decoder states.

Selecting a satellite plan tunes that receiver to its first **Aero data** group.
The frequency-group selector separates Aero data, Aero voice and STD-C, splitting
each service into windows that fit 80% of the sample rate or SDRplay IF bandwidth,
whichever is narrower. It shows the
frequency range and channel count. **Tune selected group** repeats the tuning;
expand **Channel frequencies (MHz)** to see or tune an individual frequency.
Tuning avoids putting channel centers on the receiver's DC notch.
Tuning works before Start and while running. It retains the sample rate and
resets the display to the receiver's full bandwidth, not a tiny marker width.
The antenna must already point at the selected satellite. WAV playback is fixed
frequency. National allocation plans remain overlays and do not auto-tune.

**Create decoders from plan** is enabled by default. Selecting a group or
individual frequency creates its decoders with the channel's recorded baud/mode;
Start restores that selection independently for A/B. Selecting a different group
replaces that receiver's decoders. Disable the option for manual decoder management.
The receiver being tuned is not a lock indication: the Decoders pane reports
acquisition/lock for each actual signal. Voice carriers may be silent between calls.

Place custom plans in any subfolder of `bandplans/`, then reload. Use UTF-8 JSON:

```json
{
  "name": "My verified regional plan",
  "designator": "CUSTOM",
  "regions": ["Region name"],
  "countries": ["Country name"],
  "source": "Reference URL and date",
  "bands": [
    {"lo": 1545.0, "hi": 1545.1, "label": "Example only", "color": "4287F5"}
  ]
}
```

The sample above demonstrates the format, not an asserted allocation.
Frequencies are **MHz**, with `0 <= lo < hi`; color is six hexadecimal RGB
digits, optionally prefixed with `#`. `name` and a nonempty `bands` array are
required. `designator`, `regions`, `countries` and satellite `position` in
degrees (-180 to 180) are optional. Singular `region`/`country` names also work.
Overlapping allocations are allowed. Satellite channel-center markers use
one hertz either side of a surveyed center, drawn at minimum pixel width;
this is not an occupied-bandwidth claim.
Channel entries additionally contain `frequency` (center in MHz, inside lo/hi)
and `service` (for example `Aero data`, `Aero voice`, `STD-C`). Without an explicit
`frequency`, an entry is an allocation overlay and never a tuning preset.
Automatic decoder creation also requires `baud` (600, 1200, 8400 or 10500) for
Aero, or `decoder: "egc"` for Inmarsat-C/EGC. Do not specify both. No mode is
inferred from a label or a country's general allocation.

SDR++ files use Hz and a different schema. `tools/import_bandplans.py` converts
the pinned catalogues to this format and preserves provenance/credits. Do not
copy an unconverted SDR++ file directly into this folder. The supplied sources
include partial and historical data; verify frequencies against actual reception
and current national publications before adding a plan.
