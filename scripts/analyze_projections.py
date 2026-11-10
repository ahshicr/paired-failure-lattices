"""Compare tree restrictions on the same manifests; bound unresolved optimum ratios."""
import json,math,pathlib
import pandas as pd
from audit_study import audit
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    base=ROOT/'results/projections_verified/results/laminar_projections_v1';verified,records=audit(base,7200)
    p=pd.read_csv(base/'summary.csv',dtype={'objective_numerator':'string'});sbase=ROOT/'results/fields_verified/results/field_studies_v1/scopes';s=pd.read_csv(sbase/'summary.csv',dtype={'objective_numerator':'string'})
    v=p.pivot(index='case_id',columns='method_label',values='objective_numerator').map(int);ip=s[s.method_label=='scope_integer'].set_index('case_id');v['ip_incumbent']=ip.objective_numerator.map(int);v['status']=ip.status;v['nonlaminar']=ip.actual_nonlaminar;v['upper_bound']=ip.best_bound.map(math.ceil);v['nodes']=ip.nodes;v['budget']=ip.budget
    policies=['small_first','large_first','benefit_first'];v['best_restriction']=v[policies].max(axis=1);v['lower_bound']=v[policies+['ip_incumbent']].max(axis=1);assert (v.lower_bound<=v.upper_bound).all()
    nested=s[s.method_label=='tree_scenario'].set_index('case_id').objective_numerator.map(int)
    for case,value in nested.items():assert all(v.loc[case,m]==value for m in policies)
    rows=[];cert=v[(v.status=='OPTIMAL')&v.nonlaminar]
    for method in policies+['best_restriction']:
        ratios=cert[method]/cert.ip_incumbent.where(cert.ip_incumbent!=0,1);ratios.loc[cert.ip_incumbent==0]=1
        rows.append({'method':method,'certified_nonlaminar_cases':len(cert),'strict_loss_cases':int((ratios<1-1e-12).sum()),'minimum_ratio':float(ratios.min()),'median_ratio':float(ratios.median())})
        v[method+'_ratio_lower']=v[method]/v.upper_bound.where(v.upper_bound!=0,1);v[method+'_ratio_upper']=v[method]/v.lower_bound.where(v.lower_bound!=0,1)
        v.loc[v.upper_bound==0,[method+'_ratio_lower',method+'_ratio_upper']]=1
    report={'raw_audit':verified,'same_scope_cases':len(v),'policies':rows,'exact_nested_comparisons':len(nested)*3,'nonlaminar_cases':int(v.nonlaminar.sum()),'certified_nonlaminar_cases':len(cert),'interpretation':'Ratios use identical original fragments, budgets, costs, and event support. Removing images restricts feasible choices. Unfinished general optima use bound intervals, never presumed exact.'}
    out=ROOT/'results/analysis';(out/'projection_statistics.json').write_text(json.dumps(report,indent=2));v.to_csv(out/'projection_bounds.csv')
    from result_text import projection_insert
    (ROOT/'manuscript/source/tables/projection_results.tex').write_text(projection_insert(report),encoding='utf-8');print(json.dumps(report))


if __name__=='__main__':main()
