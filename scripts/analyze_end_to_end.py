"""Verify complete task evidence and produce the sparse-support performance figure."""
import collections,hashlib,json,pathlib
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT=pathlib.Path(__file__).resolve().parents[1]
base=ROOT/'results/end_to_end_verified/results/end_to_end_v1'
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def main():
    completed=json.loads((base/'complete.json').read_text());assert sha(base/'summary.csv')==completed['summary_sha256']
    records=[];output_names=[];values=collections.defaultdict(dict)
    for p in sorted((base/'records').glob('*.json')):
        if p.name.endswith('.start.json'):continue
        r=json.loads(p.read_text());records.append(r);output_names.append(r['stdout'])
        for stream in ('stdout','stderr'):assert sha(p.parent/r[stream])==r[stream+'_sha256']
        assert sha(base/'inputs'/pathlib.Path(r['input']).name)==r['input_sha256']
        if r['status']=='COMPLETE':values[r['case_id']][r['method']]=int(r['result']['objective_numerator'])
    assert len(records)==4860 and len(set(output_names))==4860 and len(values)==972
    for methods in values.values():
        assert methods['paired_dense']==methods['paired_sparse']==methods['scenario']
        assert methods['greedy']<=methods['scenario']
        if 'replica' in methods:assert methods['replica']==methods['scenario']
    d=pd.read_csv(base/'summary.csv',dtype={'objective_numerator':'string','model_objective_numerator':'string'})
    d['full_end_to_end_ms']=d['read_ms']+d['end_to_end_ms']
    c=d[d.status=='COMPLETE'].copy();t=c.pivot(index='case_id',columns='method',values='full_end_to_end_ms');statistics={}
    for m,g in d.groupby('method'):
        good=g[g.status=='COMPLETE'];pairs=t[['paired_dense',m]].dropna() if m!='paired_dense' else t[['paired_dense']].dropna();ratio=(pairs['paired_dense']/pairs[m]) if m!='paired_dense' else pd.Series(np.ones(len(pairs)))
        statistics[m]={'tasks':len(g),'complete':len(good),'statuses':g.status.value_counts().to_dict(),'median_input_ms':float(good.read_ms.median()),'median_preprocessing_ms':float(good.preprocessing_ms.median()),'median_planning_reconstruction_ms':float(good.planning_reconstruction_ms.median()),'median_total_ms':float(good.full_end_to_end_ms.median()),'median_peak_rss_mib':float(good.peak_rss_bytes.median()/2**20),'paired_completions':len(pairs),'median_paired_dense_over_method_time':float(ratio.median()),'median_backend_states':float(good.backend_states.median())}
    quality=np.array([v['greedy']/v['paired_dense'] for v in values.values()]);quality_stats={'minimum':float(quality.min()),'median':float(np.median(quality)),'maximum':float(quality.max()),'exact_count':int(sum(v['greedy']==v['paired_dense'] for v in values.values())),'cases':len(quality)}
    out=ROOT/'results/analysis';out.mkdir(exist_ok=True);report={'evidence_verified':True,'records':len(records),'unique_outputs':len(set(output_names)),'method_statistics':statistics,'greedy_quality':quality_stats,'source_summary_sha256':sha(base/'summary.csv'),'interpretation':'Sparse support allows much stronger instance-specific scenario compression; the universal paired lower bound does not predict an end-to-end win.'};(out/'end_to_end_statistics.json').write_text(json.dumps(report,indent=2))
    contract={'conclusion':'Scenario compression is preferable for this sparse-support matrix; total time includes the paired probability compiler.','source':'972 full-factorial instances, paired methods; 363 location-bitset memory limits retained','layout':'four panels: hierarchy size, budget, replica-memory dependence, greedy objective distribution','uncertainty':'median and interquartile range over 12 deterministic replicates; no confidence interval claim','exports':['PDF','SVG','PNG'],'width_mm':178,'height_mm':112};(ROOT/'figures/end_to_end_contract.json').write_text(json.dumps(contract,indent=2))
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':8,'axes.labelsize':8,'axes.titlesize':9,'xtick.labelsize':7,'ytick.labelsize':7,'legend.fontsize':7,'pdf.fonttype':42,'ps.fonttype':42,'svg.fonttype':'none','axes.spines.top':False,'axes.spines.right':False})
    styles={'paired_dense':('Paired/dense','#0072B2','o'),'paired_sparse':('Paired/cached','#56B4E9','s'),'scenario':('Scenario','#D55E00','^'),'replica':('Location','#009E73','D'),'greedy':('Greedy','#CC79A7','v')}
    fig,axes=plt.subplots(2,2,figsize=(178/25.4,112/25.4));fig.subplots_adjust(left=.085,right=.975,bottom=.12,top=.83,wspace=.31,hspace=.65)
    sources=[]
    def series(ax,frame,x,y,divisor=1):
        for method,(label,color,marker) in styles.items():
            g=frame[frame.method==method];xs=[];med=[];lower=[];upper=[]
            for val,group in g.groupby(x):
                good=group[group.status=='COMPLETE'];n=len(good)
                if not n:continue
                z=good[y]/divisor;xs.append(val);med.append(z.median());lower.append(z.quantile(.25));upper.append(z.quantile(.75));sources.append({'panel':ax.get_label(),'method':method,x:val,'complete':n,'total':len(group),'median':z.median(),'q25':z.quantile(.25),'q75':z.quantile(.75)})
                if n<len(group):ax.annotate(f'{n}/{len(group)}',(val,z.median()),xytext=(0,8),textcoords='offset points',ha='center',fontsize=6.5,color=color)
            ax.plot(xs,med,color=color,marker=marker,markersize=3.5,lw=1.1,label=label);ax.fill_between(xs,lower,upper,color=color,alpha=.10,lw=0)
        ax.set_yscale('log');ax.grid(axis='y',alpha=.18)
    ax=axes[0,0];ax.set_label('hierarchy');series(ax,d[(d.nodes==128)&(d.budget==64)&(d.replicas==2)],'leaves','full_end_to_end_ms');ax.set_xscale('log',base=2);ax.set_xticks([64,256,1024],['64','256','1024']);ax.set_xlabel('Failure tree leaves');ax.set_ylabel('Complete planning time (ms)');ax.set_title('a   Hierarchy size',loc='left',fontweight='bold')
    ax=axes[0,1];ax.set_label('budget');series(ax,d[(d.nodes==128)&(d.leaves==256)&(d.replicas==2)],'budget','full_end_to_end_ms');ax.set_xticks([16,64,128]);ax.set_xlabel('Budget');ax.set_ylabel('Complete planning time (ms)');ax.set_title('b   Budget dependence',loc='left',fontweight='bold')
    ax=axes[1,0];ax.set_label('memory');series(ax,d[(d.nodes==128)&(d.leaves==256)&(d.budget==64)],'replicas','peak_rss_bytes',2**20);ax.set_xticks([1,2,4]);ax.set_xlabel('Replicas per nonnull choice');ax.set_ylabel('Peak RSS (MiB)');ax.set_title('c   Replica dependence',loc='left',fontweight='bold')
    ax=axes[1,1];q=np.sort(quality);ax.step(q,np.arange(1,len(q)+1)/len(q),where='post',color=styles['greedy'][1],lw=1.4);ax.set_xlabel('Greedy objective / exact optimum');ax.set_ylabel('Fraction of instances');ax.set_ylim(0,1.02);ax.set_xlim(.75,1.005);ax.set_xticks([.75,.85,.95,1]);ax.grid(alpha=.18);ax.set_title('d   Placement quality',loc='left',fontweight='bold');ax.text(.03,.93,f"Median {np.median(q):.3f}\nExact {quality_stats['exact_count']}/972",transform=ax.transAxes,va='top',fontsize=7)
    handles,labels=axes[0,0].get_legend_handles_labels();fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.52,.985),ncol=5,frameon=False,columnspacing=1.0,handlelength=1.5)
    dest=ROOT/'manuscript/source/figures';dest.mkdir(exist_ok=True)
    for ext in ('pdf','svg','png'):fig.savefig(dest/f'fig_end_to_end.{ext}',dpi=300 if ext=='png' else None)
    pd.DataFrame(sources).to_csv(ROOT/'figures/end_to_end_plot_source.csv',index=False)
    plt.close(fig);print(json.dumps({'records_verified':len(records),'statistics':statistics,'greedy':quality_stats}))

if __name__=='__main__':main()
