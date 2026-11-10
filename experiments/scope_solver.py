"""Independent exact integer coverage formulation for arbitrary image manifests."""
import argparse,itertools,json,math,pathlib,time,sys
from instance_io import leaf_masks,sum_masks

def tree_scopes(parent):
    scopes=[[] for _ in parent]
    for atom in range(len(parent)):
        v=atom
        while v>=0:scopes[v].append(atom);v=parent[v]
    return scopes

def evaluate_scope(a,selected):
    masks,_=leaf_masks(a['domain_parent']);score=cost=0
    for i,o in enumerate(selected):cost+=a['menus'][i][o]['cost']
    for event in a['events']:
        failed=sum_masks(masks,event['domains']);saved=set()
        for i,o in enumerate(selected):
            if sum_masks(masks,a['menus'][i][o]['replicas'])&~failed:saved.update(a['scopes'][i])
        score+=event['mass']*sum(a['benefit'][v] for v in saved)
    return score,cost

def enumerate_scope(a):
    best=-1;selection=None;count=0
    for chosen in itertools.product(*(range(len(m)) for m in a['menus'])):
        if sum(a['menus'][i][o]['cost'] for i,o in enumerate(chosen))>a['budget']:continue
        value,cost=evaluate_scope(a,chosen);count+=1
        if value>best:best,selection=value,list(chosen)
    return best,selection,count

def solve(a,seconds=60,seed=61109):
    from ortools.sat.python import cp_model
    begin=time.perf_counter();model=cp_model.CpModel();masks,_=leaf_masks(a['domain_parent'])
    x=[];costs=[];survivals=[]
    for i,menu in enumerate(a['menus']):
        row=[]
        for o,option in enumerate(menu):
            variable=model.NewBoolVar(f'x{i}_{o}');row.append(variable);costs.append(option['cost']*variable)
            if option['cost']>a['budget']:model.Add(variable==0)
        model.AddExactlyOne(row);x.append(row)
    model.Add(sum(costs)<=a['budget']);aggregated={}
    for event in a['events']:
        mask=sum_masks(masks,event['domains']);aggregated[mask]=aggregated.get(mask,0)+event['mass']
    terms=[];ys=0;coverage_cache={}
    for j,(failed,mass) in enumerate(sorted(aggregated.items())):
        covering=[[] for _ in a['benefit']]
        for i,menu in enumerate(a['menus']):
            for o,option in enumerate(menu):
                if sum_masks(masks,option['replicas'])&~failed:
                    for atom in a['scopes'][i]:covering[atom].append((i,o))
        # Identical surviving image sets share one Boolean coverage variable.
        for atom,indices in enumerate(covering):
            if not indices or not a['benefit'][atom]:continue
            key=tuple(indices)
            if key not in coverage_cache:
                y=model.NewBoolVar(f'y{ys}');ys+=1;model.Add(y<=sum(x[i][o] for i,o in indices));coverage_cache[key]=y
            terms.append(mass*a['benefit'][atom]*coverage_cache[key])
    model.Maximize(sum(terms));pre=time.perf_counter()-begin
    solver=cp_model.CpSolver();solver.parameters.num_search_workers=1;solver.parameters.max_time_in_seconds=seconds;solver.parameters.random_seed=seed
    solve_start=time.perf_counter();status=solver.Solve(model);elapsed=time.perf_counter()-solve_start
    rss=0
    if sys.platform=='linux':
        import resource
        rss=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024
    result={'status':solver.StatusName(status),'preprocessing_seconds':pre,'solve_seconds':elapsed,'end_to_end_seconds':time.perf_counter()-begin,'peak_rss_bytes':rss,'binary_image_variables':sum(len(r) for r in x),'coverage_variables':ys,'deduplicated_events':len(aggregated),'best_bound':solver.BestObjectiveBound(),'branches':solver.NumBranches(),'conflicts':solver.NumConflicts(),'probability_denominator':sum(e['mass'] for e in a['events'])}
    if status in (cp_model.OPTIMAL,cp_model.FEASIBLE):
        selected=[next(o for o,v in enumerate(row) if solver.Value(v)) for row in x]
        objective,cost=evaluate_scope(a,selected)
        assert cost<=a['budget']
        assert abs(objective-solver.ObjectiveValue())<0.1
        result.update(selection=selected,objective_numerator=str(objective),cost=cost,relative_bound_gap=max(0,(solver.BestObjectiveBound()-objective)/max(1,abs(objective))))
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('--seconds',type=float,default=60);p.add_argument('--seed',type=int,default=61109);a=p.parse_args()
    print(json.dumps(solve(json.loads(pathlib.Path(a.input).read_text()),a.seconds,a.seed)))

if __name__=='__main__':main()
