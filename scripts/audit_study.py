"""Check every terminal input and output, without counting censored tasks as wins."""
import argparse,collections,hashlib,json,pathlib

def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def audit(base,expected):
    base=pathlib.Path(base);marker=json.loads((base/'complete.json').read_text());assert sha(base/'summary.csv')==marker['summary_sha256'];inputs={};outputs=[];counts=collections.Counter();records=[]
    for path in sorted((base/'records').glob('*.json')):
        if path.name.endswith('.start.json'):continue
        r=json.loads(path.read_text());name=pathlib.Path(r['input']).name
        if name not in inputs:inputs[name]=sha(base/'inputs'/name)
        assert inputs[name]==r['input_sha256'],path.name
        for stream in ('stdout','stderr'):assert sha(path.parent/r[stream])==r[stream+'_sha256'],path.name
        if r['status'] in ('COMPLETE','OPTIMAL','FEASIBLE'):assert json.loads((path.parent/r['stdout']).read_text())==r['result'],path.name
        outputs.append(r['stdout']);counts[r['status']]+=1;records.append(r)
    assert len(records)==len(set(outputs))==expected==marker['expected_tasks'];assert dict(counts)==marker['statuses'];assert not marker['scientific_mismatches']
    report={'evidence_verified':True,'records':len(records),'unique_outputs':len(set(outputs)),'unique_input_files':len(inputs),'statuses':dict(counts),'summary_sha256':sha(base/'summary.csv')};(base/'RAW_AUDIT.json').write_text(json.dumps(report,indent=2));return report,records

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('base');p.add_argument('expected',type=int);a=p.parse_args();print(json.dumps(audit(a.base,a.expected)[0]))
