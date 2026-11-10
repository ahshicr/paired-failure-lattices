"""Chronological real-report replay and full arbitrary-scope solver matrix."""
import argparse,collections,copy,functools,hashlib,itertools,json,math,pathlib,random,sys
from instance_io import generate,leaf_masks,sum_masks,write_input,evaluate
from scope_solver import tree_scopes
from run_matrix import digest,atomic_json
from study_runner import execute,summary

def single_envelope(events,parent):
    masks,_=leaf_masks(parent);order=sorted(range(len(masks)),key=lambda i:(masks[i].bit_count(),i));masses=collections.Counter()
    for e in events:
        mask=sum_masks(masks,e['domains']);key=() if mask==0 else (next(i for i in order if mask&~masks[i]==0),);masses[key]+=e['mass']
    return [{'domains':list(k),'mass':v} for k,v in sorted(masses.items())]

def independent_events(events,parent,samples,seed):
    masks,leaves=leaf_masks(parent);total=sum(e['mass'] for e in events);fail=[sum_masks(masks,e['domains']) for e in events]
    marginals=[sum(e['mass'] for e,f in zip(events,fail) if f&masks[leaf])/total for leaf in leaves]
    rng=random.Random(seed);counts=collections.Counter()
    for _ in range(samples):counts[tuple(leaf for leaf,p in zip(leaves,marginals) if rng.random()<p)]+=1
    return [{'domains':list(k),'mass':v} for k,v in sorted(counts.items())]

def failure_aggregate_menus(x):
    """Menu-restricted adaptation of Mills et al.'s lexicographic failure aggregate.

    Among equal-cost options keep the option whose vector counts the fewest
    domains losing k replicas, in descending k. The common exact recovery DP
    then allocates the budget. This is not their unrestricted placement solver.
    """
    masks,_=leaf_masks(x['domain_parent']);menus=[]
    max_copies=max(len(o['replicas']) for row in x['menus'] for o in row)
    for row in x['menus']:
        groups={}
        for index,o in enumerate(row):
            r=sum_masks(masks,o['replicas']);counts=collections.Counter((m&r).bit_count() for m in masks);vector=tuple(counts[k] for k in range(max_copies,0,-1))
            key=(vector,index)
            if o['cost'] not in groups or key<groups[o['cost']][0]:groups[o['cost']]=(key,o)
        menus.append([copy.deepcopy(groups[c][1]) for c in sorted(groups)])
        assert menus[-1][0]['cost']==0 and not menus[-1][0]['replicas']
    return menus

def main():
    p=argparse.ArgumentParser();p.add_argument('--matrix',default='configs/field_studies.json');p.add_argument('--output',default='results/field_studies_v1');p.add_argument('--binary',default='bin/revision_benchmark');p.add_argument('--workers',type=int,default=1);p.add_argument('--study',choices=('real_cloud','scopes'),required=True);args=p.parse_args()
    config=json.loads(pathlib.Path(args.matrix).read_text());out=pathlib.Path(args.output).resolve()/args.study;out.mkdir(parents=True,exist_ok=True);(out/'records').mkdir(exist_ok=True)
    binary=str(pathlib.Path(args.binary).resolve());bsha=digest(binary);csha=digest(args.matrix);tasks=[];truth={}
    def add(case,x,label,method,*,scope_input=False):
        ident=case['case_id']+'__'+label;path=out/'inputs'/(ident+('.json' if scope_input else '.txt'))
        if scope_input:path.parent.mkdir(exist_ok=True);path.write_text(json.dumps(x));sha=digest(path)
        else:sha=write_input(path,x)
        task={**case,'task_id':ident,'method':method,'method_label':label,'binary':binary,'binary_sha256':bsha,'input':str(path),'input_sha256':sha,'matrix_sha256':csha,'record':str(out/'records'/(ident+'.json')),'wall_seconds':config['wall_seconds_per_task'],'address_space_bytes':config['address_space_bytes_per_task']}
        if scope_input:
            task.update(binary=sys.executable,binary_sha256=digest(sys.executable),solver_source_sha256=digest('scope_solver.py'),command=[sys.executable,str(pathlib.Path('scope_solver.py').resolve()),str(path),'--seconds',str(config['scopes']['solver_seconds'])])
        tasks.append(task)
    if args.study=='real_cloud':
        c=config['real_cloud'];trace=json.loads(pathlib.Path('data/cua_replay.json').read_text());hier=trace['hierarchy'];actual_parent=hier['parent'];actual_leaves=hier['leaf_ids'];event_cache={}
        for population,scope,conditioning,nodes,budget,replicas,rep in itertools.product(c['populations'],c['scope_interpretations'],c['conditionings'],c['nodes'],c['budgets'],c['replicas'],range(c['replicates'])):
            seed=config['seed_start']+rep;prefix=f'{population}/{scope}';fit=trace['distributions'][f'{prefix}/{c["fit_year"]}/{conditioning}'];years={str(y):trace['distributions'][f'{prefix}/{y}/{conditioning}'] for y in (2018,2019,2020)}
            x=generate(leaves=len(actual_leaves),nodes=nodes,budget=64,replicas=replicas,options=5,seed=seed,events=0);_,old=leaf_masks(x['domain_parent']);translate=dict(zip(old,actual_leaves))
            for row in x['menus']:
                for o in row:o['replicas']=[translate[r] for r in o['replicas']]
            x.update(domain_parent=actual_parent,events=fit,budget=budget)
            ident=f'trace_{population}_{scope}_{conditioning}_N{nodes}_B{budget}_R{replicas}_S{rep:02d}'
            case={'case_id':ident,'study':'real_cloud','population':population,'scope_interpretation':scope,'conditioning':conditioning,'nodes':nodes,'budget':budget,'replicas':replicas,'replicate':rep,'seed':seed,'data_sha256':digest('data/cua_replay.json')}
            truth_path=out/'truth'/(ident+'.json');truth_path.parent.mkdir(exist_ok=True);truth_path.write_text(json.dumps({'instance':x,'years':years}));truth[ident]=truth_path
            for model in c['methods']:
                y=copy.deepcopy(x);method='scenario'
                if model=='greedy':method='greedy'
                elif model=='paired_envelope':method='paired_envelope'
                elif model=='single_domain_envelope':y['events']=single_envelope(fit,actual_parent)
                elif model=='independent_marginals':
                    cache_key=(population,scope,conditioning,rep)
                    if cache_key not in event_cache:event_cache[cache_key]=independent_events(fit,actual_parent,c['independent_samples'],seed+431000)
                    y['events']=event_cache[cache_key]
                elif model=='failure_aggregate':y['menus']=failure_aggregate_menus(x)
                elif model=='test_oracle':y['events']=years[str(c['test_year'])]
                add(case,y,model,method)
                if model=='failure_aggregate':tasks[-1]['restricted_menus']=y['menus']
    else:
        c=config['scopes']
        for nodes,budget,overlap,higher,rep in itertools.product(c['nodes'],c['budgets'],c['overlap_fractions'],c['higher_order'],range(c['replicates'])):
            seed=config['seed_start']+rep;rng=random.Random(seed+5712)
            x=generate(leaves=c['domain_leaves'],nodes=nodes,budget=64,replicas=2,options=c['menu_options'],events=c['event_identities'],seed=seed,higher_order=higher)
            x['budget']=budget;x['scopes']=tree_scopes(x['parent'])
            for i,s in enumerate(x['scopes']):
                if i and rng.random()<overlap:
                    outside=list(set(range(nodes))-set(s))
                    if outside:s.extend(rng.sample(outside,min(len(outside),max(1,len(s)//2))));s.sort()
            for i,row in enumerate(x['menus']):
                for o in row:
                    if o['replicas']:o['cost']=1+math.ceil(64*len(x['scopes'][i])*len(o['replicas'])/nodes)
            actual_nonlaminar=any(set(a)&set(b) and not(set(a)<=set(b) or set(b)<=set(a)) for i,a in enumerate(x['scopes']) for b in x['scopes'][i+1:])
            ident=f'scope_N{nodes}_B{budget}_O{overlap}_H{int(higher)}_S{rep:02d}';case={'case_id':ident,'study':'scopes','nodes':nodes,'budget':budget,'overlap_fraction':overlap,'actual_nonlaminar':actual_nonlaminar,'higher_order':higher,'replicate':rep,'seed':seed}
            add(case,x,'scope_integer','scope_integer',scope_input=True)
            if overlap==0:add(case,x,'tree_scenario','scenario')
    execute(tasks,out,config,args.workers)
    if args.study=='real_cloud':
        case_values=collections.defaultdict(dict);rows=[]
        # Test years are evaluated only after all fitted deployments are terminal.
        for task in tasks:
            record_path=pathlib.Path(task['record']);r=json.loads(record_path.read_text())
            if r['status']!='COMPLETE':continue
            data=json.loads(truth[r['case_id']].read_text());x=data['instance']
            if 'restricted_menus' in r:x['menus']=r['restricted_menus']
            selection=r['result']['selection'];scores={}
            for year,events in data['years'].items():
                x['events']=events;value,cost=evaluate(x,selection);assert cost<=x['budget'];scores[year]={'objective_numerator':str(value),'probability_denominator':sum(e['mass'] for e in events)}
            r['held_out_evaluation']=scores;atomic_json(record_path,r);case_values[r['case_id']][r['method_label']]=int(scores['2020']['objective_numerator']);rows.append({'case_id':r['case_id'],'method':r['method_label'],'scores':scores,'cost':r['result']['cost']})
        errors={case:vals for case,vals in case_values.items() if 'test_oracle' in vals and max(vals.values())>vals['test_oracle']};atomic_json(out/'held_out_results.json',rows);atomic_json(out/'held_out_audit.json',{'violations_of_test_oracle':errors,'completed_deployments':len(rows),'cases':len(case_values)})
        assert not errors,errors
        report=summary(tasks,out)
    else:report=summary(tasks,out,exact_methods=('scope_integer','tree_scenario'))
    print(json.dumps(report),flush=True)

if __name__=='__main__':main()
