"""Recreate every used public conversion from verified source JSONs."""
import hashlib,json,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    out=ROOT/'regenerated/workflow_conversions';out.mkdir(parents=True,exist_ok=True);count=0;checks=[]
    for mapping in ('deepest_parent','shallowest_parent','dominator'):
        destination=out/mapping
        subprocess.run([sys.executable,str(ROOT/'legacy_scaling/scripts/convert_wfinstances.py'),'--input-root',str(ROOT/'data/raw/wfinstances/sources'),'--archive',str(ROOT/'data/raw/wfinstances/WfInstances-v1.5.zip'),'--containment-mapping',mapping,'--output-dir',str(destination)],check=True,capture_output=True)
        old=json.loads((ROOT/'experiments/data/public_workflows'/mapping/'conversion_manifest.json').read_text());new=json.loads((destination/'conversion_manifest.json').read_text());assert old['converted_count']==new['converted_count']==180 and new['excluded_count']==0
        for record in old['records']:
            file=destination/record['output_relative'];actual=hashlib.sha256(file.read_bytes()).hexdigest();assert actual==record['output_sha256'],(mapping,record['source_relative']);count+=1
        checks.append({'mapping':mapping,'converted_and_hash_verified':180})
    report={'status':'PASS','total_verified_conversions':count,'checks':checks};(ROOT/'evidence/workflow_conversion_reproduction.json').write_text(json.dumps(report,indent=2));print(json.dumps(report))

if __name__=='__main__':main()
