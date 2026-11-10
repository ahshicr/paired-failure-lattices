"""Draw the declared example; all displayed values come from exhaustive enumeration."""
from __future__ import annotations
import copy,json
import pathlib
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

REV=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(REV/"experiments"))
from instance_io import enumerate_optimum

def example():
    null={"cost":0,"replicas":[]}
    a={"domain_parent":[-1,0,0,0,1,1,2,2,3,3],"parent":[-1,0,1,1,0,4],
       "benefit":[0,0,8,5,0,3],"budget":5,
       "menus":[[null],[null,{"cost":2,"replicas":[4]},{"cost":4,"replicas":[4,6]},
                        {"cost":6,"replicas":[4,6,8]}],[null],[null],
                [null,{"cost":1,"replicas":[8]},{"cost":2,"replicas":[4,8]},
                 {"cost":3,"replicas":[4,6,8]}],[null]],
       "events":[{"mass":mass,"domains":domains} for mass,domains in
                 [(40,[]),(20,[1]),(15,[2]),(10,[3]),(10,[1,2]),(3,[1,3]),(2,[2,3])]]}
    records=[]
    for b in range(9):
        a["budget"]=b
        optimum,selected,count=enumerate_optimum(a)
        component_values=[]
        for fragments in ((2,3),(5,)):
            subproblem=copy.deepcopy(a)
            subproblem["benefit"]=[w if i in fragments else 0 for i,w in enumerate(a["benefit"])]
            value,_,_=enumerate_optimum(subproblem)
            component_values.append(value)
        records.append({"budget":b,"numerator":optimum,"denominator":100,
                        "image_numerators":component_values,
                        "selected_options":selected,"feasible_deployments":count})
    assert records[5]["numerator"]==1425
    a["budget"]=5
    (REV/"figures/worked_example.json").write_text(json.dumps({"input":a,"budget_optima":records},indent=2)+"\n",encoding="utf-8")
    return records

def node(ax,xy,label,color="#DCE6EF",width=.17,height=.105,fontsize=8):
    x,y=xy
    ax.add_patch(FancyBboxPatch((x-width/2,y-height/2),width,height,
                 boxstyle="round,pad=0.012,rounding_size=0.018",facecolor=color,
                 edgecolor="#566573",linewidth=.7,zorder=3))
    ax.text(x,y,label,ha="center",va="center",fontsize=fontsize,zorder=4)

def edge(ax,a,b,highlight=False):
    ax.plot([a[0],b[0]],[a[1],b[1]],color="#BD6B36" if highlight else "#80909B",
            lw=1.6 if highlight else .8,zorder=1)

def main():
    records=example()
    plt.rcParams.update({"font.family":"DejaVu Sans","font.size":8,"pdf.fonttype":42,
                         "svg.fonttype":"none","axes.spines.top":False,"axes.spines.right":False})
    fig=plt.figure(figsize=(7.01,5.35),facecolor="white")
    grid=fig.add_gridspec(2,2,left=.035,right=.985,top=.945,bottom=.055,wspace=.12,hspace=.27)
    axes=[fig.add_subplot(grid[i,j]) for i,j in [(0,0),(0,1),(1,0),(1,1)]]
    titles=["Failure domains","Image contents","Whole zone failure lattice","Exact budget allocation"]
    for letter,ax,title in zip("abcd",axes,titles):
        ax.set(xlim=(-.03,1.03),ylim=(0,1));ax.axis("off")
        ax.text(0,1.04,letter,weight="bold",fontsize=10,transform=ax.transAxes)
        ax.text(.075,1.04,title,fontsize=9,weight="bold",transform=ax.transAxes)
    ax=axes[0]
    root=(.5,.86);zones=[(.16,.57),(.5,.57),(.84,.57)]
    colors=["#D6E4ED","#DAE6D6","#F1E0CE"]
    for k,(zone,color) in enumerate(zip(zones,colors)):
        edge(ax,root,zone);node(ax,zone,"ABC"[k],color)
        for j,offset in enumerate([-.085,.085]):
            leaf=(zone[0]+offset,.24);edge(ax,zone,leaf)
            node(ax,leaf,f"{'abc'[k]}{j+1}",color,width=.105,height=.08,fontsize=7.5)
    node(ax,root,"root",width=.19)
    ax.text(.5,.02,"One domain outage removes all descendant locations",ha="center",fontsize=6.8)
    ax=axes[1]
    root=(.5,.87);u1=(.27,.58);u2=(.79,.58)
    edge(ax,root,u1);edge(ax,root,u2)
    node(ax,root,"virtual root",width=.28,color="#EEEEEE")
    node(ax,u1,r"image $u_1$",width=.25);node(ax,u2,r"image $u_2$",width=.25,color="#DAE6D6")
    for pos,label,color in [((.12,.23),r"$f_1\ (8)$","#D6E4ED"),((.42,.23),r"$f_2\ (5)$","#D6E4ED"),((.79,.23),r"$f_3\ (3)$","#DAE6D6")]:
        edge(ax,u1 if pos[0]<.6 else u2,pos);node(ax,pos,label,color,width=.2)
    ax.text(.5,.02,"Edges show containment and values give fragment benefits",ha="center",fontsize=6.5)
    ax=axes[2]
    positions={0:(.5,.08),1:(.16,.34),2:(.5,.34),4:(.84,.34),3:(.16,.66),5:(.5,.66),6:(.84,.66),7:(.5,.92)}
    labels={0:r"$\bot$",1:"[A]",2:"[B]",4:"[C]",3:"[A,B]",5:"[A,C]",6:"[B,C]",7:"[root]"}
    for s in positions:
        for bit in [1,2,4]:
            if not s&bit:edge(ax,positions[s],positions[s|bit],(s,s|bit) in [(1,3),(2,3),(3,7)])
    for s,pos in positions.items():node(ax,pos,labels[s],"#F1E0CE" if s in [3,7] else "#EDF1F4",width=.22,height=.085,fontsize=7.5)
    ax=axes[3]
    table_rows=[]
    for b in range(6):
        g1,g2=records[b]["image_numerators"]
        table_rows.append([str(b),f"{g1/100:.2f}",f"{g2/100:.2f}",f"{records[b]['numerator']/100:.2f}"])
    table=ax.table(cellText=table_rows,colLabels=["Budget",r"$u_1$",r"$u_2$","Combined"],
                   cellLoc="center",colLoc="center",bbox=[.03,.20,.94,.76])
    table.auto_set_font_size(False);table.set_fontsize(7.5)
    for (r,c),cell in table.get_celld().items():
        cell.set_edgecolor("#D5DDE1");cell.set_linewidth(.5)
        if r==0:cell.set_facecolor("#E8EEF2");cell.set_text_props(weight="bold")
        if r==6:cell.set_facecolor("#F3E2CF")
    ax.text(.5,.105,r"$B=5,\quad 4\rightarrow u_1,\quad 1\rightarrow u_2$",ha="center",fontsize=8)
    ax.text(.5,.025,r"$13\times0.90+3\times0.85=14.25$",ha="center",fontsize=8,color="#8E4A23")
    output=REV/"manuscript/source/figures/fig_system_overview"
    for extension in ["pdf","svg","png"]:
        fig.savefig(output.with_suffix('.'+extension),dpi=220,facecolor="white")
    plt.close(fig)
    print(json.dumps({"figure":str(output),"budget5_optimum":records[5],"data":"figures/worked_example.json"}))

if __name__=="__main__":main()
