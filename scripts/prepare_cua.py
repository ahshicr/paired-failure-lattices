"""Reproducible audit and interval union of the immutable CUA infrastructure release.

Reported degradation is an availability stress footprint, not destroyed storage.
No synthetic severity is used. Ambiguous geographical scope is retained under
both identified-only and vendor-wide interpretations; every source row is audited.
"""
import collections,csv,hashlib,json,pathlib,re,sys
import pandas as pd

ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'experiments'))
from instance_io import leaf_masks

def normal(s):return re.sub(r'\s+',' ',str(s)).strip()

def main():
    frames=[]
    for year in (2018,2019,2020):
        name=f'{year}_fixed_infra_cloud.parquet';p=ROOT/'data/raw/cloud_uptime'/name
        d=pd.read_parquet(p);d['source_year']=year;d['source_row']=range(len(d));d['source_file']=name;d['source_sha256']=hashlib.sha256(p.read_bytes()).hexdigest();frames.append(d)
    df=pd.concat(frames,ignore_index=True)
    # Location vocabulary is geographical metadata, shared across chronological
    # splits. No future masses or objective values influence training or menus.
    locations={v:set() for v in ('AWS','Azure','GCP')}
    ambiguous={'multi-region','unknown','global','none','nan','north america','us-east','us-west'}
    for r in df.itertuples():
        loc=normal(r.location)
        if loc.lower() in ambiguous or 'Tracking ID' in loc or 'Intermittent' in loc:continue
        if r.vendor=='Azure':
            loc=loc.replace('Australia East/Southeast','Australia East / Australia Southeast')
            for bit in re.split(r'\s*/\s*|\s+and\s+',loc):locations[r.vendor].add(bit.strip())
        else:locations[r.vendor].add(loc)
    labels=['root'];parent=[-1];vendor_nodes={};region_nodes={}
    for vendor in sorted(locations):
        vendor_nodes[vendor]=len(labels);labels.append(vendor);parent.append(0)
        for loc in sorted(locations[vendor]):region_nodes[(vendor,loc)]=len(labels);labels.append(vendor+'/'+loc);parent.append(vendor_nodes[vendor])
    masks,leaves=leaf_masks(parent);full=(1<<len(leaves))-1
    def cover(mask):
        return [i for i,m in enumerate(masks) if m&mask==m and (parent[i]<0 or masks[parent[i]]&mask!=masks[parent[i]])]
    paired={0:[]}
    for i,m in enumerate(masks):paired.setdefault(m,[i])
    for i,m in enumerate(masks):
        for j in range(i+1,len(masks)):paired.setdefault(m|masks[j],cover(m|masks[j]))
    sorted_paired=sorted(paired,key=lambda m:(m.bit_count(),m))
    def envelope(mask):return next(m for m in sorted_paired if mask&~m==0)
    audit=[]
    for r in df.itertuples():
        loc=normal(r.location);vendor=r.vendor
        if (vendor,loc) in region_nodes:
            nodes=[region_nodes[(vendor,loc)]];mapping='literal_single_region'
        else:
            scan=loc;nodes=[]
            for name in sorted(locations[vendor],key=lambda x:(-len(x),x)):
                pattern=r'(?<![\w-])'+re.escape(name)+r'(?![\w-])'
                if re.search(pattern,scan,re.I):nodes.append(region_nodes[(vendor,name)]);scan=re.sub(pattern,' ',scan,flags=re.I)
            if vendor=='AWS' and loc=='US-West':nodes=[region_nodes[(vendor,x)] for x in ('N. California','Oregon') if (vendor,x) in region_nodes]
            if vendor=='AWS' and loc=='US-East':nodes=[region_nodes[(vendor,x)] for x in ('N. Virginia','Ohio') if (vendor,x) in region_nodes]
            if loc=='Australia East/Southeast':nodes=[region_nodes[(vendor,x)] for x in ('Australia East','Australia Southeast')]
            mapping='named_region_union' if nodes else 'ambiguous_scope'
        s=pd.Timestamp(r.event_start_time);e=pd.Timestamp(r.event_end_time)
        start=int(s.value//10**9);end=int(e.value//10**9);valid=end>start
        storage=bool(re.search(r'Google Cloud Storage|Simple Storage Service|^Storage(?:\s|$)',normal(r.service_name),re.I))
        flags=[]
        if not valid:flags.append('nonpositive_duration')
        if '2017' in str(r.description) and r.source_year==2018:flags.append('description_mentions_previous_year')
        if r.status==0:flags.append('status_zero_retained_no_severity_inference')
        if 'not affected' in str(r.description).lower() or 'not have been affected' in str(r.description).lower():flags.append('description_contains_unaffected_scope')
        audit.append({'row_id':f'{r.source_year}_{r.source_row:04d}','source_file':r.source_file,'source_sha256':r.source_sha256,'source_row':r.source_row,'year':r.source_year,'vendor':vendor,'service':normal(r.service_name),'reported_location':loc,'status':float(r.status),'start_seconds':start,'end_seconds':end,'positive_duration':valid,'mapping':mapping,'identified_domains':sorted(set(nodes)),'vendor_envelope_domains':sorted(set(nodes)) if nodes else [vendor_nodes[vendor]],'storage_service_report':storage,'audit_flags':flags,'description_sha256':hashlib.sha256(str(r.description).encode()).hexdigest()})
    out=ROOT/'data/processed';out.mkdir(exist_ok=True)
    (out/'cua_row_audit.json').write_text(json.dumps(audit,indent=2),encoding='utf-8')
    with (out/'cua_row_audit.csv').open('w',encoding='utf-8',newline='') as f:
        w=csv.DictWriter(f,fieldnames=list(audit[0]));w.writeheader();w.writerows(audit)
    distributions={};intervals=[];stats=[]
    for population in ('all_services','storage_services'):
        for scope in ('identified','vendor_envelope'):
            for year in (2018,2019,2020):
                rows=[r for r in audit if r['year']==year and (population=='all_services' or r['storage_service_report'])]
                begin=int(pd.Timestamp(f'{year}-01-01').value//10**9);finish=int(pd.Timestamp(f'{year+1}-01-01').value//10**9)
                changes=collections.defaultdict(list);changes[begin]=[];changes[finish]=[]
                excluded=[]
                for r in rows:
                    if not r['positive_duration']:excluded.append(r['row_id']);continue
                    left=max(begin,r['start_seconds']);right=min(finish,r['end_seconds'])
                    if left>=right:excluded.append(r['row_id']);continue
                    mask=0
                    for domain in r[scope+'_domains']:mask|=masks[domain]
                    changes[left].append((r['row_id'],mask,1));changes[right].append((r['row_id'],mask,-1))
                active={};masses=collections.Counter();incident_masses=collections.Counter();previous=begin;local=[]
                for stamp,updates in sorted(changes.items()):
                    if stamp>previous:
                        mask=0
                        for x in active.values():mask|=x
                        seconds=stamp-previous;masses[mask]+=seconds
                        if active:incident_masses[mask]+=seconds
                        if active:
                            local.append({'population':population,'scope':scope,'year':year,'start_seconds':previous,'end_seconds':stamp,'mass':seconds,'failed_domains':cover(mask),'source_rows':sorted(active),'higher_than_paired':mask not in paired})
                    for row_id,mask,sign in updates:
                        if sign>0:active[row_id]=mask
                        else:active.pop(row_id,None)
                    previous=stamp
                assert sum(masses.values())==finish-begin
                for conditioning,source in [('calendar',masses),('report_active',incident_masses)]:
                    key=f'{population}/{scope}/{year}/{conditioning}'
                    assert source
                    values=[{'mass':v,'domains':cover(k)} for k,v in sorted(source.items())]
                    distributions[key]=values
                    total=sum(source.values());higher=sum(v for k,v in source.items() if k not in paired)
                    stats.append({'population':population,'scope':scope,'year':year,'conditioning':conditioning,'source_rows':len(rows),'invalid_duration_rows':excluded,'unique_failed_regions':len(values),'total_seconds':total,'higher_order_seconds':higher,'higher_order_fraction':higher/total,'maximum_minimal_domain_cover':max(len(cover(k)) for k in source),'ambiguous_rows':sum(r['mapping']=='ambiguous_scope' for r in rows)})
                intervals.extend(local)
    package={'schema':'cua-availability-footprints-v1','source_doi':'10.5281/zenodo.14712442','interpretation':'Reported service-degradation footprint replay; not observed destruction or complete physical endpoint failure','source_rows':len(audit),'positive_duration_rows':sum(r['positive_duration'] for r in audit),'storage_service_rows':sum(r['storage_service_report'] for r in audit),'hierarchy':{'labels':labels,'parent':parent,'leaf_ids':leaves},'distributions':distributions,'statistics':stats,'probability_rule':'Empirical durations, without published synthetic severity; overlapping reports use union, not summed probability','scope_rule':'Unresolved location maps to no identified region or the entire provider, with unchanged time mass','temporal_rule':'Retain published structured timestamps; nonpositive-duration rows remain in audit but cannot carry duration mass','chronological_split':{'train':2018,'validation':2019,'test':2020}}
    (out/'cua_replay.json').write_text(json.dumps(package,indent=2),encoding='utf-8')
    (out/'cua_interval_provenance.json').write_text(json.dumps(intervals,indent=2),encoding='utf-8')
    print(json.dumps({k:package[k] for k in ('source_rows','positive_duration_rows','storage_service_rows')}))
    print(json.dumps(stats,indent=2))

if __name__=='__main__':main()
