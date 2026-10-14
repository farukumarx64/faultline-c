#!/usr/bin/env python3
"""Audit three matching campaigns and write separate power-mode comparison tables.

Consumes retained raw campaign directories; never merges partial or failed runs.
Standard library only. Historical published reports remain separate context.
"""

import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys

sys.dont_write_bytecode = True
import run_baseline as base
import run_recovery as recovery
from run_batch import RunFailure, require
import power


def read(path):
    return json.loads(path.read_text())


def clean_result(result):
    require(result['exit_code'] == 0 and result['cleanup']['ok'] and
            not result['cleanup']['remaining'] and not result['cleanup']['errors'],
            'failed execution or cleanup cannot enter a comparison')
    require(result['children'] and all(c['reaped'] and c['group_retired'] for c in result['children']),
            'missing process cleanup evidence')


def audit_jobs(sample, profile):
    accounting = sample['accounting']
    require(accounting['verified'] is True, 'job accounting was not verified')
    ids = accounting['submitted_ids']
    require(len(ids) == len(set(ids)) == profile['jobs'] and
            all(type(i) is int and i > 0 for i in ids), 'missing or duplicate job IDs')
    require(sorted(accounting['completed_ids']) == sorted(ids) and
            accounting['terminally_failed_ids'] == [], 'unaccounted job IDs')
    jobs = accounting['jobs']
    require(len(jobs) == len(ids) and sorted(j['submission']['job_id'] for j in jobs) == sorted(ids),
            'per-job evidence differs from acknowledged IDs')
    retries = 0
    for job in jobs:
        submitted, status = job['submission'], job['terminal_status']
        require(all(submitted[k] == profile[k] for k in ('task', 'arguments', 'max_retries')),
                'job definition differs from profile')
        require(status['job_id'] == str(submitted['job_id']) and status['state'] == 'DONE' and
                status['failure'] == 'NONE' and status['result'] == json.dumps(profile['expected_result']) and
                status['result_bytes'] == str(profile['expected_result_bytes']), 'incorrect final result')
        used, limit = map(int, status['retries'].split('/'))
        require(limit == profile['max_retries'] and 0 <= used <= limit and
                int(status['attempt']) == used + 1, 'invalid retries or attempt')
        retries += used
    if profile['profile'] == 'recovery-sleep-v1':
        faults = int(sample['scenario'] != 'no_fault')
        require(retries == sample['metrics']['retries'] == faults and
                sample['metrics']['attempts'] == profile['jobs'] + retries and
                sample['coverage']['injected_faults'] == faults, 'recovery accounting/coverage differs')
        if faults:
            require(sample['coverage']['recovery_demonstrated'] is True, 'recovery not demonstrated')
            for metric, value in recovery.recovery_delays(sample['fault']).items():
                require(sample['metrics'][metric] == value, 'recovery timing differs from fault evidence')
    return ids


def audit_campaign(directory, name):
    directory = directory.resolve()
    require(not (directory/'INVALID').exists(), 'campaign was manually invalidated')
    summary, profile = read(directory/'summary.json'), read(directory/'profile.json')
    clean_result(summary)
    is_recovery = profile['profile'] == 'recovery-sleep-v1'
    require(profile == (recovery.load_profile() if is_recovery else base.load_profile()),
            'unsupported or changed workload profile')
    expected_power = power.load_profile(name)
    require(summary['power_profile'] == read(directory/'power-profile.json') == expected_power,
            'campaign power profile mismatch')
    require(summary.get('reportable_recovery_campaign' if is_recovery else 'reportable_scaling_campaign') is True,
            'only complete reportable campaigns are comparable')
    require(summary['verdict'] == ('RECOVERY_PASS' if is_recovery else 'SCALING_PASS'), 'wrong campaign verdict')
    provenance, build = read(directory/'provenance.json'), read(directory/'build.json')
    base.require_clean_source(provenance['git_status'])
    require(not provenance['source_patch'].strip() and re.fullmatch(r'[0-9a-f]{40,64}', provenance['revision']),
            'missing clean source revision')
    require(provenance['source_inputs'] and provenance['power_profile'] == expected_power,
            'missing source/power provenance')
    for filename, checksum in provenance['source_inputs'].items():
        path = directory/'source-inputs'/filename
        require(path.resolve().is_relative_to((directory/'source-inputs').resolve()), 'unsafe source path')
        require(base.digest(path) == checksum, 'saved source inputs changed')
    require(build['cflags'] == profile['build']['cflags'] and build['sanitize'] == 0 and
            build['build']['exit_code'] == build['correctness']['exit_code'] == 0, 'invalid build/correctness evidence')
    require(set(build['binaries']) == set(base.PROGRAMS) and
            {n: base.digest(directory/'bin'/n) for n in base.PROGRAMS} == build['binaries'], 'binaries changed')
    if is_recovery:
        require(provenance['recovery_regressions']['exit_code'] == 0, 'recovery regression failure')
    plan = recovery.sample_plan(profile) if is_recovery else base.sample_plan(profile, True)
    require(read(directory/'plan.json')['runs'] == plan and len(summary['runs']) == len(plan),
            'incomplete or changed sample plan')
    samples, observations, hashes = [], [], {}
    reference_machine = None
    paths = set()
    for entry, expected in zip(summary['runs'], plan):
        require(all(entry[k] == v for k, v in expected.items()), 'sample order/configuration mismatch')
        require(Path(entry['path']).name == entry['path'] and entry['path'] not in ('.', '..') and entry['path'] not in paths,
                'invalid or duplicate sample directory')
        paths.add(entry['path'])
        path = directory/entry['path']
        require(path.resolve().is_relative_to(directory), 'sample path escapes campaign')
        require(not (path/'INVALID').exists(), 'sample was manually invalidated')
        sample, manifest = read(path/'summary.json'), read(path/'manifest.json')
        clean_result(sample)
        require(sample['power_profile'] == manifest['power_profile'] == expected_power and
                manifest['profile'] == profile, 'mixed sample power/workload profiles')
        require(manifest['source'] == dict(revision=provenance['revision'], dirty=False), 'sample source changed')
        ids = audit_jobs(sample, profile)
        timing, latencies = read(path/'timings.json'), read(path/'latencies.json')
        require(0 < timing['t0_ns'] <= timing['t_ack_ns'] <= timing['t_done_ns'] <= timing['verified_ns'],
                'invalid timing boundaries')
        require(sample['metrics']['batch_elapsed_ms'] == (timing['t_done_ns']-timing['t0_ns'])/1e6,
                'batch metric differs from timing evidence')
        require(sample['metrics']['completed_jobs_per_second'] == profile['jobs']*1000/sample['metrics']['batch_elapsed_ms'],
                'throughput metric differs from timing evidence')
        require(sorted(j['job_id'] for j in latencies['jobs']) == sorted(ids), 'latency job IDs differ')
        values = []
        for job in latencies['jobs']:
            value = job['completed_ms'] - job['submitted_ms']
            require(value == job['accepted_latency_ms'] and value >= 0, 'invalid job latency')
            values.append(value)
        require(sample['metrics']['mean_accepted_latency_ms'] == statistics.mean(values) and
                sample['metrics']['p95_accepted_latency_ms'] == sorted(values)[math.ceil(.95*len(values))-1],
                'latency metrics differ from per-job evidence')
        machine = read(path/'machine.json')
        for moment in ('before', 'after'):
            raw = read(path/f'{moment}-power.json')
            require(raw['profile'] == expected_power and 'error' not in raw, 'invalid raw power evidence')
            observed = power.interpret_probe(raw, expected_power)
            require(observed == machine[moment]['power'] == raw['power'], 'power observation changed')
            power.validate_observation(observed, expected_power,
                                       initial=expected['campaign_index'] == 0 and moment == 'before')
            identity = base.machine_identity(machine[moment])
            if reference_machine is None:
                reference_machine = identity
            require(identity == reference_machine, 'machine changed during campaign')
        observations.append(dict(**expected, before=machine['before']['power'], after=machine['after']['power']))
        for filename in ('summary.json', 'manifest.json', 'machine.json', 'before-power.json', 'after-power.json',
                         'accounting.json', 'timings.json', 'latencies.json', 'events.jsonl', 'coordinator.wal'):
            hashes[f'{entry["path"]}/{filename}'] = base.digest(path/filename)
        samples.append(sample)
    aggregate = recovery.aggregate(samples, profile) if is_recovery else base.scaling_aggregate(samples, profile)
    require(aggregate == summary['aggregate'], 'campaign aggregate differs from samples')
    require(summary['measured_samples'] == (15 if is_recovery else 20), 'wrong measured sample count')
    for filename in ('summary.json', 'profile.json', 'power-profile.json', 'provenance.json', 'build.json', 'plan.json'):
        hashes[filename] = base.digest(directory/filename)
    return dict(directory=str(directory), workload=profile, power_profile=expected_power,
                source_revision=provenance['revision'], source_inputs=provenance['source_inputs'],
                build={k:build[k] for k in ('compiler','compiler_version','make_version','cflags','sanitize')},
                binaries=build['binaries'], machine=reference_machine, aggregate=aggregate,
                observations=observations, sha256=hashes)


def compare(campaigns):
    require(set(campaigns) == set(power.NAMES), 'need AC, battery, and battery-low-power campaigns')
    control = campaigns['ac']
    for name, candidate in campaigns.items():
        require(candidate['power_profile'] == power.load_profile(name), 'wrong power-profile label')
        for field in ('workload', 'source_revision', 'source_inputs', 'build', 'machine'):
            require(candidate[field] == control[field], f'cannot isolate power mode: {field} differs')
    is_recovery = control['workload']['profile'] == 'recovery-sleep-v1'
    configurations = list(recovery.SCENARIOS) if is_recovery else ['1','2','4','8']
    rows = []
    for configuration in configurations:
        def metrics(name):
            item = campaigns[name]['aggregate'][configuration]
            return item['metrics'] if is_recovery else item
        for name in power.NAMES:
            for metric, value in metrics(name).items():
                if not isinstance(value, dict) or 'median' not in value:
                    continue
                baseline = metrics('ac')[metric]['median']
                battery = metrics('battery')[metric]['median']
                rows.append(dict(configuration=configuration, power_profile=name, metric=metric,
                    median=value['median'], minimum=value['minimum'], maximum=value['maximum'],
                    samples=value['values'],
                    change_vs_ac_percent=100*(value['median']/baseline-1) if baseline > 0 else None,
                    change_vs_battery_percent=100*(value['median']/battery-1) if battery > 0 else None))
    return dict(report='faultline-power-comparison-v1', verdict='POWER_COMPARISON_PASS',
                workload=control['workload'], campaigns=campaigns, comparisons=rows,
                limitations=['Separate campaigns, not randomized paired power-mode trials.',
                    'Before/after observations do not prove continuous power or thermal stability.',
                    'Battery percentage is a coarse condition check, not an energy-efficiency measurement.',
                    'Historical AC reports use a different revision and are context, not controlled samples.'])


def write_report(directory, report):
    directory.mkdir(parents=True, exist_ok=False)
    (directory/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    rows = [{k:v for k,v in row.items() if k != 'samples'} for row in report['comparisons']]
    with (directory/'comparison.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    lines = ['# Power-mode comparison', '', 'All three complete campaigns passed the evidence checks.', '',
             f'Source revision: `{report["campaigns"]["ac"]["source_revision"]}`.', '',
             'Values are medians [minimum–maximum] across five measured repetitions; warmups are excluded.', '',
             '| Configuration | Power | Batch time (ms) | Jobs/s | Mean latency (ms) | Batch change vs AC |',
             '| --- | --- | --- | --- | --- | --- |']
    groups = {}
    for row in report['comparisons']:
        groups.setdefault((row['configuration'], row['power_profile']), {})[row['metric']] = row
    for (configuration, name), metrics in groups.items():
        def cell(metric):
            v = metrics[metric]
            return f'{v["median"]:.3f} [{v["minimum"]:.3f}–{v["maximum"]:.3f}]'
        lines.append(f'| {configuration} | {name} | {cell("batch_elapsed_ms")} | '
                     f'{cell("completed_jobs_per_second")} | {cell("mean_accepted_latency_ms")} | '
                     f'{metrics["batch_elapsed_ms"]["change_vs_ac_percent"]:+.2f}% |')
    lines += ['', 'Positive batch-time change means slower completion. Full metrics, including recovery',
              'timings where applicable, are in comparison.csv and comparison.json.', '']
    lines += ['- '+text for text in report['limitations']]
    (directory/'comparison.md').write_text('\n'.join(lines)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in power.NAMES:
        parser.add_argument('--'+name, type=Path, required=True, help='retained raw campaign directory')
    parser.add_argument('--output-dir', type=Path, required=True, help='new comparison directory')
    args = parser.parse_args()
    try:
        campaigns = {name:audit_campaign(getattr(args, name.replace('-', '_')), name) for name in power.NAMES}
        write_report(args.output_dir, compare(campaigns))
    except (OSError, ValueError, KeyError, TypeError, RunFailure) as error:
        print(f'comparison rejected: {error}', file=sys.stderr)
        return 1
    print(f'POWER_COMPARISON_PASS: {args.output_dir}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
