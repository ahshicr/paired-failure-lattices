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
g++ -O3 -DNDEBUG -std=c++17 -I include src/pfl.cpp src/representation_benchmark.cpp -o bin/representation_benchmark
sha256sum bin/* > results/binaries.sha256
python_bin=/share/apps/miniconda3/envs/tj.pytorch2.0.0/bin/python
"$python_bin" tests/test_record_isolation.py > results/record_isolation_qa.txt
"$python_bin" tests/test_exactness.py --binary bin/revision_benchmark --output results/native_exactness --cases 144
"$python_bin" run_supplemental.py --workers "${SLURM_CPUS_PER_TASK:-1}"
