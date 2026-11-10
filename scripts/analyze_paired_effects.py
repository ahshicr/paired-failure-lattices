"""Estimate paired effects from completed runs with blocked resampling."""
import hashlib,json,pathlib
import numpy as np
import pandas as pd
from scipy.stats import binomtest
ROOT=pathlib.Path(__file__).resolve().parents[1]
OUT=ROOT/'scientific_revision';OUT.mkdir(exist_ok=True)
SOURCES={}

def read(relative):
    path=ROOT/relative;SOURCES[relative]=hashlib.sha256(path.read_bytes()).hexdigest()
    return pd.read_csv(path,dtype={'objective_numerator':'string'})

def evaluate(frame,method,numerator,denominator,value,unit,key,label,initialization):
    good=frame[frame.status=='COMPLETE']
    values=good.pivot(index='case_id',columns=method,values=value)[[numerator,denominator]].dropna()
    ratios=values[numerator]/values[denominator]
    units=frame.groupby('case_id')[unit].first().reindex(ratios.index)
    groups=[ratios[units==u].to_numpy() for u in sorted(units.unique())]
    group_medians=np.array([np.median(g) for g in groups])
    positive=int((group_medians>1).sum());negative=int((group_medians<1).sum())
    p=float(binomtest(positive,positive+negative,.5,alternative='two-sided').pvalue)
    rng=np.random.default_rng(initialization)
    bootstrap=np.array([np.median(np.concatenate([groups[i] for i in rng.integers(0,len(groups),len(groups))])) for _ in range(10000)])
    low,high=np.quantile(bootstrap,[.025,.975])
    pd.DataFrame({'case_id':ratios.index,'block':units.values,'ratio':ratios.values}).to_csv(OUT/(key+'_pairs.csv'),index=False)
    return {'key':key,'comparison':label,'numerator':numerator,'denominator':denominator,'measurement':value,'paired_cases':len(ratios),'blocks':len(groups),'block_unit':unit,'median_ratio':float(ratios.median()),'confidence_interval_95':[float(low),float(high)],'positive_block_medians':positive,'negative_block_medians':negative,'tied_block_medians':int((group_medians==1).sum()),'two_sided_sign_p':p,'bootstrap_resamples':10000,'random_initialization':initialization}

def main():
    s=read('results/supplemental_verified/results/supplemental_v1/summary.csv');s['full_ms']=s.read_ms+s.end_to_end_ms
    e=read('results/end_to_end_verified/results/end_to_end_v1/summary.csv');e['full_ms']=e.read_ms+e.end_to_end_ms
    p=read('results/public_verified/results/public_workflows_v1/summary.csv');p['full_ms']=p.read_ms+p.end_to_end_ms
    rows=[
      evaluate(s[(s.study=='representation')&(s.events==32768)],'method_label','scenario','paired_dense','query_ms','replicate','query_effect','Scenario / paired query',2026110901),
      evaluate(s[s.study=='ablations'],'method_label','paired_dense+full_states','paired_dense','full_ms','replicate','reachable_effect','All states / reachable states',2026110902),
      evaluate(s[(s.study=='ablations')&(s.duplicate_fraction==.8)],'method_label','paired_dense+no_compression','paired_dense','full_ms','replicate','menu_effect','Unmerged / merged menus',2026110903),
      evaluate(e,'method','paired_dense','scenario','full_ms','replicate','synthetic_effect','Paired / scenario, synthetic',2026110904),
      evaluate(p,'method','paired_dense','scenario','full_ms','workflow_id','public_effect','Paired / scenario, public',2026110905)]
    running=0
    for rank,index in enumerate(sorted(range(len(rows)),key=lambda i:rows[i]['two_sided_sign_p'])):
        running=max(running,min(1,(len(rows)-rank)*rows[index]['two_sided_sign_p']))
        rows[index]['holm_adjusted_p']=running
    report={'status':'PASS','source_sha256':SOURCES,'estimand':'Median ratio of matched observed running times within the stated experimental matrix.','resampling':'Percentile interval from 10000 blocked bootstrap samples. Public comparisons resample workflows with all mappings and realizations together. Synthetic comparisons resample realization indices with all parameter settings together.','test':'Two sided exact sign tests on the counts of block median ratios above and below one. Holm correction across the five stated contrasts.','scope':'Variation across the tested instances and realization blocks. Completion conditioned estimates retain their stated matched population.','inclusion':{'query_effect':'All eight hierarchy sizes and all three topologies at 32768 generated events.','reachable_effect':'All matched completed ablation cases.','menu_effect':'All matched ablation cases at duplicate injection probability 0.8.','synthetic_effect':'All synthetic complete planning cases.','public_effect':'All public workflow cases, clustered by source workflow.'},'contrasts':rows}
    (OUT/'paired_effects.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    table=[r'\begin{table*}[t]',r'\centering\small',r'\caption{Paired running time effects. Intervals describe the median of within instance ratios under blocked resampling. Query results pool all eight hierarchy sizes at 32,768 generated events. Menu results use duplicate injection probability 0.8. The final column gives two sided sign test $p$ values from block medians after Holm correction.}',r'\label{tab:paired-effects}',r'\begin{tabular}{lrrrr}',r'\toprule',r'Time ratio & Pairs / blocks & Median & 95\% interval & Adjusted $p$\\',r'\midrule']
    for row in rows:
        lo,hi=row['confidence_interval_95'];pv=row['holm_adjusted_p']
        pp=r'$<0.0001$' if pv<.0001 else f'{pv:.4f}'
        table.append(f"{row['comparison']} & {row['paired_cases']} / {row['blocks']} & {row['median_ratio']:.2f} & {lo:.2f} to {hi:.2f} & {pp}"+r'\\')
    table += [r'\bottomrule',r'\end{tabular}',r'\end{table*}']
    (ROOT/'manuscript/source/tables/paired_effects.tex').write_text('\n'.join(table)+'\n',encoding='utf-8')
    print(json.dumps(rows,indent=2))

if __name__=='__main__':main()
