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
sha256sum bin/revision_benchmark > results/binary.sha256
python_base=/share/apps/miniconda3/envs/tj.pytorch2.0.0/bin/python
"$python_base" -m venv field_env
field_env/bin/python -m pip install --no-index --find-links wheels ortools==9.15.6755 > results/dependency_install.log
field_env/bin/python -m pip freeze > results/dependencies.txt
field_env/bin/python tests/test_record_isolation.py > results/record_isolation_qa.txt
field_env/bin/python tests/test_scopes.py --output results/scope_exactness --cases 120
field_env/bin/python run_field_studies.py --study real_cloud --workers "${SLURM_CPUS_PER_TASK:-1}"
field_env/bin/python run_field_studies.py --study scopes --workers "${SLURM_CPUS_PER_TASK:-1}"
