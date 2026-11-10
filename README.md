# Implicit Paired Failure Lattices

Research code and experimental evidence for *Implicit Paired Failure
Lattices for Exact Recovery over Hierarchical Workflow Images*.

Authors are Weian Guo, Tinglong Zhang and Wuzhao Li.
Wuzhao Li is the corresponding author.

## Download the experiment artifact

[Release v20261110](https://github.com/ahshicr/paired-failure-lattices/releases/tag/v20261110) contains the complete experiment archive,
including source code, input data, retained execution records and analysis
instructions. The large archive is distributed as a release attachment.
It can be downloaded without a GitHub account.

The small source tree in this repository supports browsing the code.
Download and extract the release artifact before running the experiments
or analyses because it supplies the data and historical execution records.

## Verify the download

Compare the archive's SHA-256 with SHA256SUMS.txt on the release page.
After extraction, PACKAGE_SHA256_MANIFEST.json identifies the files inside.
The five compressed full-run archives have additional checksums in
evidence/archive_manifest.json.

## Reproduce the analysis

Follow REPRODUCE.txt for the recorded environments, full reruns and figure
generation. For a numerical check using the included summary tables:

~~~sh
python -m pip install -r requirements-analysis.txt
python scripts/audit_statistical_reporting.py --evidence-only
~~~

This checks all 39,520 terminal task records and independently recomputes
the five paired comparisons. Full run verification and figure generation
use the retained archives described in REPRODUCE.txt.

## Scientific contents

The artifact contains the native recovery implementations and comparators,
the arbitrary-scope solver, six study summaries, five retained full-run
archives, frozen execution sources and binaries, original parallel
measurements, derived plotting tables, and blocked statistical analyses.
Experimental inputs, measured results and the historical execution
archives are preserved without alteration.

## Public source data

The Cloud Uptime Archive release is identified by
[doi:10.5281/zenodo.14712442](https://doi.org/10.5281/zenodo.14712442).
Its included source metadata identifies an MIT licence.
The WfInstances release is identified by
[doi:10.5281/zenodo.12510982](https://doi.org/10.5281/zenodo.12510982).
Its included LGPL licence applies to the upstream workflow data.
These source records and licences are retained in the experiment artifact.

## Version

Version v20261110 identifies the artifact used for the November 2026
revision. Subsequent changes should use a new release version so that this
record remains distinguishable from later experimental results.
