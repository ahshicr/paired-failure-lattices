"""Chronological held-out results, trace ambiguity, and honest solver certificates."""
import json,pathlib,collections,sys
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from audit_study import audit
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    trace=ROOT/'results/fields_verified/results/field_studies_v1/real_cloud'
    if not (trace/'complete.json').exists():trace=ROOT/'results/trace_verified/results/field_studies_v1/real_cloud'
    verified,records=audit(trace,8960)
    held=json.loads((trace/'held_out_results.json').read_text());lookup={(r['case_id'],r['method']):r for r in held};assert len(lookup)==8960
    data=[];bycase=collections.defaultdict(dict)
    for r in records:
        assert lookup[r['case_id'],r['method_label']]['scores']==r['held_out_evaluation']
        bycase[r['case_id']][r['method_label']]=r
        for year,score in r['held_out_evaluation'].items():
            data.append({**{k:r[k] for k in ('case_id','population','scope_interpretation','conditioning','nodes','budget','replicas','replicate','method_label')},'year':int(year),'numerator':int(score['objective_numerator']),'denominator':score['probability_denominator'],'value':int(score['objective_numerator'])/score['probability_denominator']})
    full=pd.DataFrame(data);test=full[full.year==2020].copy();oracle=test[test.method_label=='test_oracle'].set_index('case_id').value;test['oracle_ratio']=test.value/test.case_id.map(oracle);assert (test.oracle_ratio<=1+1e-12).all()
    # The conservative guarantee is for the fitted distribution, not a future year.
    cua=json.loads((ROOT/'data/processed/cua_replay.json').read_text());changed={(x['population'],x['scope'],x['conditioning']):x['higher_order_seconds'] for x in cua['statistics'] if x['year']==2018};bounds=[]
    for case,rows in bycase.items():
        f=rows['full_scenarios'];p=rows['paired_envelope'];truth=json.loads((trace/'truth'/(case+'.json')).read_text());w=sum(truth['instance']['benefit']);mass=changed[f['population'],f['scope_interpretation'],f['conditioning']];opt=int(f['held_out_evaluation']['2018']['objective_numerator']);actual=int(p['result']['objective_numerator']);model=int(p['result']['model_objective_numerator']);assert 0<=actual-model<=mass*w and 0<=opt-actual<=mass*w
        bounds.append({'case_id':case,'training_regret_numerator':opt-actual,'model_error_numerator':actual-model,'certified_bound_numerator':mass*w,'total_benefit':w})
    groups=[]
    for (population,scope,conditioning,method),g in test.groupby(['population','scope_interpretation','conditioning','method_label']):
        groups.append({'population':population,'scope':scope,'conditioning':conditioning,'method':method,'n':len(g),'minimum':float(g.oracle_ratio.min()),'q25':float(g.oracle_ratio.quantile(.25)),'median':float(g.oracle_ratio.median()),'maximum':float(g.oracle_ratio.max())})
    out=ROOT/'results/analysis';out.mkdir(exist_ok=True);pd.DataFrame(bounds).to_csv(out/'trace_envelope_bounds.csv',index=False);test.to_csv(out/'trace_test_scores.csv',index=False);pd.DataFrame(groups).to_csv(out/'trace_groups.csv',index=False)
    report={'raw_audit':verified,'chronological_split':cua['chronological_split'],'held_out_oracle_violations':0,'fitted_envelope_bounds_verified':len(bounds),'groups':groups};(out/'trace_statistics.json').write_text(json.dumps(report,indent=2))
    scopebase=ROOT/'results/fields_verified/results/field_studies_v1/scopes'
    if not (scopebase/'complete.json').exists():scopebase=ROOT/'results/scope_tables'
    sc=pd.read_csv(scopebase/'summary.csv',dtype={'objective_numerator':'string'});ip=sc[sc.method_label=='scope_integer'].copy();scope_rows=[]
    for n,g in ip.groupby('nodes'):
        scope_rows.append({'nodes':int(n),'attempts':len(g),'optimal':int((g.status=='OPTIMAL').sum()),'feasible':int((g.status=='FEASIBLE').sum()),'median_seconds':float(g.end_to_end_seconds.median()),'median_peak_rss_mib':float((g.peak_rss_bytes/2**20).median()),'median_relative_bound_gap':float(g.relative_bound_gap.median()),'maximum_relative_bound_gap':float(g.relative_bound_gap.max())})
    pairs=sc.pivot(index='case_id',columns='method_label',values='objective_numerator').dropna();pair_status=sc[sc.method_label=='scope_integer'].set_index('case_id').status;optimal_pairs=pairs[pairs.index.map(pair_status)=='OPTIMAL'];assert all(int(r.scope_integer)==int(r.tree_scenario) for r in optimal_pairs.itertuples())
    sreport={'rows':scope_rows,'nested_tree_completed':len(pairs),'nested_ip_certified_optimal':len(optimal_pairs),'nested_certified_objective_mismatches':0,'nonlaminar_cases':int(ip.actual_nonlaminar.sum()),'statuses':ip.status.value_counts().to_dict()};(out/'scope_statistics.json').write_text(json.dumps(sreport,indent=2));pd.DataFrame(scope_rows).to_csv(out/'scope_sizes.csv',index=False)
    contract={'conclusion':'Ambiguous scope interpretation changes held-out sensitivity; higher-order probability is small but nonzero; arbitrary-scope exact optimization has substantial certificate cost.','layout':'trace higher-order mass, held-out loss distribution, scope optimal certificates, scope bound gaps','scope':'All trace populations/interpretations in tables; figure held-out panel explicitly all-services/vendor-envelope/report-active stress replay.','width_mm':178,'height_mm':118};(ROOT/'figures/field_contract.json').write_text(json.dumps(contract,indent=2))
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':8,'axes.titlesize':9,'xtick.labelsize':7,'ytick.labelsize':7,'legend.fontsize':7,'pdf.fonttype':42,'ps.fonttype':42,'axes.spines.top':False,'axes.spines.right':False})
    fig,axes=plt.subplots(2,2,figsize=(178/25.4,118/25.4));fig.subplots_adjust(left=.10,right=.98,bottom=.11,top=.94,wspace=.42,hspace=.7)
    ax=axes[0,0]
    for scope,label,color,mark in [('identified','Identified regions','#0072B2','o'),('vendor_envelope','Provider envelope','#D55E00','s')]:
        rows=[x for x in cua['statistics'] if x['population']=='all_services' and x['scope']==scope and x['conditioning']=='report_active'];ax.plot([x['year'] for x in rows],[100*x['higher_order_fraction'] for x in rows],label=label,c=color,marker=mark,ms=4)
    ax.set_xticks([2018,2019,2020]);ax.set_ylabel('Mass outside paired support (%)');ax.set_xlabel('Calendar year');ax.set_ylim(0,1.3);ax.legend(frameon=False,fontsize=6.5,loc='lower left');ax.set_title('a   All services during active reports',loc='left',fontweight='bold')
    ax=axes[0,1];subset=test[(test.population=='all_services')&(test.scope_interpretation=='vendor_envelope')&(test.conditioning=='report_active')]
    order=['full_scenarios','paired_envelope','single_domain_envelope','failure_aggregate','independent_marginals','greedy'];colors=['#009E73','#0072B2','#56B4E9','#666666','#CC79A7','#D55E00'];labels=['Full events','Paired envelope','Single envelope','Failure aggregate','Independent','Greedy']
    for method,label,color in zip(order,labels,colors):
        x=np.sort(100*(1-subset[subset.method_label==method].oracle_ratio.to_numpy()));ax.step(x,np.arange(1,len(x)+1)/len(x),where='post',label=label,c=color,lw=1)
    ax.set_xlabel('2020 loss vs hindsight optimum (%)');ax.set_ylabel('Fraction of 160 cases');ax.legend(frameon=False,fontsize=5.8,loc='lower right');ax.set_title('b   Held out report replay',loc='left',fontweight='bold');ax.set_ylim(0,1.02)
    ax=axes[1,0];x=np.arange(5);rows=pd.DataFrame(scope_rows);ax.bar(x,rows.optimal/480*100,color='#0072B2',label='Proved optimal');ax.bar(x,rows.feasible/480*100,bottom=rows.optimal/480*100,color='#999999',label='Feasible at 60 s');ax.set_xticks(x,rows.nodes);ax.set_ylim(0,100);ax.set_xlabel('Images / atomic fragments');ax.set_ylabel('Fraction of 480 cases (%)');ax.legend(frameon=False,fontsize=6,loc='lower left');ax.set_title('c   General scope certificates',loc='left',fontweight='bold')
    ax=axes[1,1]
    for n,color in [(32,'#0072B2'),(64,'#D55E00'),(128,'#009E73')]:
        values=np.sort(ip[ip.nodes==n].relative_bound_gap.to_numpy());ax.step(values,np.arange(1,len(values)+1)/len(values),where='post',c=color,label=str(n),lw=1)
    ax.set_xlabel('Relative upper bound gap');ax.set_ylabel('Fraction of 480 cases');ax.legend(title='Images',frameon=False,fontsize=6,loc='lower right');ax.set_ylim(0,1.02);ax.set_title('d   Optimality bound distributions',loc='left',fontweight='bold')
    for ax in axes.flat:ax.grid(axis='y',alpha=.15)
    for ext in ('pdf','svg','png'):fig.savefig(ROOT/f'manuscript/source/figures/fig_trace_scope.{ext}',dpi=300 if ext=='png' else None)
    plt.close(fig);print(json.dumps({'trace_records':len(records),'bounds_verified':len(bounds),'scope':sreport}))

if __name__=='__main__':main()
