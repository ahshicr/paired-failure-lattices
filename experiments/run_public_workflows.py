"""All 180 public workflows, all 3 declared manifests, all 3 repetitions."""
import argparse,hashlib,json,math,pathlib,random
from instance_io import generate,leaf_masks,write_input
from run_matrix import digest
from study_runner import execute,summary

def main():
    p=argparse.ArgumentParser();p.add_argument('--matrix',default='configs/public_workflows.json');p.add_argument('--output',default='results/public_workflows_v1');p.add_argument('--workers',type=int,default=1);args=p.parse_args()
    config=json.loads(pathlib.Path(args.matrix).read_text());root=pathlib.Path('data/public_workflows');index=json.loads((root/'index.json').read_text());out=pathlib.Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True);(out/'records').mkdir(exist_ok=True)
    assert index['workflow_count']==config['workflow_count'];binary=str(pathlib.Path('bin/revision_benchmark').resolve());bsha=digest(binary);csha=digest(args.matrix);tasks=[]
    for r in index['records']:
        if r['mapping'] not in config['mappings']:continue
        source=root/r['path'];assert digest(source)==r['sha256'];fields={line.split()[0]:line.split()[1:] for line in source.read_text().splitlines() if line.strip()}
        parent=list(map(int,fields['parent']));benefit=list(map(int,fields['benefit']));payload=list(map(int,fields['fragment_output_bytes']));assert len(parent)==r['nodes']
        subtree=payload.copy()
        # Frozen conversions use a stable topological order; validate before summing.
        assert parent[0]==-1 and all(0<=parent[v]<v for v in range(1,len(parent)))
        for v in reversed(range(1,len(parent))):subtree[parent[v]]+=subtree[v]
        for rep in range(config['replicates']):
            seed=int(hashlib.sha256((r['source_sha256']+f':{rep}').encode()).hexdigest()[:15],16)
            x=generate(leaves=config['domain_leaves'],nodes=2,budget=config['budget'],options=0,events=config['event_identities'],topology='random',seed=seed)
            _,leaves=leaf_masks(x['domain_parent']);rng=random.Random(seed+1);menus=[]
            for v in range(len(parent)):
                menu=[{'cost':0,'replicas':[]}]
                if v and subtree[v]>0:
                    for copies in config['replicas_by_option']:
                        chosen=sorted(rng.sample(leaves,copies));cost=max(1,(64*subtree[v]*copies+max(subtree[0],1)-1)//max(subtree[0],1));menu.append({'cost':cost,'replicas':chosen})
                menus.append(menu)
            x.update(parent=parent,benefit=benefit,menus=menus)
            case=f"pub_{r['mapping']}_{r['source_sha256'][:12]}_S{rep}";path=out/'inputs'/(case+'.txt');sha=write_input(path,x)
            for method in config['methods']:
                ident=case+'__'+method
                tasks.append({'case_id':case,'task_id':ident,'method':method,'mapping':r['mapping'],'workflow_id':r['workflow_id'],'workflow_source_sha256':r['source_sha256'],'structure_sha256':r['sha256'],'replicate':rep,'seed':seed,'leaves':config['domain_leaves'],'nodes':len(parent),'budget':config['budget'],'replicas':'mixed_1_4','topology':'random','binary':binary,'binary_sha256':bsha,'input':str(path),'input_sha256':sha,'matrix_sha256':csha,'record':str(out/'records'/(ident+'.json')),'wall_seconds':config['wall_seconds_per_task'],'address_space_bytes':config['address_space_bytes_per_task']})
    assert len(tasks)==config['workflow_count']*len(config['mappings'])*config['replicates']*len(config['methods'])
    execute(tasks,out,config,args.workers);print(json.dumps(summary(tasks,out,exact_methods=('paired_dense','paired_sparse','scenario','replica'))),flush=True)

if __name__=='__main__':main()
