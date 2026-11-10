"""Complete representation and one-factor ablation studies with terminal evidence."""
import argparse,concurrent.futures,itertools,json,pathlib,random,os,platform,sys,time
from collections import Counter,defaultdict
from instance_io import generate,write_input
from run_matrix import run_task,digest,atomic_json

def main():
    p=argparse.ArgumentParser();p.add_argument('--matrix',default='configs/supplemental.json');p.add_argument('--output',default='results/supplemental_v1');p.add_argument('--workers',type=int,default=1);a=p.parse_args()
    config=json.loads(pathlib.Path(a.matrix).read_text());out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);(out/'records').mkdir(exist_ok=True)
    tasks=[];matrix_sha=digest(a.matrix)
    def add(case,instance,methods,binary,extra_args=[]):
        path=out/'inputs'/(case['case_id']+'.txt');sha=write_input(path,instance);binary=str(pathlib.Path(binary).resolve())
        for method,variant in methods:
            label=method+('+'+'+'.join(variant) if variant else '')
            ident=case['case_id']+'__'+label
            tasks.append({**case,'task_id':ident,'method':method,'method_label':label,'variant':variant+extra_args,'binary':binary,'binary_sha256':digest(binary),'input':str(path),'input_sha256':sha,'matrix_sha256':matrix_sha,'record':str(out/'records'/(ident+'.json')),'wall_seconds':config['wall_seconds_per_task'],'address_space_bytes':config['address_space_bytes_per_task']})
    c=config['representation']
    for leaves,topology,events,rep in itertools.product(c['leaves'],c['topologies'],c['event_counts'],range(c['replicates'])):
        seed=config['seed_start']+rep;ident=f'rep_L{leaves}_T{topology}_Q{events}_S{rep:02d}'
        x=generate(leaves=leaves,nodes=2,budget=0,events=events,topology=topology,seed=seed)
        add({'study':'representation','case_id':ident,'leaves':leaves,'nodes':2,'budget':0,'replicas':0,'topology':topology,'replicate':rep,'events':events},x,[(m,[]) for m in c['methods']],'bin/representation_benchmark',[str(c['query_count'])])
    c=config['ablations']
    for leaves,nodes,budget,duplicate,rep in itertools.product(c['leaves'],c['nodes'],c['budgets'],c['duplicate_fractions'],range(c['replicates'])):
        topology=['balanced','star','random'][rep%3];ident=f'ab_L{leaves}_N{nodes}_B{budget}_D{duplicate}_S{rep:02d}'
        x=generate(leaves=leaves,nodes=nodes,budget=64,replicas=2,options=8,events=256,topology=topology,seed=config['seed_start']+rep,duplicate_fraction=duplicate);x['budget']=budget
        add({'study':'ablations','case_id':ident,'leaves':leaves,'nodes':nodes,'budget':budget,'replicas':2,'topology':topology,'replicate':rep,'duplicate_fraction':duplicate},x,c['methods'],'bin/revision_benchmark')
    random.Random(config['task_order_seed']).shuffle(tasks)
    atomic_json(out/'run_manifest.json',{'config':config,'matrix_sha256':matrix_sha,'tasks':len(tasks),'workers':a.workers,'host':platform.node(),'python':sys.version,'slurm_job_id':os.environ.get('SLURM_JOB_ID'),'task_order':[t['task_id'] for t in tasks]})
    counts=Counter();print(json.dumps({'tasks':len(tasks),'workers':a.workers}),flush=True)
    with concurrent.futures.ProcessPoolExecutor(max_workers=a.workers) as pool:
        fs=[pool.submit(run_task,t) for t in tasks]
        for i,f in enumerate(concurrent.futures.as_completed(fs),1):
            r=f.result();counts[r['status']]+=1
            if i%50==0 or i==len(tasks):
                prog={'terminal':i,'total':len(tasks),'statuses':dict(counts),'updated_unix':time.time()};atomic_json(out/'progress.json',prog);print(json.dumps(prog),flush=True)
    import csv
    rows=[];values=defaultdict(set);status=Counter()
    for task in tasks:
        r=json.loads(pathlib.Path(task['record']).read_text());status[r['status']]+=1
        row={k:v for k,v in task.items() if k not in ('record','binary','input','variant')};row.update({k:r[k] for k in ('status','elapsed_seconds','observed_process_tree_peak_rss_bytes')});row.update({k:v for k,v in r.get('result',{}).items() if k not in ('method','status','selection')});rows.append(row)
        if r['status']=='COMPLETE':values[r['case_id']].add(int(r['result']['objective_numerator']))
    bad={k:list(v) for k,v in values.items() if len(v)>1}
    fields=list(dict.fromkeys(k for r in rows for k in r))
    with (out/'summary.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
    audit={'expected_tasks':len(tasks),'terminal_tasks':len(rows),'statuses':dict(status),'exact_objective_mismatches':bad,'scientific_equality_pass':not bad,'summary_sha256':digest(out/'summary.csv')};atomic_json(out/'analysis.json',audit)
    if bad or status['ERROR'] or status['PROCESS_FAILURE']:raise RuntimeError('scientific or execution errors; inspect records')
    atomic_json(out/'complete.json',audit);print(json.dumps(audit),flush=True)

if __name__=='__main__':main()
