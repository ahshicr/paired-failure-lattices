#!/bin/bash
set -euo pipefail
case "$(pwd -P)" in /share/*) ;; *) echo 'The study must run under /share'; exit 10;; esac
mkdir -p bin results tmp
export TMPDIR="$(pwd -P)/tmp"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
if ! type module >/dev/null 2>&1; then source /etc/profile.d/modules.sh; fi
module load gcc/13.2.0
g++ --version > results/compiler.txt
/usr/bin/time -v -o results/build.time g++ -O3 -DNDEBUG -std=c++17 -I include src/pfl.cpp src/revision_benchmark.cpp -o bin/revision_benchmark
sha256sum bin/revision_benchmark > results/binary.sha256
python_bin=/share/apps/miniconda3/envs/tj.pytorch2.0.0/bin/python
"$python_bin" tests/test_exactness.py --binary bin/revision_benchmark --output results/native_exactness --cases 144
"$python_bin" run_matrix.py --matrix configs/end_to_end.json --binary bin/revision_benchmark --output results/end_to_end_v1 --workers "${SLURM_CPUS_PER_TASK:-1}"
