# Inmarsat satellite plans

Reviewed 17 September 2026. Search **Inmarsat** in the band-plan selector to
show every bundled Inmarsat plan. Search a satellite name or region to narrow
the list. Each receiver has an independent selection.

| Satellite / searchable names | Orbital position | Coverage reference | Included frequency data |
| --- | --- | --- | --- |
| I4A / Alphasat / I-4A F4 / AF1 | 25 E | EMEA: Europe, Middle East, Africa | Partial channel survey |
| 4F2 / I-4 F2 | 143.5 E | APAC / Asia-Pacific / POR | General 1525–1559 MHz receive-band reference; individual active channels **not verified** |
| 3F5 / I-3 F5 | 54 W | AORE: Atlantic Ocean East | Partial channel survey |
| 4F3 / I-4 F3 | 98 W | AMER / Americas / AORW | Partial channel survey |
| 6F1 / I-6 F1 | 83.5 E | IOE: Indian Ocean East | Partial channel survey |
| 4F1 / I-4 F1 — **historical** | Historical 143.5 E | Former APAC survey | 32 historical channel centers, retained under the source author's satellite label; not verified for current 4F2 |

The five regional service satellites above are listed in Viasat's
[September 2025 Aero Services brochure](https://www.viasat.com/content/dam/us-site/government/missions/documents/Viasat-Aero-Services-brochure-September-2025-digital.pdf),
which separately identifies 4F1 as backup/repositioning. Positions are regional
reference slots, not live orbital tracking. Footprints overlap and do not
guarantee reception at every location in a named region.

[JSAT's 10 September 2025 service bulletin](https://www.jsatmobile.com/general1/id=222)
identifies 4F2 at 143.5 E, 4F3 at 98 W and 6F1 at 83.5 E, including Classic
Aero service. The [Inmarsat migration report to ICAO](https://www.icao.int/sites/default/files/APAC/Meetings/2023/2023%20FIT%20Asia13/Flimsies/Flimsy01-Inmarsat-Update-on-Satellite-Service-Outage.pdf)
explains the planned transfer from 4F2 in MEAS to 6F1, and 4F2's relocation to
APAC after the 4F1 power failure. Older lists showing 4F2 at 64 E and 4F1 as the
current APAC satellite predate that transition.

4F2's band reference uses the [ITU's Inmarsat Asia-Pacific receive allocation](https://www.itu.int/net/Itu-R/space/res647/index.asp?nmod=asc&norder=freq_assgn).
It does **not** assert that every frequency is active or decodable. No verified
current 4F2 channel survey was found during this review. The old 4F1 table is
not silently relabelled as 4F2. All supplied channel surveys remain partial;
beam assignments and active channels must be checked against actual reception.

This is the L-band reception catalogue for InmarScope, not the entire Inmarsat
spacecraft fleet. Global Xpress I-5 and other Ka-band spacecraft do not supply
these L-band channels. Retired, failed, future or contingency spacecraft are
not presented as active regional channel plans. The historical 4F1 entry is
explicitly labelled so existing saved selections remain usable.

Legacy filenames `inmarsat-af1.json` and `inmarsat-f1.json` are retained for
saved-selection compatibility; their displayed satellite names are now I4A
and 6F1. For channel-source revisions and national plans see
[CATALOGUE.md](CATALOGUE.md). Both the catalogue and this reference are included
in every platform's release archive.
