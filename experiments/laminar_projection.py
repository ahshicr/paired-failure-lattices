"""Optimize a laminar subset of actual manifests without changing any image."""
import argparse,copy,json,pathlib,subprocess,time
from manifest_tree import from_manifests
from scope_solver import evaluate_scope

def restrict(a,policy):
    sets=[set(s) for s in a['scopes']]
    if policy=='small_first':key=lambda i:(len(sets[i]),i)
    elif policy=='large_first':key=lambda i:(-len(sets[i]),i)
    elif policy=='benefit_first':key=lambda i:(-sum(a['benefit'][v] for v in sets[i]),i)
    else:raise ValueError(policy)
    keep=[]
    for i in sorted(range(len(sets)),key=key):
        if all(not(sets[i]&sets[j]) or sets[i]<=sets[j] or sets[j]<=sets[i] for j in keep):keep.append(i)
    b=copy.deepcopy(a);b['scopes']=[a['scopes'][i] for i in keep];b['menus']=[a['menus'][i] for i in keep]
    tree,mapping=from_manifests(b)
    return tree,mapping,keep

def main():
    from instance_io import write_input
    p=argparse.ArgumentParser();p.add_argument('input');p.add_argument('policy');p.add_argument('--binary',required=True);p.add_argument('--tree-output',required=True);args=p.parse_args()
    start=time.perf_counter();a=json.loads(pathlib.Path(args.input).read_text());tree,mapping,keep=restrict(a,args.policy);write_input(args.tree_output,tree)
    process=subprocess.run([args.binary,args.tree_output,'scenario'],capture_output=True,text=True,check=True);r=json.loads(process.stdout)
    selected=[0]*len(a['menus'])
    for reduced,node in mapping['image_nodes'].items():selected[keep[reduced]]=r['selection'][node]
    elapsed=time.perf_counter()-start;value,cost=evaluate_scope(a,selected)
    assert str(value)==r['objective_numerator'] and cost<=a['budget']
    print(json.dumps({'status':'COMPLETE','objective_numerator':str(value),'probability_denominator':r['probability_denominator'],'cost':cost,'selection':selected,'retained_images':len(keep),'original_images':len(a['menus']),'retained_image_ids':keep,'end_to_end_seconds':elapsed,'tree_result':r}))

if __name__=='__main__':main()
