#!/usr/bin/env python3
"""Plot published power-scaling measurements; requires optional Matplotlib.

This reads the reviewed JSON report and never runs a benchmark. Dots retain all
five measurements; connected markers show medians, with warmups excluded.
"""

import argparse
from datetime import date
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output-prefix', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    if report['verdict'] != 'POWER_COMPARISON_PASS' or report['workload']['profile'] != 'scaling-prime-count-v1':
        parser.error('expected a reviewed power-scaling comparison')

    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    styles = [('ac', 'AC · normal', '#0072B2', 'o', -.14),
              ('battery', 'Battery · normal', '#009E73', 's', 0),
              ('battery-low-power', 'Battery · Low Power Mode', '#D55E00', '^', .14)]
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 11,
        'axes.spines.top': False, 'axes.spines.right': False,
        'axes.edgecolor': '#cbd1d8', 'text.color': '#172d3e',
        'axes.labelcolor': '#405567', 'xtick.color': '#405567', 'ytick.color': '#405567',
        'figure.facecolor': 'white', 'axes.facecolor': 'white',
        'svg.fonttype': 'none', 'svg.hashsalt': 'faultline-power-scaling'})
    fig, axes = plt.subplots(1, 2, figsize=(12, 5.6))
    panels = [('batch_elapsed_ms', 'Batch completion time', 'Seconds · lower is better', .001),
              ('completed_jobs_per_second', 'Completed throughput', 'Jobs / second · higher is better', 1)]
    for ax, (metric, title, ylabel, scale) in zip(axes, panels):
        for name, label, color, marker, offset in styles:
            medians = []
            for x, workers in enumerate(('1', '2', '4', '8')):
                group = report['campaigns'][name]['aggregate'][workers][metric]
                values = group['values']
                if len(values) != 5:
                    parser.error('expected all five measured repetitions')
                ax.scatter([x + offset + (i - 2) * .026 for i in range(5)],
                           [value * scale for value in values], s=22, marker=marker,
                           color=color, alpha=.55, edgecolors='none', zorder=3)
                medians.append(group['median'] * scale)
            ax.plot([x + offset for x in range(4)], medians, color=color,
                    marker=marker, markersize=5, linewidth=1.8, zorder=4)
        ax.set_title(title, loc='left', fontweight='bold', pad=14)
        ax.set_xticks(range(4), ['1', '2', '4', '8'])
        ax.set_xlabel('Workers')
        ax.set_ylabel(ylabel)
        ax.set_xlim(-.4, 3.4)
        ax.set_ylim(0, ax.get_ylim()[1] * 1.06)
        ax.grid(axis='y', color='#e8edf1', linewidth=.7, zorder=0)
    fig.suptitle('Faultline · CPU scaling across power modes', x=.075, y=.98,
                 ha='left', fontsize=20, fontweight='bold')
    machine = report['campaigns']['ac']['machine']['hardware']['machdep.cpu.brand_string']
    campaign_date = date.fromisoformat(report['publication']['campaign_date_utc']).strftime('%B %d, %Y')
    subtitle = f'{report["workload"]["jobs"]} prime-count jobs per run · {machine} · optimized build · {campaign_date}'
    fig.text(.075, .90, subtitle,
             fontsize=11, color='#536171')
    handles = [Line2D([0], [0], color=color, marker=marker, label=label, linewidth=1.8)
               for _, label, color, marker, _ in styles]
    fig.legend(handles=handles, loc='lower center', bbox_to_anchor=(.5, .095),
               ncol=3, frameon=False, fontsize=10)
    fig.text(.075, .025, 'Dots: all five measured runs. Connected markers: medians. Warmups excluded.\n'
             'Separate campaigns; thermal telemetry unavailable. Performance measurements do not establish energy savings.',
             fontsize=9, color='#536171')
    fig.subplots_adjust(left=.075, right=.98, top=.80, bottom=.26, wspace=.25)
    args.output_prefix.parent.mkdir(parents=True, exist_ok=True)
    for extension in ('svg', 'png'):
        path = args.output_prefix.with_suffix('.' + extension)
        options = {'metadata': {'Date': None}} if extension == 'svg' else {'dpi': 160}
        fig.savefig(path, **options)
        if extension == 'svg':
            path.write_text('\n'.join(line.rstrip() for line in path.read_text().splitlines()) + '\n')
        print(path)
    plt.close(fig)


if __name__ == '__main__':
    main()
