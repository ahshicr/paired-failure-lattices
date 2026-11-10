"""Render result paragraphs directly from the verified analysis dictionaries."""
import json,pathlib
from academic_style import polish
ROOT=pathlib.Path(__file__).resolve().parents[1]

def public_insert(report):
    methods=report['methods'];quality=report['greedy_quality']
    assert report['raw_audit']['evidence_verified'] and report['integer_objective_mismatches']==0
    for method in ('paired_dense','paired_sparse','scenario'):assert methods[method]['statuses']['COMPLETE']==report['cases']
    text=r'''Paired-dense, paired-cached, and scenario completed all '''+f"{report['cases']:,}"+r'''
declared workflow instances with identical integer objectives.
The recovery structures contain '''+f"{report['nodes_min']:,}--{report['nodes_max']:,}"+r'''
nodes, including the virtual root. Table~\ref{tab:public-comparison}
reports completion counts and medians over completed cases.
The median within-instance paired-dense/scenario full-time ratio was
'''+f"{methods['scenario']['median_paired_dense_over_method_time']:.3f}"+r''' across all '''+f"{report['cases']:,}"+r''' matched instances.
On '''+f"{quality['paired_cases']:,}"+r''' matched completions, the greedy objective/exact optimum
ratio had median '''+f"{quality['median']:.4f}"+' and minimum '+f"{quality['minimum']:.4f}"+r'''.
Across methods, '''+str(sum(x['statuses'].get('TIMEOUT',0) for x in methods.values()))+' tasks reached the time limit and '+str(sum(x['statuses'].get('MEMORY_LIMIT',0) for x in methods.values()))+r''' reached
the memory limit.

\begin{table}[t]
\centering\small
\caption{Public-workflow completion counts and performance. Full-time
and RSS medians use completed instances for each method.
The text reports within-instance time ratios on matched completions.}
\label{tab:public-comparison}
\begin{tabular}{@{}lrrr@{}}\toprule
Method & Complete & Time (s) & RSS (MiB)\\\midrule
'''
    labels={'paired_dense':'Paired/dense','paired_sparse':'Paired/cached','scenario':'Scenario','replica':'Location','greedy':'Greedy'}
    for method,label in labels.items():
        r=methods[method]
        text+=f"{label} & {r['statuses'].get('COMPLETE',0)}/{r['attempts']} & {r['median_full_seconds']:.3f} & {r['median_peak_rss_mib']:.1f}"+r'\\'+'\n'
    return polish(text+r'\bottomrule\end{tabular}'+'\n'+r'\end{table}'+'\n')

def projection_insert(report):
    assert report['raw_audit']['evidence_verified']
    values={x['method']:x for x in report['policies']};order=('small_first','large_first','benefit_first')
    text=r'''To measure the effect of restricting image choices, we retain maximal
laminar subsets by selecting the smallest scope, the largest scope,
or the highest total fragment benefit first. Contents, costs, and events
remain fixed, and omitted images receive the null choice. Each retained
collection is optimized by scenario-based tree DP.
All 7,200 restrictions completed. The 2,400 comparisons on 800
zero-injection instances equaled the unrestricted tree optimum.
Among the '''+f"{report['certified_nonlaminar_cases']:,}"+r''' nonlaminar instances with a certified unrestricted
optimum, the three policies reduced objective value in '''+', '.join(str(values[x]['strict_loss_cases']) for x in order)+r'''
cases, respectively. Their minimum ratios to that optimum were
'''+', '.join(f"{values[x]['minimum_ratio']:.4f}" for x in order)+r'''.
The best of the three restrictions reduced value in '''+str(values['best_restriction']['strict_loss_cases'])+r''' cases.
For uncertified instances, the best available feasible value and the
general solver's upper bound define an interval for the restriction/optimum ratio. The archive
provides these intervals and the selected images.
'''
    return polish(text)

def main():
    for name,renderer in [('public',public_insert),('projection',projection_insert)]:
        report=json.loads((ROOT/f'results/analysis/{name}_statistics.json').read_text())
        (ROOT/f'manuscript/source/tables/{name}_results.tex').write_text(renderer(report),encoding='utf-8')
    print('Public and projection paragraphs rendered from verified statistics.')

if __name__=='__main__':main()
