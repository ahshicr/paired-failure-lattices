"""Materialize real ETL output images and restore them under every leaf failure set.

This validates manifest-based recovery semantics with actual archive I/O. It is
not a cloud performance measurement. The three fragments are deterministic CUA
service-count summaries; costs and work-saving weights are illustrative units.
"""
import argparse,collections,hashlib,itertools,json,pathlib,subprocess,sys,zipfile
ROOT=pathlib.Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'experiments'))
from instance_io import write_input,enumerate_optimum
from scope_solver import solve,evaluate_scope,enumerate_scope
from manifest_tree import from_manifests

def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary');parser.add_argument('--output');args=parser.parse_args()
    binary=pathlib.Path(args.binary).resolve() if args.binary else ROOT/'experiments'/('revision_benchmark.exe' if sys.platform=='win32' else 'bin/revision_benchmark')
    out=ROOT/'results/manifest_demonstrator';out.mkdir(parents=True,exist_ok=True)
    if args.output:out=pathlib.Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
    audit=json.loads((ROOT/'data/processed/cua_row_audit.json').read_text());payload={}
    for name,vendor in [('f1.json','AWS'),('f2.json','Azure'),('f3.json','GCP')]:
        counts=collections.Counter(r['service'] for r in audit if r['vendor']==vendor)
        payload[name]=(json.dumps({'vendor':vendor,'source_doi':'10.5281/zenodo.14712442','operator_report_rows':sum(counts.values()),'counts_by_service':dict(sorted(counts.items()))},indent=2)+'\n').encode()
    source=out/'etl_outputs';source.mkdir(exist_ok=True)
    for name,data in payload.items():(source/name).write_bytes(data)
    expected={name:hashlib.sha256(data).hexdigest() for name,data in payload.items()}
    # root -> three zones -> two independent access endpoints per zone.
    domain_parent=[-1,0,0,0,1,1,2,2,3,3]
    events=[{'mass':p,'domains':d} for p,d in [(40,[]),(20,[1]),(15,[2]),(10,[3]),(10,[1,2]),(3,[1,3]),(2,[2,3])]]
    menu1=[{'cost':0,'replicas':[]},{'cost':2,'replicas':[4]},{'cost':4,'replicas':[4,6]},{'cost':6,'replicas':[4,6,8]}]
    menu2=[{'cost':0,'replicas':[]},{'cost':1,'replicas':[8]},{'cost':2,'replicas':[4,8]},{'cost':3,'replicas':[4,6,8]}]
    cases=[]
    for name,scopes in [('nested',[[0,1],[2]]),('overlapping',[[0,1],[1,2]])]:
        a={'domain_parent':domain_parent,'benefit':[8,5,3],'menus':[menu1,menu2],'events':events,'budget':5,'scopes':scopes}
        result=solve(a);exact,_,feasible=enumerate_scope(a);assert result['status']=='OPTIMAL' and int(result['objective_numerator'])==exact
        tree_validation=None
        if name=='nested':
            assert exact==1425
            tree,tree_validation=from_manifests(a);tree_input=out/'nested_tree.txt';write_input(tree_input,tree)
            native=subprocess.run([str(binary),str(tree_input),'paired_dense'],capture_output=True,text=True,check=True)
            tree_result=json.loads(native.stdout);assert int(tree_result['objective_numerator'])==exact
            tree_validation['native_objective_numerator']=tree_result['objective_numerator']
        else:
            try:from_manifests(a)
            except ValueError:tree_validation={'nonlaminar_tree_conversion_rejected':True}
            else:raise AssertionError('Overlapping manifests must not silently become a tree')
        selected=result['selection'];replicas={};manifest=[]
        base=out/name;base.mkdir(exist_ok=True)
        for image_id,option_id in enumerate(selected):
            option=a['menus'][image_id][option_id];members=[f'f{i+1}.json' for i in scopes[image_id]]
            for leaf in option['replicas']:
                path=base/'replicas'/f'endpoint_{leaf}'/f'image_{image_id}.zip';path.parent.mkdir(parents=True,exist_ok=True)
                with zipfile.ZipFile(path,'w',compression=zipfile.ZIP_STORED) as archive:
                    for member in members:
                        info=zipfile.ZipInfo(member,date_time=(1980,1,1,0,0,0));archive.writestr(info,payload[member])
                replicas[(image_id,leaf)]=path
                manifest.append({'image':image_id,'endpoint':leaf,'members':members,'archive_relative':path.relative_to(out).as_posix(),'archive_sha256':sha(path),'member_sha256':{m:expected[m] for m in members}})
        observations=[]
        for fail in range(64):
            failed={4+i for i in range(6) if fail&(1<<i)};restored={};attempts=0;blocked=0
            destination=base/'restored'/f'failure_{fail:02d}';destination.mkdir(parents=True,exist_ok=True)
            for (image_id,leaf),path in sorted(replicas.items()):
                attempts+=1
                # Access fault injection: unavailable endpoints refuse reads.
                if leaf in failed:blocked+=1;continue
                with zipfile.ZipFile(path) as archive:
                    for member in archive.namelist():
                        data=archive.read(member);assert hashlib.sha256(data).hexdigest()==expected[member]
                        (destination/member).write_bytes(data);restored[member]=sha(destination/member)
            predicted=set()
            for image_id,option_id in enumerate(selected):
                if any(leaf not in failed for leaf in a['menus'][image_id][option_id]['replicas']):predicted.update(f'f{i+1}.json' for i in scopes[image_id])
            assert set(restored)==predicted and all(expected[m]==h for m,h in restored.items())
            observations.append({'failure_leaf_mask':fail,'failed_endpoints':sorted(failed),'actual_restored_sha256':restored,'manifest_predicted_fragments':sorted(predicted),'archive_access_attempts':attempts,'refused_reads':blocked,'benefit_counted_once':sum(a['benefit'][int(m[1])-1] for m in restored),'agreement':True})
        (base/'input.json').write_text(json.dumps(a,indent=2));(base/'archive_manifest.json').write_text(json.dumps(manifest,indent=2));(base/'restoration_records.json').write_text(json.dumps(observations,indent=2))
        cases.append({'case':name,'scope_incidence':scopes,'solver':result,'tree_validation':tree_validation,'enumerated_feasible_deployments':feasible,'tested_failure_subsets':len(observations),'actual_archive_count':len(manifest),'payload_hashes':expected,'hash_verified_restorations':sum(len(r['actual_restored_sha256']) for r in observations)})
    report={'status':'PASS','source_rows':len(audit),'source_doi':'10.5281/zenodo.14712442','binary_sha256':sha(binary),'cost_and_benefit_units':'illustrative, not measured cloud prices or runtime','failure_model':'injected endpoint read refusal, all 64 subsets of six endpoints','cases':cases}
    (out/'demonstrator_report.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='cases'}));print([(c['case'],c['solver']['objective_numerator'],c['hash_verified_restorations']) for c in cases])

if __name__=='__main__':main()
