#!/bin/bash
set -euo pipefail
case "$(pwd -P)" in /share/*) ;; *) exit 10;; esac
mkdir -p bin results tmp
export TMPDIR="$(pwd -P)/tmp"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
if ! type module >/dev/null 2>&1; then source /etc/profile.d/modules.sh; fi
module load gcc/13.2.0
g++ --version > results/compiler.txt
g++ -O3 -DNDEBUG -std=c++17 -I include src/pfl.cpp src/revision_benchmark.cpp -o bin/revision_benchmark
sha256sum bin/* > results/binaries.sha256
python_bin=/share/apps/miniconda3/envs/tj.pytorch2.0.0/bin/python
"$python_bin" tests/test_laminar_projection.py > results/projection_qa.txt
"$python_bin" run_laminar_projections.py --workers "${SLURM_CPUS_PER_TASK:-1}" --scope-source /share/home/u24134/data/codex-jobs/tcc-r1-fields-20261109/runs/all-trace-and-scope-v2-20261109/results/field_studies_v1/scopes
