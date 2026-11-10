"""Independently check statistical populations, estimands and reported values."""
import argparse
import hashlib
import json
import math
import pathlib
import re

import numpy as np
import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = {}


def source(relative):
    path = ROOT / relative
    SOURCES[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return path


def csv(relative):
    return pd.read_csv(source(relative), dtype={
        'objective_numerator': 'string', 'model_objective_numerator': 'string',
        'numerator': 'string'})


def read(relative):
    return json.loads(source(relative).read_text(encoding='utf-8'))


def close(a, b):
    assert np.allclose(a, b, rtol=1e-11, atol=1e-12), (a, b)


def paired(frame, method, numerator, denominator, measurement):
    good = frame[frame.status == 'COMPLETE']
    table = good.pivot(index='case_id', columns=method, values=measurement)
    table = table[[numerator, denominator]].dropna()
    assert (table > 0).all().all()
    return table[numerator] / table[denominator]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--evidence-only', action='store_true', help='Check numerical evidence without manuscript text.')
    args = parser.parse_args()
    supplemental = csv('results/supplemental_verified/results/supplemental_v1/summary.csv')
    synthetic = csv('results/end_to_end_verified/results/end_to_end_v1/summary.csv')
    public = csv('results/public_verified/results/public_workflows_v1/summary.csv')
    trace = csv('results/fields_verified/results/field_studies_v1/real_cloud/summary.csv')
    scopes = csv('results/fields_verified/results/field_studies_v1/scopes/summary.csv')
    restrictions = csv('results/projections_verified/results/laminar_projections_v1/summary.csv')
    frames = {'representation_and_ablations': supplemental, 'synthetic': synthetic,
              'public': public, 'trace': trace, 'scopes': scopes,
              'restrictions': restrictions}
    populations = {}
    for name, frame in frames.items():
        assert frame.task_id.is_unique
        populations[name] = {'tasks': len(frame), 'cases': frame.case_id.nunique(),
                             'statuses': frame.status.value_counts().to_dict()}
    assert sum(len(frame) for frame in frames.values()) == 39520
    assert supplemental.groupby('study').size().to_dict() == {
        'ablations': 2400, 'representation': 4800}
    assert synthetic.case_id.nunique() == 972
    assert public.case_id.nunique() == 1620 and public.workflow_id.nunique() == 180
    assert public.groupby('workflow_id').case_id.nunique().eq(9).all()
    assert trace.case_id.nunique() == 1280
    assert trace.groupby(['population', 'scope_interpretation', 'conditioning']).case_id.nunique().eq(160).all()
    assert scopes[scopes.method_label == 'scope_integer'].groupby('nodes').size().eq(480).all()

    for frame in (supplemental, synthetic, public):
        frame['full_ms'] = frame.read_ms + frame.end_to_end_ms
    native = synthetic[synthetic.status == 'COMPLETE']
    phase_sum = native.preprocessing_ms + native.planning_reconstruction_ms
    # Native fields are printed with six significant digits.
    assert (native.end_to_end_ms + 2e-5 * native.end_to_end_ms >= phase_sum).all()
    ip = scopes[scopes.method_label == 'scope_integer']
    assert (ip.end_to_end_seconds >= ip.preprocessing_seconds + ip.solve_seconds).all()
    objective = ip.objective_numerator.map(int).astype(float)
    calculated_gap = np.maximum(0, (ip.best_bound - objective) / np.maximum(1, np.abs(objective)))
    close(calculated_gap, ip.relative_bound_gap)
    assert (ip.loc[ip.status == 'OPTIMAL', 'relative_bound_gap'] == 0).all()
    sizes = {int(n): {'all_cases': len(g), 'optimal': int((g.status == 'OPTIMAL').sum()),
                     'median_gap_all_cases': float(g.relative_bound_gap.median()),
                     'median_model_and_search_seconds': float(g.end_to_end_seconds.median())}
             for n, g in ip.groupby('nodes')}
    assert f"{sizes[128]['median_gap_all_cases']:.3f}" == '2.806'

    effects = read('scientific_revision/paired_effects.json')
    query = supplemental[(supplemental.study == 'representation') & (supplemental.events == 32768)]
    ablation = supplemental[supplemental.study == 'ablations']
    selections = {
        'query_effect': (query, 'method_label'),
        'reachable_effect': (ablation, 'method_label'),
        'menu_effect': (ablation[ablation.duplicate_fraction == .8], 'method_label'),
        'synthetic_effect': (synthetic, 'method'),
        'public_effect': (public, 'method')}
    checks, pvalues = [], []
    for row in effects['contrasts']:
        frame, method = selections[row['key']]
        ratios = paired(frame, method, row['numerator'], row['denominator'], row['measurement'])
        units = frame.groupby('case_id')[row['block_unit']].first().reindex(ratios.index)
        groups = [ratios[units == unit].to_numpy() for unit in sorted(units.unique())]
        medians = np.array([np.median(group) for group in groups])
        positive, negative = int((medians > 1).sum()), int((medians < 1).sum())
        n = positive + negative
        pvalue = min(1.0, 2 * sum(math.comb(n, i) for i in range(min(positive, negative) + 1)) / 2**n)
        rng = np.random.default_rng(row['random_initialization'])
        draws = rng.integers(0, len(groups), size=(10000, len(groups)))
        bootstrap = [np.median(np.concatenate([groups[i] for i in sample])) for sample in draws]
        interval = np.quantile(bootstrap, [.025, .975])
        assert len(ratios) == row['paired_cases'] and len(groups) == row['blocks']
        close(np.median(ratios), row['median_ratio'])
        close(interval, row['confidence_interval_95'])
        assert math.isclose(pvalue, row['two_sided_sign_p'], rel_tol=1e-12)
        retained = csv('scientific_revision/' + row['key'] + '_pairs.csv').set_index('case_id')
        assert set(retained.index) == set(ratios.index)
        close(retained.loc[ratios.index].ratio, ratios)
        assert retained.loc[ratios.index].block.astype(str).tolist() == units.astype(str).tolist()
        checks.append({'contrast': row['key'], 'matched_cases': len(ratios), 'blocks': len(groups),
                       'median_of_instance_ratios': float(ratios.median()),
                       'confidence_interval_95': interval.tolist(),
                       'positive_blocks': positive, 'negative_blocks': negative,
                       'two_sided_sign_p': pvalue})
        pvalues.append(pvalue)
    running = 0
    for rank, i in enumerate(np.argsort(pvalues)):
        running = max(running, min(1, (len(pvalues) - rank) * pvalues[i]))
        assert math.isclose(running, effects['contrasts'][i]['holm_adjusted_p'], rel_tol=1e-12)
        checks[i]['holm_adjusted_p'] = running
    assert checks[0]['matched_cases'] == 480

    query_ordering = []
    for leaves, group in query.groupby('leaves'):
        ratios = paired(group, 'method_label', 'scenario', 'paired_dense', 'query_ms')
        assert len(ratios) == 60
        query_ordering.append({'leaves': int(leaves), 'cases': len(ratios),
                               'median_scenario_over_paired': float(ratios.median()),
                               'paired_faster_cases': int((ratios > 1).sum())})
    assert len(query_ordering) == 8
    assert all((row['median_scenario_over_paired'] > 1) == (row['leaves'] >= 128)
               for row in query_ordering)
    faster = {}
    for name, frame in [('synthetic', synthetic), ('public', public)]:
        ratios = paired(frame, 'method', 'paired_dense', 'scenario', 'full_ms')
        faster[name] = {'scenario_faster': int((ratios > 1).sum()),
                        'paired_faster': int((ratios < 1).sum()),
                        'ties': int((ratios == 1).sum()), 'matched': len(ratios)}
    assert faster['synthetic']['scenario_faster'] == 967
    assert faster['public']['scenario_faster'] == 1620

    cua = read('data/processed/cua_replay.json')
    parent = cua['hierarchy']['parent']
    leaves = cua['hierarchy']['leaf_ids']
    masks = [0] * len(parent)
    for bit, leaf in enumerate(leaves):
        node = leaf
        while node >= 0:
            masks[node] |= 1 << bit
            node = parent[node]
    paired_masks = {0, *masks}
    paired_masks.update(a | b for a in masks for b in masks)
    for row in cua['statistics']:
        key = '/'.join(map(str, (row['population'], row['scope'], row['year'], row['conditioning'])))
        events = cua['distributions'][key]
        footprints = []
        for event in events:
            assert event['mass'] > 0
            mask = 0
            for node in event['domains']:
                mask |= masks[node]
            footprints.append(mask)
        assert len(set(footprints)) == len(events) == row['unique_failed_regions']
        duration = sum(event['mass'] for event in events)
        outside = sum(event['mass'] for event, mask in zip(events, footprints) if mask not in paired_masks)
        assert duration == row['total_seconds'] and outside == row['higher_order_seconds']
        close(outside / duration, row['higher_order_fraction'])

    projection = csv('results/analysis/projection_bounds.csv')
    for policy in ('small_first', 'large_first', 'benefit_first', 'best_restriction'):
        nonzero = projection.upper_bound > 0
        close(projection.loc[nonzero, policy] / projection.loc[nonzero, 'upper_bound'],
              projection.loc[nonzero, policy + '_ratio_lower'])
        close(projection.loc[nonzero, policy] / projection.loc[nonzero, 'lower_bound'],
              projection.loc[nonzero, policy + '_ratio_upper'])
    assert (projection.lower_bound >= projection.ip_incumbent).all()
    assert (projection.lower_bound >= projection.best_restriction).all()

    # Keep the audited timing and generation definitions attached to their code.
    for relative in ('experiments/src/revision_benchmark.cpp',
                     'experiments/src/representation_benchmark.cpp',
                     'experiments/instance_io.py', 'experiments/run_field_studies.py',
                     'experiments/run_public_workflows.py', 'experiments/scope_solver.py'):
        source(relative)
    first = effects['contrasts'][0]
    if not args.evidence_only:
        for relative in ('manuscript/source/main.tex', 'manuscript/source/sections/computational_evaluation.tex',
                         'manuscript/source/sections/conclusion.tex',
                         'manuscript/source/tables/paired_effects.tex',
                         'manuscript/source/tables/public_results.tex',
                         'manuscript/source/tables/projection_results.tex',
                         'supplement.tex', 'response.tex', 'assess.tex'):
            text = source(relative).read_text(encoding='utf-8')
            assert not re.search(r'QUERY_(?:MEDIAN|LOW|HIGH)|duplicate fraction|duplicate fractions', text)
            if relative in ('response.tex', 'supplement.tex'):
                assert '2.95' not in text and '2.89 to 2.99' not in text
        manuscript = re.sub(r'\s+', ' ', (ROOT/'manuscript/source/sections/computational_evaluation.tex').read_text())
        assert f"{first['median_ratio']:.2f}, with interval {first['confidence_interval_95'][0]:.2f} to {first['confidence_interval_95'][1]:.2f}" in manuscript
    report = {'status': 'PASS', 'manuscript_text_checked': not args.evidence_only, 'terminal_tasks': 39520, 'populations': populations,
              'estimand': 'Median of ratios formed within matched completed instances.',
              'intervals': '10000 bootstrap samples of complete realization or workflow blocks.',
              'tests': 'Two sided exact binomial sign counts on block medians, with Holm adjustment.',
              'independently_recomputed_contrasts': checks,
              'query_hierarchy_ordering': query_ordering,
              'complete_planning_direction_counts': faster, 'scope_gap_populations': sizes,
              'trace_duration_and_unique_set_checks': len(cua['statistics']),
              'projection_ratio_intervals_verified': len(projection),
              'definition_corrections': [
                  'Native time ends before verification. Process startup belongs to task wall time.',
                  'Solver time covers model construction and search. Extraction and verification follow.',
                  'Duplicate and overlap controls are injection probabilities.',
                  'Public repetitions generate new failure and replica realizations.',
                  'Independent marginals are computed from fitted event masses before joint sampling.',
                  'Trace support size counts distinct failed location sets, including an empty set when present.',
                  'The pooled large-event query contrast includes all eight hierarchy sizes.',
                  'Optimality gaps include certified zero values and describe bounds on a feasible objective.'],
              'source_sha256': SOURCES}
    path = ROOT / 'scientific_revision/statistical_scope_audit.json'
    path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'status': report['status'], 'tasks': report['terminal_tasks'],
                      'contrasts': len(checks), 'query_pairs': first['paired_cases'],
                      'duration_strata': len(cua['statistics'])}))


if __name__ == '__main__':
    main()
