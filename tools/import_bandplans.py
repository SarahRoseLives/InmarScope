"""Import pinned, GPL-compatible SDR catalogues into InmarScope's MHz JSON format.
This imports the entire available catalogues, not a claim of worldwide completeness.
"""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDRPP = '8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af'
SATELLITES = 'c5e767ec75b1511fdc44a0644f600d228144467d'
NAMES = 'australia austria belgium brazil canada china france general germany-mobile-lte-bands germany-mobile-networks germany ireland italy netherlands qo-100 republic-of-korea russia slovakia turkey united-kingdom usa'.split()
COLORS = {'amateur':'4CAF50','broadcast':'FF9800','military':'B85AC9','aviation':'4287F5','marine':'00A6A6','satellite':'D76B37','mobile':'C75151'}
FLEET_SOURCE = 'https://www.viasat.com/content/dam/us-site/government/missions/documents/Viasat-Aero-Services-brochure-September-2025-digital.pdf'
REGION_SOURCE = 'https://www.jsatmobile.com/general1/id=222'
# Keep legacy filenames so existing saved A/B selections remain valid.
SATELLITE_NAMES = {
    '4F3': ('4F3', 'I-4 F3 / 4F3 - 98W', ['AMER', 'Americas', 'AORW']),
    '3F5': ('3F5', 'I-3 F5 / 3F5 - 54W', ['AORE', 'Atlantic Ocean East']),
    'AF1': ('I4A', 'Alphasat / I4A / I-4A F4 / AF1 - 25E', ['EMEA', 'Europe Middle East Africa']),
    'F1': ('6F1', 'I-6 F1 / 6F1 - 83.5E', ['IOE', 'Indian Ocean East']),
}

def fetch(url):
    with urllib.request.urlopen(url) as response:
        return response.read()

def write(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')

def national(name):
    url=f'https://raw.githubusercontent.com/AlexandreRouma/SDRPlusPlus/{SDRPP}/root/res/bandplans/{name}.json'
    raw=fetch(url); source=json.loads(raw)
    bands=[]; rejected=[]
    for band in source['bands']:
        lo,hi=band['start']/1e6,band['end']/1e6
        if not 0 <= lo < hi:
            rejected.append(band); continue
        bands.append({'lo':lo,'hi':hi,'label':band['name'],'color':COLORS.get(band.get('type'),'888888')})
    country=source.get('country_name','')
    output={'name':source['name'],'designator':source.get('country_code',''),
            'regions':['Satellite' if name=='qo-100' else 'International (generic)' if name=='general' else 'National'],
            'countries':[country] if country and name not in ('general','qo-100') else [],
            'source':url,'source_sha256':hashlib.sha256(raw).hexdigest(),'license':'GPL-3.0',
            'source_metadata':{k:v for k,v in source.items() if k!='bands'},'excluded_invalid_source_entries':rejected,'bands':bands}
    write(ROOT/'bandplans'/'national'/f'{name}.json',output)
    return output

def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        plans=list(pool.map(national,NAMES))
    url=f'https://raw.githubusercontent.com/alphafox02/inmarsat-sniffer/{SATELLITES}/satellites.c'
    raw=fetch(url); text=raw.decode()
    arrays=dict(re.findall(r'static const channel_def_t (channels_\w+)\[\] = \{(.*?)\n\};',text,re.S))
    body=text.split('static const satellite_t satellites[] = {',1)[1].split('\n};',1)[0]
    metadata=re.findall(r'\.name = "([^"]+)",\s*\.designator = "([^"]+)",\s*\.position = ([\d.-]+),\s*\.region = "([^"]+)".*?\.channels = (channels_\w+),',body,re.S)
    for name,designator,position,region,array in metadata:
        channels=[]
        for hz,kind in re.findall(r'\{\s*([\d.]+),\s*(CHAN_\w+),',arrays[array]):
            mhz=float(hz)/1e6
            # Center-only markers: 1 Hz either side for the existing range schema,
            # displayed at a minimum pixel width. Not an occupied bandwidth claim.
            label=kind.removeprefix('CHAN_').replace('_',' ')
            channels.append({'lo':round(mhz-0.000001,6),'hi':round(mhz+0.000001,6),
                             **({'decoder':'egc'} if 'STDC' in kind else {'baud':int(kind.rsplit('_',1)[1])}),
                             'frequency':mhz,'service':'STD-C' if 'STDC' in kind else 'Aero voice' if '8400' in kind else 'Aero data',
                             'label':f'{label} {mhz:g} MHz (channel center)','color':'FF9800' if 'STDC' in kind else '4287F5'})
        if not channels: raise ValueError(f'Empty satellite table: {designator}')
        canonical,display,regions=SATELLITE_NAMES[designator]
        output={'name':display+' (surveyed channels; partial)','designator':canonical,'position':float(position),
                'regions':['Inmarsat']+regions, 'countries':[], 'source':url,'source_sha256':hashlib.sha256(raw).hexdigest(),
                'fleet_source':FLEET_SOURCE,'region_source':REGION_SOURCE,'reviewed':'2026-09-17',
                'license':'GPL-3.0-or-later','notes':'Surveyed channel centers, not complete allocations. Verify active frequencies locally. No geographic aliases inferred from the source.', 'bands':channels}
        write(ROOT/'bandplans'/'satellite'/f'inmarsat-{designator.lower()}.json',output);plans.append(output)
    # Extract numerical reception facts for the missing APAC table; do not copy
    # the guide's prose, images, or table presentation.
    apac_revision='828314e0512604879df6828ee5cbda2d02f9ea6d'
    apac_url=f'https://raw.githubusercontent.com/thebaldgeek/thebaldgeek.github.io/{apac_revision}/L-Band.md'
    apac_raw=fetch(apac_url)
    section=apac_raw.decode().split('### 4F1 143E',1)[1].split('###',1)[0]
    channels=[]
    for line in section.splitlines():
        match=re.match(r'^(\d{4},\d+)\s',line)
        baud=re.search(r'\(\s*(600|1200|8400|10500)\s*\)',line)
        if not match or not baud: continue
        center=float(match[1].replace(',','.'))
        channels.append({'lo':round(center-0.000001,6),'hi':round(center+0.000001,6),
                         'baud':int(baud[1]),
                         'frequency':center,'service':'Aero voice' if baud[1]=='8400' else 'Aero data',
                         'label':f'Aero {baud[1]} baud {center:g} MHz (channel center)','color':'4287F5'})
    if len(channels)!=32: raise ValueError('APAC survey changed; review conversion')
    apac={'name':'I-4 F1 / 4F1 - historical 143.5E APAC survey (not current 4F2)','designator':'4F1','position':143.5,
          'regions':['Inmarsat','Historical','APAC','Asia Pacific'],'countries':[],
          'source':apac_url,'source_sha256':hashlib.sha256(apac_raw).hexdigest(),
          'source_credit':'Frequency facts: David L. Wilson and Sergi.vdl2, February 2024; published by thebaldgeek.',
          'notes':'Historical survey labelled 4F1 by its author. APAC is now served by 4F2 at 143.5E; these channel centers have NOT been verified for 4F2. This position is historical, not a current 4F1 pointing instruction. +/-1 Hz is a display marker, not channel bandwidth.', 'bands':channels}
    write(ROOT/'bandplans'/'satellite'/'inmarsat-4f1.json',apac);plans.append(apac)
    # KrakenRF explicitly identifies the successor as 4F2, gives the same Aero
    # range and directs readers to this APAC channel survey. Preserve both sources
    # and the survey date: these are published presets, not live verification.
    apac_current={'name':'I-4 F2 / 4F2 - 143.5E (published APAC channels)',
          'designator':'4F2','position':143.5,'regions':['Inmarsat','APAC','Asia Pacific','POR'],
          'countries':[], 'fleet_source':FLEET_SOURCE,'region_source':REGION_SOURCE,
          'source':apac_url,'source_sha256':hashlib.sha256(apac_raw).hexdigest(),
          'identity_and_stdc_source':'https://github.com/krakenrf/discoverydish_docs/wiki/09.-Inmarsat-STD%E2%80%90C,-AERO-and-Pirates-Setup',
          'reviewed':'2026-09-17',
          'notes':'Published APAC channel presets: February 2024 Wilson/Sergi.vdl2 survey. KrakenRF identifies 4F2 as the successor at 143.5E, gives the matching Aero range and refers to this survey. STD-C 1541.450 MHz is listed explicitly for 4F2. Active channels/spot beams require local confirmation; this is not a live 2026 survey.',
          'bands':[{'lo':1541.449999,'hi':1541.450001,'frequency':1541.45,'service':'STD-C','decoder':'egc','label':'STD-C EGC 1541.450 MHz (channel center)','color':'FF9800'}]+channels}
    write(ROOT/'bandplans'/'satellite'/'inmarsat-4f2.json',apac_current);plans.append(apac_current)
    licenses=ROOT/'bandplans'/'licenses';licenses.mkdir(exist_ok=True)
    for name,url in [('SDRPlusPlus.txt',f'https://raw.githubusercontent.com/AlexandreRouma/SDRPlusPlus/{SDRPP}/license'),('inmarsat-sniffer.txt',f'https://raw.githubusercontent.com/alphafox02/inmarsat-sniffer/{SATELLITES}/LICENSE')]:
        (licenses/name).write_bytes(fetch(url))
    rejected=[(p['name'],e) for p in plans for e in p.get('excluded_invalid_source_entries',[])]
    countries=sorted({c for p in plans for c in p['countries']})
    report=f'# Included catalogue\n\n{len(plans)} plans, {sum(len(p["bands"]) for p in plans)} entries.\n\n'
    report+='National coverage: '+', '.join(countries)+'.\n\n'
    report+='Inmarsat plans: '+', '.join(p['designator']+' ('+', '.join(p['regions'][1:])+')' for p in plans if p['regions'][0]=='Inmarsat')+'.\n\n'
    report+='See [SATELLITES.md](SATELLITES.md) for the current L-band regional coverage, satellite aliases, orbital positions and the distinction between channel surveys, receive-band references and historical data.\n\n'
    report+='This is every plan in the pinned source catalogues, **not every country or every satellite/beam**. National plans vary in scope and may be outdated; Inmarsat tables are partial surveys. Generic international entries are not substitutes for missing national plans.\n\n'
    report+='Excluded malformed source records (not guessed or silently repaired): '+json.dumps(rejected,ensure_ascii=False)+'.\n\n'
    report+='Sources and conversion:\n\n- SDR++ '+SDRPP+': all 21 supplied plans; frequencies converted from Hz to MHz, source names/credits retained in each JSON file.\n- inmarsat-sniffer '+SATELLITES+': all 4 satellite channel tables; center markers use +/-1 Hz for display only, not asserted bandwidth. Original C table copyright: 2026 CEMAXECUTER LLC.\n\n'
    report+='APAC: 32 frequency/baud facts from the February 2024 Wilson/Sergi.vdl2 survey, published in thebaldgeek L-Band guide at revision '+apac_revision+'. Guide prose/images are not redistributed. Historical survey; verify current reception.\n\n'
    report+='The public SarahRoseLives/InmarScope repository has no bandplans or recordings directory. These catalogues are independently sourced additions, not copied from its separately distributed binary package. Add verified missing regional plans as described in README.md.\n'
    (ROOT/'bandplans'/'CATALOGUE.md').write_text(report,encoding='utf-8')
    print(report)

if __name__=='__main__': main()
