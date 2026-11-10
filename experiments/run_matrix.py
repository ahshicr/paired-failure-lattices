"""Execute every declared case with bounded resources and durable per-task evidence."""
from __future__ import annotations
import argparse
import concurrent.futures
import csv
import hashlib
import itertools
import json
import os
import pathlib
import platform
import random
import signal
import subprocess
import sys
import time
import uuid
from collections import Counter, defaultdict
from instance_io import generate, write_input

def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def atomic_json(path, value):
    path = pathlib.Path(path)
    temp = path.with_name(path.name + ".tmp-" + uuid.uuid4().hex)
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temp, path)

def process_tree_rss(pid):
    if sys.platform != "linux":
        return 0
    todo, seen, rss = [pid], set(), 0
    while todo:
        current = todo.pop()
        if current in seen:
            continue
        seen.add(current)
        try:
            text = pathlib.Path(f"/proc/{current}/status").read_text()
            for line in text.splitlines():
                if line.startswith("VmRSS:"):
                    rss += int(line.split()[1]) * 1024
            todo.extend(map(int, pathlib.Path(f"/proc/{current}/task/{current}/children").read_text().split()))
        except (FileNotFoundError, ProcessLookupError):
            pass
    return rss

def run_task(task):
    record_path = pathlib.Path(task["record"])
    if record_path.exists():
        old = json.loads(record_path.read_text())
        for field in ("input_sha256", "binary_sha256", "matrix_sha256"):
            if old[field] != task[field]:
                raise RuntimeError("Cannot reuse evidence with different " + field)
        return {"task_id": task["task_id"], "status": old["status"], "reused": True}
    attempt = uuid.uuid4().hex[:12]
    prefix = record_path.parent / (task["task_id"] + "_" + attempt)
    # Task IDs legitimately contain decimals. Replacing a pathlib suffix would
    # truncate the ID and collide across repetitions; append every extension.
    stdout_path, stderr_path = pathlib.Path(str(prefix)+".stdout"), pathlib.Path(str(prefix)+".stderr")
    start = time.monotonic()
    record = {k:v for k,v in task.items() if k not in ("record",)}
    record.update({"attempt": attempt, "host": platform.node(), "slurm_job_id": os.environ.get("SLURM_JOB_ID"),
                   "slurm_account": os.environ.get("SLURM_JOB_ACCOUNT"), "started_unix": time.time(), "status": "RUNNING"})
    atomic_json(pathlib.Path(str(prefix)+".start.json"), record)
    def limits():
        import resource
        size = task["address_space_bytes"]
        resource.setrlimit(resource.RLIMIT_AS, (size, size))
        seconds = task["wall_seconds"]
        resource.setrlimit(resource.RLIMIT_CPU, (seconds + 5, seconds + 10))
    timed_out, peak = False, 0
    with stdout_path.open("wb") as out, stderr_path.open("wb") as err:
        command = task.get("command") or [task["binary"], task["input"], task["method"], *task.get("variant", [])]
        process = subprocess.Popen(command,
                                   stdout=out, stderr=err, start_new_session=True,
                                   preexec_fn=limits if sys.platform == "linux" else None)
        while process.poll() is None:
            peak = max(peak, process_tree_rss(process.pid))
            if time.monotonic() - start > task["wall_seconds"]:
                timed_out = True
                if sys.platform == "linux":
                    os.killpg(process.pid, signal.SIGTERM)
                else:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    if sys.platform == "linux":
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                process.wait()
                break
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
    record.update({"elapsed_seconds": time.monotonic()-start, "exit_code": process.returncode,
                   "observed_process_tree_peak_rss_bytes": peak, "stdout": stdout_path.name,
                   "stderr": stderr_path.name, "stdout_sha256": digest(stdout_path), "stderr_sha256": digest(stderr_path)})
    raw = stdout_path.read_text(encoding="utf-8", errors="replace").strip()
    try:
        result = json.loads(raw)
    except json.JSONDecodeError:
        result = None
    if timed_out:
        record["status"] = "TIMEOUT"
    elif result is not None:
        record["status"], record["result"] = result["status"], result
        if process.returncode != 0 and record["status"] == "COMPLETE":
            raise RuntimeError("Nonzero exit with COMPLETE result")
    else:
        record["status"] = "PROCESS_FAILURE"
    atomic_json(record_path, record)
    return {"task_id": task["task_id"], "status": record["status"], "reused": False}

def build_cases(config, out):
    cases = []
    factors = itertools.product(config["domain_leaves"], config["recovery_nodes"], config["replica_counts"], range(config["replicates"]))
    for leaves,nodes,replicas,replicate in factors:
        seed = config["seed_start"] + replicate
        topology = config["topology_by_replicate"][replicate % len(config["topology_by_replicate"]) ]
        a = generate(leaves=leaves,nodes=nodes,budget=64,replicas=replicas,options=config["menu_options"],
                     events=config["event_identities"],topology=topology,seed=seed)
        for budget in config["budgets"]:
            a["budget"] = budget
            ident = f"L{leaves}_N{nodes}_B{budget}_R{replicas}_S{replicate:02d}"
            path = out / "inputs" / (ident + ".txt")
            sha = write_input(path, a)
            cases.append({"case_id": ident,"input":str(path.resolve()),"input_sha256":sha,
                          "leaves":leaves,"nodes":nodes,"budget":budget,"replicas":replicas,
                          "replicate":replicate,"seed":seed,"topology":topology})
    return cases

def summarize(out, tasks):
    statuses = Counter()
    exact = defaultdict(set)
    methods = defaultdict(dict)
    rows = []
    for task in sorted(tasks,key=lambda x:x["task_id"]):
        record = json.loads(pathlib.Path(task["record"]).read_text())
        statuses[record["status"]] += 1
        row = {k:record[k] for k in ("task_id","case_id","method","leaves","nodes","budget","replicas","replicate","topology","status","elapsed_seconds")}
        result = record.get("result", {})
        row.update({k:v for k,v in result.items() if k not in ("selection","method","status")})
        if record["status"] == "COMPLETE":
            value = int(result["objective_numerator"])
            methods[record["case_id"]][record["method"]] = value
            if record["method"] not in ("greedy","paired_envelope"):
                exact[record["case_id"]].add(value)
        rows.append(row)
    mismatches = {k:sorted(v) for k,v in exact.items() if len(v)>1}
    for case, values in methods.items():
        if exact.get(case) and "greedy" in values and values["greedy"] > min(exact[case]):
            mismatches[case] = "greedy exceeds exact optimum"
    fields = list(dict.fromkeys(k for row in rows for k in row))
    with (out / "summary.csv").open("w",newline="",encoding="utf-8") as handle:
        writer=csv.DictWriter(handle,fieldnames=fields);writer.writeheader();writer.writerows(rows)
    audit={"expected_tasks":len(tasks),"terminal_tasks":len(rows),"statuses":dict(statuses),
           "exact_objective_mismatches":mismatches,"cases_with_exact_results":len(exact),
           "scientific_equality_pass":not mismatches,"all_tasks_attempted":len(rows)==len(tasks)}
    atomic_json(out / "analysis.json",audit)
    if mismatches:
        raise RuntimeError("Exact method mismatch; inspect analysis.json")
    atomic_json(out / "complete.json",{**audit,"completed_unix":time.time(),"summary_sha256":digest(out / "summary.csv")})
    return audit

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--matrix",required=True);parser.add_argument("--binary",required=True)
    parser.add_argument("--output",required=True);parser.add_argument("--workers",type=int,default=1)
    args=parser.parse_args();out=pathlib.Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True);(out/"records").mkdir(exist_ok=True)
    config=json.loads(pathlib.Path(args.matrix).read_text());binary=str(pathlib.Path(args.binary).resolve())
    cases=build_cases(config,out);binary_sha=digest(binary);matrix_sha=digest(args.matrix);tasks=[]
    for case in cases:
        for method in config["methods"]:
            ident=case["case_id"]+"__"+method
            tasks.append({**case,"task_id":ident,"method":method,"binary":binary,"binary_sha256":binary_sha,
                          "matrix_sha256":matrix_sha,"record":str(out/"records"/(ident+".json")),
                          "wall_seconds":config["wall_seconds_per_task"],"address_space_bytes":config["address_space_bytes_per_task"]})
    random.Random(config["task_order_seed"]).shuffle(tasks)
    atomic_json(out/"run_manifest.json",{"config":config,"matrix_sha256":matrix_sha,"binary_sha256":binary_sha,
                "cases":len(cases),"tasks":len(tasks),"workers":args.workers,"host":platform.node(),
                "slurm_job_id":os.environ.get("SLURM_JOB_ID"),"python":sys.version,"task_order":[t["task_id"] for t in tasks]})
    print(json.dumps({"cases":len(cases),"tasks":len(tasks),"workers":args.workers}),flush=True)
    counts=Counter()
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures={pool.submit(run_task,t):t for t in tasks}
        for i,future in enumerate(concurrent.futures.as_completed(futures),1):
            record=future.result();counts[record["status"]]+=1
            if i%20==0 or i==len(tasks):
                progress={"terminal_tasks":i,"total_tasks":len(tasks),"statuses":dict(counts),"updated_unix":time.time()}
                atomic_json(out/"progress.json",progress);print(json.dumps(progress),flush=True)
    print(json.dumps(summarize(out,tasks)),flush=True)

if __name__ == "__main__":
    main()
