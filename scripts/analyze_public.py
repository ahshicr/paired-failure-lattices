"""Audit all public-workflow attempts and compare only identical completed inputs."""
import argparse,collections,json,pathlib
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from audit_study import audit
ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--tables-only',action='store_true');args=parser.parse_args()
    base=ROOT/'results/public_verified/results/public_workflows_v1'
    if args.tables_only:
        base=ROOT/'results/public_tables';rows=pd.read_csv(base/'summary.csv',dtype={'objective_numerator':'string'}).to_dict('records');records=[]
        for r in rows:r['result']={'objective_numerator':r['objective_numerator']};records.append(r)
        verified={'evidence_verified':False,'records':len(records),'stage':'Table digest checked; full raw audit still required.'}
    else:verified,records=audit(base,8100)
    bycase=collections.defaultdict(dict);source=set();inputs=collections.defaultdict(set)
    for r in records:
        source.add(r['workflow_source_sha256']);inputs[r['case_id']].add(r['input_sha256'])
        if r['status']=='COMPLETE':bycase[r['case_id']][r['method']]=int(r['result']['objective_numerator'])
    assert len(source)==180 and len(inputs)==1620 and all(len(v)==1 for v in inputs.values())
    exact_methods=('paired_dense','paired_sparse','scenario','replica');eq=0;allfour=0;anyexact=0;quality=[]
    for case,vals in bycase.items():
        exact=[vals[m] for m in exact_methods if m in vals];assert len(set(exact))<=1
        if exact:anyexact+=1
        if len(exact)>1:eq+=1
        if len(exact)==4:allfour+=1
        if exact and 'greedy' in vals:
            assert vals['greedy']<=exact[0];quality.append({'case_id':case,'greedy_ratio':vals['greedy']/exact[0] if exact[0] else 1.0})
    d=pd.read_csv(base/'summary.csv',dtype={'objective_numerator':'string'});d['full_ms']=d.read_ms+d.end_to_end_ms;complete=d[d.status=='COMPLETE'];times=complete.pivot(index='case_id',columns='method',values='full_ms');methods={};mapping=[]
    for method,g in d.groupby('method'):
        good=g[g.status=='COMPLETE'];matched=times[['paired_dense',method]].dropna() if method!='paired_dense' else times[['paired_dense']].dropna();ratios=matched.paired_dense/matched[method]
        methods[method]={'attempts':len(g),'statuses':g.status.value_counts().to_dict(),'median_read_ms':float(good.read_ms.median()),'median_preprocess_ms':float(good.preprocessing_ms.median()),'median_plan_ms':float(good.planning_reconstruction_ms.median()),'median_full_seconds':float(good.full_ms.median()/1000),'median_peak_rss_mib':float(good.peak_rss_bytes.median()/2**20),'paired_completions':len(matched),'median_paired_dense_over_method_time':float(ratios.median())}
    for (embedding,method),g in d.groupby(['mapping','method']):mapping.append({'mapping':embedding,'method':method,'attempts':len(g),'statuses':g.status.value_counts().to_dict()})
    q=pd.DataFrame(quality);meta=d.drop_duplicates('case_id').set_index('case_id');q['mapping']=q.case_id.map(meta.mapping);qr=q.greedy_ratio
    report={'raw_audit':verified,'workflows':len(source),'cases':len(inputs),'nodes_min':int(d.nodes.min()),'nodes_max':int(d.nodes.max()),'cases_with_exact_results':anyexact,'cases_with_two_or_more_exact_completions':eq,'cases_with_all_four_exact_completions':allfour,'integer_objective_mismatches':0,'methods':methods,'completion_by_mapping':mapping,'greedy_quality':{'paired_cases':len(q),'minimum':float(qr.min()),'median':float(qr.median()),'optimal_count':int((qr==1).sum())}}
    out=ROOT/'results/analysis';(out/'public_statistics.json').write_text(json.dumps(report,indent=2));q.to_csv(out/'public_greedy_quality.csv',index=False);d.to_csv(out/'public_plot_source.csv',index=False)
    from result_text import public_insert
    (ROOT/'manuscript/source/tables/public_results.tex').write_text(public_insert(report),encoding='utf-8')
    names={'paired_dense':'Paired/dense','paired_sparse':'Paired/cached','scenario':'Scenario','replica':'Location','greedy':'Greedy'}
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':8,'axes.titlesize':9,'xtick.labelsize':7,'ytick.labelsize':7,'legend.fontsize':7,'pdf.fonttype':42,'ps.fonttype':42,'axes.spines.top':False,'axes.spines.right':False})
    fig,ax=plt.subplots(1,2,figsize=(178/25.4,74/25.4));fig.subplots_adjust(left=.08,right=.98,bottom=.29,top=.86,wspace=.38);order=['paired_dense','paired_sparse','scenario','replica','greedy'];bottom=np.zeros(5)
    for status,color,label in [('COMPLETE','#0072B2','Complete'),('TIMEOUT','#999999','Time limit'),('MEMORY_LIMIT','#D55E00','Memory limit')]:
        values=np.array([methods[m]['statuses'].get(status,0) for m in order]);ax[0].bar(np.arange(5),values,bottom=bottom,color=color,label=label);bottom+=values
    ax[0].set_xticks(np.arange(5),[names[m] for m in order],rotation=25,ha='right');ax[0].set_ylabel('Executions');ax[0].set_title('a   All 8,100 attempts',loc='left',fontweight='bold');ax[0].legend(frameon=False,fontsize=6,loc='lower left')
    for embedding,color in [('deepest_parent','#0072B2'),('shallowest_parent','#D55E00'),('dominator','#009E73')]:
        values=np.sort(q[q.mapping==embedding].greedy_ratio);ax[1].step(values,np.arange(1,len(values)+1)/len(values),where='post',c=color,label=embedding.replace('_',' ')+f' (n={len(values)})')
    ax[1].set_title('b   Quality on matched completions',loc='left',fontweight='bold');ax[1].set_xlabel('Greedy / exact objective');ax[1].set_ylabel('Fraction of matched instances');ax[1].set_ylim(0,1.02);ax[1].legend(frameon=False,fontsize=6,loc='upper left')
    for axis in ax:axis.grid(axis='y',alpha=.16)
    (ROOT/'figures/public_contract.json').write_text(json.dumps({'conclusion':'Full-corpus algorithm comparison retains resource limits and compares quality only against completed exact optima.','width_mm':178,'height_mm':74,'panels':['all terminal statuses','matched quality by declared packaging']},indent=2))
    for ext in ('pdf','svg','png'):fig.savefig(ROOT/f'manuscript/source/figures/fig_public_comparison.{ext}',dpi=300 if ext=='png' else None)
    plt.close(fig);print(json.dumps(report))

if __name__=='__main__':main()
