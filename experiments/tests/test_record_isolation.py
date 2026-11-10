"""Regression: concurrent decimal-valued task IDs must keep independent evidence."""
import concurrent.futures,json,pathlib,sys,tempfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]))
from run_matrix import run_task,digest

def main():
    with tempfile.TemporaryDirectory(prefix='pfl-record-isolation-') as tmp:
        root=pathlib.Path(tmp);script=root/'worker.py';script.write_text('import sys,json,time\ntime.sleep(0.05)\nprint(json.dumps({"status":"COMPLETE","objective_numerator":sys.argv[1]}))\n')
        tasks=[]
        for i in range(24):
            ident=f'case_D0.8_S{i:02d}__method';tasks.append({'task_id':ident,'case_id':ident,'method':'test','binary':sys.executable,'binary_sha256':digest(sys.executable),'input':str(script),'input_sha256':digest(script),'matrix_sha256':'regression','record':str(root/(ident+'.json')),'command':[sys.executable,str(script),str(i)],'wall_seconds':10,'address_space_bytes':5368709120})
        with concurrent.futures.ProcessPoolExecutor(max_workers=8) as pool:results=list(pool.map(run_task,tasks))
        outputs=[];starts=list(root.glob('*.start.json'))
        for i,t in enumerate(tasks):
            r=json.loads(pathlib.Path(t['record']).read_text());assert r['status']=='COMPLETE';assert r['result']['objective_numerator']==str(i);outputs.append(r['stdout']);assert digest(root/r['stdout'])==r['stdout_sha256']
        assert len(set(outputs))==24 and len(starts)==24
        print('PASS: 24 concurrent decimal task IDs, independent outputs and hashes')

if __name__=='__main__':main()
