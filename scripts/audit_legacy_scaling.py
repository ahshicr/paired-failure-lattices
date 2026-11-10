"""Recheck and preserve the submitted full strong-scaling measurements."""
import hashlib,json,pathlib,shutil,sys
ROOT=pathlib.Path(__file__).resolve().parents[1];PROJECT=ROOT.parent

def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def main():
    original=PROJECT/'sub20260722/reproducibility/paper2_reproducibility_source'
    base=PROJECT/'related_artifacts/hpc_fetched/p2_scaling_mem96_combined_20260719/results'
    out=ROOT/'legacy_scaling';out.mkdir(exist_ok=True)
    if original.exists():
        shutil.copytree(original/'scripts',out/'scripts',dirs_exist_ok=True)
        shutil.copyfile(original/'source/scripts/progress_heartbeat.py',out/'scripts/progress_heartbeat.py')
        shutil.copytree(original/'configs',out/'configs',dirs_exist_ok=True)
        for rel in ('apps/experiment_runner.cpp','src/pfl.cpp','include/pfl/pfl.hpp'):
            target=out/'source'/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(original/'source'/rel,target)
    sys.path.insert(0,str(out/'scripts'));from analyze_scaling_contract import parse_gnu_time,canonical_projection,read_records
    if not base.exists():base=out/'results'
    baseline=None;rows=[];equal=0
    for threads in (1,2,4,8,16,32):
        condition=base/f'scaling_threads_{threads}';att=json.loads((condition/'run_attestation.json').read_text());assert att['status']=='COMPLETE'
        assert sha(condition/'raw_records.jsonl')==att['raw_records_sha256']
        assert sha(condition/'input_config.json')==att['config_sha256']
        assert sha(condition/'rlimit_as_attestation.json')==att['rlimit_as_attestation_sha256']
        timepath=base/att['attempt_relative_path']/'gnu_time.txt';assert sha(timepath)==att['time_output_sha256']
        records=read_records(condition/'raw_records.jsonl');projection=[canonical_projection(r) for r in records]
        if baseline is None:baseline=projection
        else:assert baseline==projection;equal+=len(records)
        metrics=parse_gnu_time(timepath);rows.append({'threads':threads,**metrics,'peak_rss_gib':metrics['maximum_rss_kbytes']/2**20,'config_sha256':att['config_sha256'],'raw_records_sha256':att['raw_records_sha256'],'recorded_binary_sha256':att['native_runner_sha256']})
        target=out/'results'/condition.name
        if target.resolve()!=condition.resolve():shutil.copytree(condition,target,dirs_exist_ok=True)
        target=out/'results'/att['attempt_relative_path'];target.mkdir(parents=True,exist_ok=True)
        if (target/'gnu_time.txt').resolve()!=timepath.resolve():shutil.copyfile(timepath,target/'gnu_time.txt')
    for row in rows:row['speedup']=rows[0]['wall_seconds']/row['wall_seconds'];row['parallel_efficiency']=row['speedup']/row['threads']
    report={'status':'PASS','provenance':'Retained submitted experiment; re-audited, not rerun for this revision. Instance-parallel full runner includes compiler, queries, optimizer, and auxiliary baselines.','raw_instances':192,'cross_thread_scientific_equalities':equal,'rows':rows}
    (ROOT/'results/analysis/legacy_scaling_statistics.json').write_text(json.dumps(report,indent=2));(out/'AUDIT.json').write_text(json.dumps(report,indent=2))
    (out/'SHA256_MANIFEST.json').write_text(json.dumps({f.relative_to(out).as_posix():sha(f) for f in sorted(out.rglob('*')) if f.is_file() and f.name!='SHA256_MANIFEST.json' and '__pycache__' not in f.parts},indent=2));print(json.dumps({'status':'PASS','records':192,'equalities':equal,'32_thread_speedup':rows[-1]['speedup']}))

if __name__=='__main__':main()
