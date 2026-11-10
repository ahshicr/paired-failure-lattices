"""Exhaustive oracle checks for the arbitrary-scope integer formulation."""
import argparse,json,pathlib,random,sys,time
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from instance_io import generate,enumerate_optimum
from scope_solver import solve,evaluate_scope,tree_scopes,enumerate_scope

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',default='results/scope_exactness');p.add_argument('--cases',type=int,default=120);a=p.parse_args();out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True)
    start=time.time();records=[]
    for seed in range(a.cases):
        rng=random.Random(43109+seed);nodes=1+seed%6
        x=generate(leaves=2+seed%6,nodes=nodes,budget=[0,1,3,7,12][seed%5],replicas=1+seed%3,options=1+seed%3,events=3+seed%9,seed=43109+seed,higher_order=True,test_boundaries=True)
        x['scopes']=tree_scopes(x['parent'])
        if seed%2:
            x['scopes']=[sorted(rng.sample(range(nodes),rng.randint(1,nodes))) for _ in range(nodes)]
        expected,selected,feasible=enumerate_scope(x);result=solve(x,seconds=10)
        assert result['status']=='OPTIMAL' and int(result['objective_numerator'])==expected,(seed,result,expected)
        if seed%2==0:assert expected==enumerate_optimum(x)[0]
        records.append({'seed':seed,'nested':seed%2==0,'expected_numerator':str(expected),'feasible_deployments':feasible,'solver':result})
        (out/f'case_{seed:03d}.json').write_text(json.dumps(x))
    report={'status':'PASS','cases':len(records),'elapsed_seconds':time.time()-start,'records':records};(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='records'}))

if __name__=='__main__':main()
