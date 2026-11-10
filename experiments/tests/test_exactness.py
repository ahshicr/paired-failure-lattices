"""Independent exhaustive deployment checks; never used as scientific benchmarks."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from instance_io import generate, write_input, enumerate_optimum, evaluate

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cases", type=int, default=144)
    args = parser.parse_args()
    binary = pathlib.Path(args.binary).resolve()
    out = pathlib.Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = []
    start = time.monotonic()
    for case in range(args.cases):
        higher = case % 4 == 3
        a = generate(leaves=2 + case % 7, nodes=1 + case % 6,
                     budget=[0,1,3,7,12,20][case % 6], replicas=1 + case % 4,
                     options=1 + case % 3, events=3 + case % 19,
                     topology=["balanced","star","random"][case % 3], seed=900000 + case,
                     duplicate_fraction=0.5, higher_order=higher, test_boundaries=True)
        path = out / f"case_{case:04d}.txt"
        digest = write_input(path, a)
        best, _, feasible = enumerate_optimum(a)
        methods = ["scenario","replica","greedy","paired_envelope"] if higher else ["paired_dense","paired_sparse","scenario","replica","explicit","greedy"]
        for method in methods:
            variants = [[], ["no_compression"]] if method != "greedy" else [[]]
            if method in ("paired_dense", "explicit"):
                variants.append(["full_states"])
            for variant in variants:
                result = subprocess.run([str(binary), str(path), method, *variant], capture_output=True, text=True, timeout=60)
                if result.returncode:
                    raise RuntimeError(f"case={case}, method={method}, variant={variant}: {result.stdout} {result.stderr}")
                r = json.loads(result.stdout)
                value, cost = evaluate(a, r["selection"])
                assert value == int(r["objective_numerator"]), (case, method, "reconstruction")
                assert cost == r["cost"] and cost <= a["budget"], (case, method, "budget")
                if method not in ("greedy", "paired_envelope"):
                    assert value == best, (case, method, variant, value, best)
                else:
                    assert value <= best, (case, method, "exceeds exact optimum")
                if method == "paired_envelope":
                    assert int(r["model_objective_numerator"]) <= value, (case, "envelope must be conservative")
                records.append({"case": case, "input_sha256": digest, "method": method,
                                "variant": variant, "feasible_deployments": feasible,
                                "enumerated_optimum": str(best), "result": r})
        if (case + 1) % 12 == 0:
            print(json.dumps({"cases_done": case + 1, "comparisons": len(records)}), flush=True)
    report = {"status": "PASS", "cases": args.cases, "comparisons": len(records),
              "elapsed_seconds": time.monotonic() - start,
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "records": records}
    (out / "exactness_report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items() if k != "records"}))

if __name__ == "__main__":
    main()
