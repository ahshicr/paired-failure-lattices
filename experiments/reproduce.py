"""Portable Linux entry points for full scientific reproduction (no SSH required)."""
import argparse,os,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parent

def run(args):subprocess.run([str(x) for x in args],cwd=ROOT,check=True)

def main():
    p=argparse.ArgumentParser();p.add_argument('study',choices=['build','qa','end_to_end','supplemental','public','trace','scopes','projections']);p.add_argument('--workers',type=int,default=1);p.add_argument('--compiler',default='g++');p.add_argument('--output',default='reproduced');p.add_argument('--scope-source');a=p.parse_args()
    if sys.platform!='linux':raise SystemExit('Use Linux/WSL for reproducible RLIMIT_AS and RSS semantics; unit tests can also run directly on other platforms.')
    os.environ.update(OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1',MKL_NUM_THREADS='1');py=sys.executable;out=pathlib.Path(a.output).resolve();native=ROOT/'bin/revision_benchmark'
    if a.study=='build':
        (ROOT/'bin').mkdir(exist_ok=True)
        for name in ('revision_benchmark','representation_benchmark'):run([a.compiler,'-O3','-DNDEBUG','-std=c++17','-I','include','src/pfl.cpp',f'src/{name}.cpp','-o',f'bin/{name}'])
    elif a.study=='qa':
        run([py,'tests/test_record_isolation.py']);run([py,'tests/test_exactness.py','--binary',native,'--output',out/'native_exactness','--cases','144']);run([py,'tests/test_scopes.py','--output',out/'scope_exactness','--cases','120']);run([py,'tests/test_envelope_bound.py','--output',out/'envelope_bound','--cases','96']);run([py,'tests/test_laminar_projection.py'])
    elif a.study=='end_to_end':run([py,'run_matrix.py','--matrix','configs/end_to_end.json','--binary',native,'--output',out/'end_to_end_v1','--workers',a.workers])
    elif a.study=='supplemental':run([py,'run_supplemental.py','--output',out/'supplemental_v1','--workers',a.workers])
    elif a.study=='public':run([py,'run_public_workflows.py','--output',out/'public_workflows_v1','--workers',a.workers])
    elif a.study in ('trace','scopes'):run([py,'run_field_studies.py','--study','real_cloud' if a.study=='trace' else 'scopes','--output',out/'field_studies_v1','--binary',native,'--workers',a.workers])
    else:
        if not a.scope_source:raise SystemExit('--scope-source must name a completed scope study (including inputs, records, and summary.csv).')
        run([py,'run_laminar_projections.py','--scope-source',pathlib.Path(a.scope_source).resolve(),'--output',out/'laminar_projections_v1','--binary',native,'--workers',a.workers])

if __name__=='__main__':main()
