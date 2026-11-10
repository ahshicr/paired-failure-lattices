"""Common execution and audit for predeclared native studies."""
import concurrent.futures,csv,json,os,pathlib,platform,random,sys,time
from collections import Counter,defaultdict
from run_matrix import run_task,atomic_json,digest

def execute(tasks,out,config,workers):
    out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True);(out/'records').mkdir(exist_ok=True)
    random.Random(config['task_order_seed']).shuffle(tasks)
    atomic_json(out/'run_manifest.json',{'config':config,'tasks':len(tasks),'workers':workers,'python':sys.version,'host':platform.node(),'slurm_job_id':os.environ.get('SLURM_JOB_ID'),'task_order':[x['task_id'] for x in tasks]})
    print(json.dumps({'tasks':len(tasks),'workers':workers}),flush=True);counts=Counter()
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        fs=[pool.submit(run_task,t) for t in tasks]
        for i,f in enumerate(concurrent.futures.as_completed(fs),1):
            result=f.result();counts[result['status']]+=1
            if i%40==0 or i==len(tasks):
                progress={'terminal':i,'total':len(tasks),'statuses':dict(counts),'updated_unix':time.time()};atomic_json(out/'progress.json',progress);print(json.dumps(progress),flush=True)
    return counts

def summary(tasks,out,*,exact_methods=(),greedy_method='greedy'):
    out=pathlib.Path(out);rows=[];values=defaultdict(dict);counts=Counter()
    for task in tasks:
        r=json.loads(pathlib.Path(task['record']).read_text());counts[r['status']]+=1
        row={k:v for k,v in r.items() if k not in ('result','command','variant')};row.update({k:v for k,v in r.get('result',{}).items() if k not in ('method','status','selection')});rows.append(row)
        if r['status'] in ('COMPLETE','OPTIMAL'):values[r['case_id']][r.get('method_label',r['method'])]=int(r['result']['objective_numerator'])
    bad={}
    for case,vals in values.items():
        exact=[v for m,v in vals.items() if m in exact_methods]
        if len(set(exact))>1:bad[case]={'exact_mismatch':vals}
        if exact and greedy_method in vals and vals[greedy_method]>min(exact):bad[case]={'greedy_exceeds_exact':vals}
    fields=list(dict.fromkeys(k for r in rows for k in r))
    with (out/'summary.csv').open('w',newline='',encoding='utf-8') as f:w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
    report={'expected_tasks':len(tasks),'terminal_tasks':len(rows),'statuses':dict(counts),'scientific_mismatches':bad,'all_tasks_attempted':len(tasks)==len(rows),'summary_sha256':digest(out/'summary.csv')};atomic_json(out/'analysis.json',report)
    if bad or counts['ERROR'] or counts['PROCESS_FAILURE']:raise RuntimeError('Study requires investigation; inspect analysis.json')
    atomic_json(out/'complete.json',report);return report
