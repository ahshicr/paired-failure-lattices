"""Audit all representation/ablation attempts and show the measured tradeoffs."""
import collections,hashlib,json,pathlib
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=pathlib.Path(__file__).resolve().parents[1];base=ROOT/'results/supplemental_verified/results/supplemental_v1'
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()

def main():
    marker=json.loads((base/'complete.json').read_text());assert sha(base/'summary.csv')==marker['summary_sha256'];records=[];outputs=[];values=collections.defaultdict(set);inputs={}
    for path in sorted((base/'records').glob('*.json')):
        if path.name.endswith('.start.json'):continue
        r=json.loads(path.read_text());records.append(r);outputs.append(r['stdout']);name=pathlib.Path(r['input']).name
        if name not in inputs:inputs[name]=sha(base/'inputs'/name)
        assert inputs[name]==r['input_sha256']
        for stream in ('stdout','stderr'):assert sha(path.parent/r[stream])==r[stream+'_sha256']
        if r['status']=='COMPLETE':assert json.loads((path.parent/r['stdout']).read_text())==r['result'];values[r['case_id']].add(int(r['result']['objective_numerator']))
    assert len(records)==len(set(outputs))==7200 and all(len(v)==1 for v in values.values())
    d=pd.read_csv(base/'summary.csv',dtype={'objective_numerator':'string'});d['full_end_to_end_ms']=d.read_ms+d.end_to_end_ms;stats={};ablation=[]
    for (study,method),g in d.groupby(['study','method_label']):stats[study+'/'+method]={'attempts':len(g),'statuses':g.status.value_counts().to_dict()}
    ab=d[(d.study=='ablations')&(d.status=='COMPLETE')];times=ab.pivot(index='case_id',columns='method_label',values='full_end_to_end_ms');ratios={}
    for method in times:
        if method=='paired_dense':continue
        pair=times[[method,'paired_dense']].dropna();ratio=pair[method]/pair.paired_dense;ratios[method]=ratio
        ablation.append({'method':method,'paired_completed':len(pair),'median_ratio':float(ratio.median()),'q25':float(ratio.quantile(.25)),'q75':float(ratio.quantile(.75))})
    duplicate_effect=[]
    for duplicate,g in ab.groupby('duplicate_fraction'):
        v=g.pivot(index='case_id',columns='method_label',values='full_end_to_end_ms');r=v['paired_dense+no_compression']/v.paired_dense;duplicate_effect.append({'duplicate_fraction':duplicate,'paired_cases':len(r),'median_no_compression_over_dense':float(r.median())})
    rep=d[d.study=='representation'];trade=[]
    for (q,leaves),g in rep[rep.status=='COMPLETE'].groupby(['events','leaves']):
        v=g.pivot(index='case_id',columns='method_label',values='query_ms');r=v.scenario/v.paired_dense;trade.append({'event_identities':int(q),'leaves':int(leaves),'paired_cases':len(r),'median_scenario_over_paired_query_time':float(r.median())})
    out=ROOT/'results/analysis';out.mkdir(exist_ok=True);report={'evidence_verified':True,'unique_outputs':len(set(outputs)),'source_summary_sha256':sha(base/'summary.csv'),'statuses':stats,'ablation_ratios':ablation,'duplicate_effect':duplicate_effect,'representation_query_ratios':trade};(out/'supplemental_statistics.json').write_text(json.dumps(report,indent=2))
    (ROOT/'figures/representation_contract.json').write_text(json.dumps({'conclusion':'Paired dense query cost remains nearly constant as the hierarchy grows; larger finite support can reverse the scenario advantage, while compilation and full-state DP retain separate costs.','layout':'sparse queries, larger-support queries, process RSS, complete ablation distributions','width_mm':178,'height_mm':118,'resource_censoring':'All terminal statuses reported; missing explicit points are not assigned a speedup; ablation ratios use only matched completed instances.','uncertainty':'Median and interquartile range over all 60 topology/seed conditions per hierarchy/support combination; ablation boxes show all completed ranges.'},indent=2))
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':8,'axes.labelsize':8,'axes.titlesize':9,'xtick.labelsize':7,'ytick.labelsize':7,'legend.fontsize':7,'pdf.fonttype':42,'ps.fonttype':42,'svg.fonttype':'none','axes.spines.top':False,'axes.spines.right':False})
    styles={'paired_dense':('Paired/dense','#0072B2','o'),'paired_sparse':('Paired/cached','#56B4E9','s'),'scenario':('Scenario','#D55E00','^'),'replica':('Location','#009E73','D'),'explicit':('Explicit catalogue','#666666','x')}
    fig,axes=plt.subplots(2,2,figsize=(178/25.4,118/25.4));fig.subplots_adjust(left=.09,right=.98,bottom=.115,top=.85,wspace=.43,hspace=.62);plotrows=[]
    def plot(ax,events,metric,scale):
        for method,(label,color,mark) in styles.items():
            rows=[]
            for leaves,g in rep[(rep.events==events)&(rep.method_label==method)].groupby('leaves'):
                good=g[g.status=='COMPLETE'];n=len(good)
                if not n:continue
                y=good[metric]*scale;row={'panel':ax.get_label(),'method':method,'leaves':leaves,'event_identities':events,'complete':n,'attempted':len(g),'median':y.median(),'q25':y.quantile(.25),'q75':y.quantile(.75)};rows.append(row);plotrows.append(row)
            xs=[r['leaves'] for r in rows];ys=[r['median'] for r in rows];ax.plot(xs,ys,label=label,c=color,marker=mark,ms=3,lw=1);ax.fill_between(xs,[r['q25'] for r in rows],[r['q75'] for r in rows],color=color,alpha=.08,lw=0)
        ax.set_xscale('log',base=2);ax.set_yscale('log');ax.set_xticks([16,64,256,1024,2048],['16','64','256','1024','2048']);ax.set_xlabel('Failure tree leaves');ax.grid(axis='y',alpha=.18)
    ax=axes[0,0];ax.set_label('sparse_query');plot(ax,256,'query_ms',1000/1024);ax.set_ylabel('Union + loss query (us)');ax.set_title('a   256 generated events',loc='left',fontweight='bold')
    ax=axes[0,1];ax.set_label('larger_support_query');plot(ax,32768,'query_ms',1000/1024);ax.set_ylabel('Union + loss query (us)');ax.set_title('b   32,768 generated events',loc='left',fontweight='bold');ax.text(.97,.95,'Explicit 0/60 at 2048',transform=ax.transAxes,ha='right',va='top',fontsize=6.5,color='#555555')
    ax=axes[1,0];ax.set_label('memory');plot(ax,32768,'peak_rss_bytes',1/2**20);ax.set_ylabel('Peak RSS (MiB)');ax.set_title('c   Total process memory',loc='left',fontweight='bold')
    ax=axes[1,1];order=['paired_dense+full_states','paired_dense+no_compression','paired_sparse','scenario'];labels=['All states','No menu merge','Cached masses','Scenario'];box=ax.boxplot([ratios[m].values for m in order],vert=False,labels=labels,whis=(0,100),showfliers=False,patch_artist=True,widths=.45,medianprops={'color':'#111111','lw':1.2},boxprops={'lw':.7},whiskerprops={'lw':.7},capprops={'lw':.7})
    for patch,color in zip(box['boxes'],['#999999','#0072B2','#56B4E9','#D55E00']):patch.set_facecolor(color);patch.set_alpha(.45)
    ax.axvline(1,c='#777777',ls=':',lw=.8);ax.set_xscale('log');ax.set_xlabel('Time / paired dense time');ax.set_title('d   Ablation effects',loc='left',fontweight='bold');ax.tick_params(axis='y',labelsize=7);ax.invert_yaxis();ax.grid(axis='x',alpha=.18)
    handles,labels=axes[0,0].get_legend_handles_labels();fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.53,.985),ncol=5,frameon=False,columnspacing=.8,handlelength=1.3)
    dest=ROOT/'manuscript/source/figures'
    for ext in ('pdf','svg','png'):fig.savefig(dest/f'fig_representation_ablation.{ext}',dpi=300 if ext=='png' else None)
    pd.DataFrame(plotrows).to_csv(ROOT/'figures/representation_plot_source.csv',index=False);plt.close(fig)
    print(json.dumps({'records':len(records),'ablation_ratios':ablation,'duplicate_effect':duplicate_effect,'query_tradeoffs':trade}))

if __name__=='__main__':main()
