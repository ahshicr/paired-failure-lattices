"""Check both envelope inequalities against exhaustive independent deployments."""
import argparse,copy,json,pathlib,sys,time
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from instance_io import generate,leaf_masks,sum_masks,enumerate_optimum,evaluate

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',default='results/envelope_bound');p.add_argument('--cases',type=int,default=96);a=p.parse_args();out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True);rows=[];start=time.time()
    for seed in range(a.cases):
        x=generate(leaves=3+seed%6,nodes=2+seed%5,budget=[1,3,7,12][seed%4],replicas=1+seed%3,options=1+seed%3,events=4+seed%9,topology=['star','balanced','random'][seed%3],seed=871109+seed,higher_order=True,test_boundaries=True)
        masks,_=leaf_masks(x['domain_parent']);catalog={0:[]}
        for i,m in enumerate(masks):catalog.setdefault(m,[i])
        for i,m in enumerate(masks):
            for j in range(i+1,len(masks)):catalog.setdefault(m|masks[j],[i,j])
        full=max(masks,key=int.bit_count);envelope=copy.deepcopy(x);changed=0
        for event in envelope['events']:
            failed=sum_masks(masks,event['domains']);hull=full
            for candidate in catalog:
                if failed&~candidate==0:hull&=candidate
            assert hull in catalog and failed&~hull==0
            if hull!=failed:changed+=event['mass']
            event['domains']=catalog[hull]
        best,choice,feasible=enumerate_optimum(x);envelope_best,envelope_choice,_=enumerate_optimum(envelope)
        actual,_=evaluate(x,envelope_choice);under,_=evaluate(envelope,choice);bound=changed*sum(x['benefit'])
        assert 0<=best-under<=bound and 0<=best-actual<=bound and envelope_best<=actual
        rows.append({'seed':seed,'true_optimum':str(best),'envelope_optimum':str(envelope_best),'envelope_deployment_true_value':str(actual),'changed_event_mass':changed,'integer_error_bound':str(bound),'feasible_deployments':feasible})
    report={'status':'PASS','cases':len(rows),'nontrivial_changed_support_cases':sum(r['changed_event_mass']>0 for r in rows),'elapsed_seconds':time.time()-start,'records':rows};(out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='records'}))

if __name__=='__main__':main()
