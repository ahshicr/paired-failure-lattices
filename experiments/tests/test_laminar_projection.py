import pathlib,sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from instance_io import generate,enumerate_optimum
from scope_solver import enumerate_scope,tree_scopes
from laminar_projection import restrict

checks=0
for seed in range(24):
    a=generate(leaves=6,nodes=4,budget=20,replicas=2,options=3,events=8,seed=seed,higher_order=True);a['scopes']=tree_scopes(a['parent'])
    if seed%2:a['scopes'][2]=[1,2];a['scopes'][3]=[2,3]
    best,_,_=enumerate_scope(a)
    for policy in ('small_first','large_first','benefit_first'):
        tree,_,_=restrict(a,policy);result=enumerate_optimum(tree);value=result[0] if isinstance(result,tuple) else result['objective_numerator']
        assert int(value)<=best
        if not seed%2:assert int(value)==best
        checks+=1
print({'cases':24,'restriction_checks':checks,'status':'PASS'})
