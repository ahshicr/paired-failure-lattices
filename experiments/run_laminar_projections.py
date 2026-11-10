"""Full three-policy restriction study on all 2,400 recorded scope instances."""
import argparse,json,pathlib,shutil,sys
from run_matrix import digest
from study_runner import execute,summary

def main():
    p=argparse.ArgumentParser();p.add_argument('--scope-source',required=True);p.add_argument('--workers',type=int,default=1);p.add_argument('--binary',default='bin/revision_benchmark');p.add_argument('--output',default='results/laminar_projections_v1');p.add_argument('--expected',type=int,default=2400);args=p.parse_args()
    source=pathlib.Path(args.scope_source);out=pathlib.Path(args.output).resolve();(out/'inputs').mkdir(parents=True,exist_ok=True);(out/'tree_inputs').mkdir(exist_ok=True);tasks=[];binary=str(pathlib.Path(args.binary).resolve())
    config={'policies':['small_first','large_first','benefit_first'],'source_summary_sha256':digest(source/'summary.csv'),'expected_cases':args.expected,'wall_seconds_per_task':300,'address_space_bytes_per_task':5*2**30,'task_order_seed':6110951}
    paths=sorted((source/'records').glob('*__scope_integer.json'));assert len(paths)==args.expected
    for path in paths:
        r=json.loads(path.read_text());original=source/'inputs'/pathlib.Path(r['input']).name;assert digest(original)==r['input_sha256'];target=out/'inputs'/original.name;shutil.copyfile(original,target)
        for policy in config['policies']:
            ident=r['case_id']+'__'+policy;tree=out/'tree_inputs'/(ident+'.txt')
            fields={k:r[k] for k in ('case_id','nodes','budget','overlap_fraction','actual_nonlaminar','higher_order','replicate','seed')}
            tasks.append({**fields,'study':'laminar_projection','task_id':ident,'method':policy,'method_label':policy,'input':str(target),'input_sha256':r['input_sha256'],'binary':sys.executable,'binary_sha256':digest(sys.executable),'native_binary_sha256':digest(binary),'record':str(out/'records'/(ident+'.json')),'wall_seconds':config['wall_seconds_per_task'],'address_space_bytes':config['address_space_bytes_per_task'],'scope_record_sha256':digest(path),'command':[sys.executable,str(pathlib.Path('laminar_projection.py').resolve()),str(target),policy,'--binary',binary,'--tree-output',str(tree)]})
    execute(tasks,out,config,args.workers);print(json.dumps(summary(tasks,out)))

if __name__=='__main__':main()
