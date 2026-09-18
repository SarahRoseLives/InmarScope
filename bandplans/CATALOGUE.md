# Included catalogue

27 plans, 1771 entries.

National coverage: Australia, Austria, Belgium, Brazil, Canada, France, Germany, Italy, Netherlands, People's Republic of China, Republic Of Ireland, Republic of Korea, Russia, Slovakia, Turkey, United Kingdom, United States of America.

Inmarsat plans: 4F3 (AMER, Americas, AORW), 3F5 (AORE, Atlantic Ocean East), I4A (EMEA, Europe Middle East Africa), 6F1 (IOE, Indian Ocean East), 4F1 (Historical, APAC, Asia Pacific), 4F2 (APAC, Asia Pacific, POR).

See [SATELLITES.md](SATELLITES.md) for the current L-band regional coverage, satellite aliases, orbital positions and the distinction between channel surveys, receive-band references and historical data.

This is every plan in the pinned source catalogues, **not every country or every satellite/beam**. National plans vary in scope and may be outdated; Inmarsat tables are partial surveys. Generic international entries are not substitutes for missing national plans.

Excluded malformed source records (not guessed or silently repaired): [["Italy", {"name": "GSM-R", "type": "cellular", "start": 9210000000, "end": 925000000}], ["Italy", {"name": "Radiolocalizzazione GNSS", "type": "utility", "start": 1164000000, "end": 124000000}], ["Italy", {"name": "Wind profiler", "type": "military", "start": 1270000000, "end": 129800000}], ["Italy", {"name": "Radiolocalizzazione", "type": "military", "start": 1298000000, "end": 130000000}], ["Italy", {"name": "MSS 2 GHz", "type": "satellite", "start": 1980000000, "end": 201000000}], ["Italy", {"name": "Reti fisse numeriche", "type": "comms", "start": 22674750000, "end": 2283350000}], ["Italy", {"name": "LPR, SRD e SRR", "type": "utility", "start": 25109000000, "end": 2544500000}], ["Netherlands", {"name": "Digital network (fixed)", "type": "utility", "start": 22674750000, "end": 2283350000}], ["Netherlands", {"name": "LPR, SRD and SRR", "type": "utility", "start": 25109000000, "end": 2544500000}], ["Russia", {"name": "C-Band", "type": "broadcast", "start": 5850000000, "end": 5650000000}], ["Turkey", {"name": "75GHz", "type": "amateur", "start": 75500000000, "end": 7600000000}], ["UK", {"name": "11m Broadcast", "type": "broadcast", "start": 256700000, "end": 26100000}], ["UK", {"name": "Band 38 Cell phones", "type": "cellular", "start": 2500000000, "end": 269000000}]].

Sources and conversion:

- SDR++ 8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af: all 21 supplied plans; frequencies converted from Hz to MHz, source names/credits retained in each JSON file.
- inmarsat-sniffer c5e767ec75b1511fdc44a0644f600d228144467d: all 4 satellite channel tables; center markers use +/-1 Hz for display only, not asserted bandwidth. Original C table copyright: 2026 CEMAXECUTER LLC.

APAC: 32 frequency/baud facts from the February 2024 Wilson/Sergi.vdl2 survey, published in thebaldgeek L-Band guide at revision 828314e0512604879df6828ee5cbda2d02f9ea6d. Guide prose/images are not redistributed. Historical survey; verify current reception.

The public SarahRoseLives/InmarScope repository has no bandplans or recordings directory. These catalogues are independently sourced additions, not copied from its separately distributed binary package. Add verified missing regional plans as described in README.md.
