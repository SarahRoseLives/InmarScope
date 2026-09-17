# Band plans

See [CATALOGUE.md](CATALOGUE.md) for included countries, satellite regions,
source revisions, exclusions and coverage limits. These are reception guides,
not a complete worldwide allocation database. Plans do not add decoders for
other radio modes or extend a receiver's tuning range.

See [SATELLITES.md](SATELLITES.md) for I4A, 4F2, 3F5, 4F3 and 6F1, their
regions/positions, and the separate historical 4F1 survey. Search **Inmarsat**
to show all six entries. 4F2 currently has a receive-band reference only,
not a verified channel list. The selected plan's notes appear below the selector.

In Control, enable **Band Plan**, search **Region / country / plan**, and select
a plan. Receiver B has its own independent search and selection. Selections are
saved by file path so adding plans does not silently select a different one.
**Reload plans** refreshes the catalogue and the currently selected files.
Invalid files appear under **Band plan errors** and are never partially loaded.

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

SDR++ files use Hz and a different schema. `tools/import_bandplans.py` converts
the pinned catalogues to this format and preserves provenance/credits. Do not
copy an unconverted SDR++ file directly into this folder. The supplied sources
include partial and historical data; verify frequencies against actual reception
and current national publications before adding a plan.
