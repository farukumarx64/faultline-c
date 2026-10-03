#!/usr/bin/env python3
"""Render this demo's recorded terminal output as a GIF; requires Pillow.

Uses the real output timestamps, with no speedup or invented result text. This is
an offline terminal replay, not a screen capture of an interactive desktop app.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
CLEAR = '\x1b[2J\x1b[H'


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_recording(path):
    records = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
    header, events = records[0], records[1:]
    if header.get('version') != 2 or header.get('width') != 112 or header.get('height') != 30:
        raise ValueError('expected the recovery demo\'s asciicast v2 format (112 x 30)')
    if not events or events[-1][2] != '':
        raise ValueError('recording has no closing timestamp; it may be incomplete')
    frames, previous = [(0.0, '')], 0.0
    for timestamp, kind, data in events:
        if kind != 'o' or not math.isfinite(timestamp) or timestamp < previous:
            raise ValueError('invalid output event or non-monotonic timestamp')
        previous = timestamp
        if not data:
            continue
        if not data.startswith(CLEAR) or '\x1b' in data[len(CLEAR):]:
            raise ValueError('unsupported terminal escapes; use output from demos/recovery.py')
        screen = data[len(CLEAR):].replace('\r\n', '\n')
        if len(screen.splitlines()) > 30 or any(len(line) > 112 for line in screen.splitlines()):
            raise ValueError('terminal output exceeds the declared dimensions')
        frames.append((timestamp, screen))
    if events[-1][0] <= frames[-1][0] or events[-1][0] > 125:
        raise ValueError('recording must have a positive final hold and be at most 125 seconds')
    return header, frames, events[-1][0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path, help='recovery.cast beside its successful summary.json')
    parser.add_argument('--output', type=Path, required=True, help='new .gif; also writes sibling .cast and .json')
    parser.add_argument('--font', type=Path, help='monospaced .ttf/.ttc; auto-detects Menlo or DejaVu Sans Mono')
    args = parser.parse_args(argv)
    try:
        from PIL import Image, ImageDraw, ImageFont, __version__ as pillow_version
        source, output = args.recording.resolve(), args.output.resolve()
        if source.name != 'recovery.cast' or output.suffix != '.gif':
            raise ValueError('input must be recovery.cast and output must have the .gif suffix')
        directory = source.parent
        summary = json.loads((directory / 'summary.json').read_text())
        manifest = json.loads((directory / 'manifest.json').read_text())
        if (summary['verdict'] != 'RECOVERY_DEMO_PASS' or summary['exit_code'] != 0 or
                not summary['cleanup']['ok'] or summary['cleanup']['remaining'] or
                not all(c['reaped'] and c['group_retired'] for c in summary['children'])):
            raise ValueError('refusing to publish a failed or incompletely cleaned-up demo')
        header, screens, ended = load_recording(source)
        target = summary['target']
        status = summary['accounting']['jobs'][0]['terminal_status']
        final = screens[-1][1]
        if ('5 / VERIFIED: THE SAME JOB COMPLETED' not in final or
                any(f'{key}={value}\n' not in final for key, value in status.items())):
            raise ValueError('recorded final output disagrees with the verified terminal status')
        cast_out, report_out = output.with_suffix('.cast'), output.with_suffix('.json')
        for dest in (output, report_out, cast_out):
            if dest != source and dest.exists():
                raise ValueError(f'refusing to overwrite evidence: {dest}')
        font_path = args.font
        if font_path is None:
            font_path = next((p for p in map(Path, [
                '/System/Library/Fonts/Menlo.ttc',
                '/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf']) if p.is_file()), None)
        if font_path is None:
            raise ValueError('a monospace font is required; pass --font /path/to/font.ttf')
        font = ImageFont.truetype(str(font_path), 18)
        width = math.ceil((font.getlength('M') * header['width'] + 64) / 2) * 2
        height, line_height = 812, 24
        images, durations = [], []
        for index, (started, screen) in enumerate(screens):
            end = screens[index + 1][0] if index + 1 < len(screens) else ended
            duration = max(10, round((end - started) * 100) * 10)
            canvas = Image.new('RGB', (width, height), '#0b1220')
            draw = ImageDraw.Draw(canvas)
            draw.rounded_rectangle((12, 12, width - 13, height - 13), radius=16, fill='#101b2c', outline='#334155')
            draw.text((32, 24), 'recovery.cast  /  real CLI output, real time', font=font, fill='#94a3b8')
            draw.line((24, 56, width - 25, 56), fill='#334155')
            for row, line in enumerate(screen.splitlines()):
                color = '#e2e8f0'
                if line.startswith('#') or set(line) == {'-'}:
                    color = '#94a3b8'
                if line.startswith('$'):
                    color = '#c4b5fd'
                if 'kill -KILL' in line or 'HARD CRASH' in line:
                    color = '#fda4af'
                if line.startswith('state=RUNNING'):
                    color = '#fcd34d'
                if line.startswith(('FAULTLINE', 'job_id=', 'worker_id=', 'attempt=', 'retries=')):
                    color = '#67e8f9'
                if line.startswith(('state=DONE', 'result=', '# PASS', '# Cleanup', '5 / VERIFIED')):
                    color = '#6ee7b7'
                draw.text((32, 72 + row * line_height), line, font=font, fill=color)
            images.append(canvas)
            durations.append(duration)
        output.parent.mkdir(parents=True, exist_ok=True)
        images[0].save(output, format='GIF', save_all=True, append_images=images[1:],
                       duration=durations, loop=0, disposal=2, optimize=False)
        if cast_out != source:
            shutil.copyfile(source, cast_out)
        report = dict(format='faultline-recovery-demo-capture-v1', created_utc=manifest['created_utc'],
            verdict=summary['verdict'], target=target, terminal_status=status,
            counts=summary['accounting']['counts'], attempts=summary['totals']['job_attempts_total'],
            retries=summary['totals']['job_retries_total'],
            cleanup=dict(ok=True, reaped_children=len(summary['children']), remaining=[]),
            source=manifest['source'], demo_source_sha256=manifest['demo_source_sha256'],
            binary_sha256={name: info['sha256'] for name, info in manifest['binaries'].items()},
            transitions=summary['transitions'],
            recording=dict(type='rendered asciicast v2 terminal replay', time_compressed=False,
                original_duration_seconds=ended, gif_duration_ms=sum(durations),
                timestamp_rounding='GIF centiseconds', dimensions=[width, height],
                pillow=pillow_version, font=font_path.name, gif_sha256=sha256(output),
                cast_sha256=sha256(cast_out)),
            evidence_directory=str(directory.relative_to(ROOT)) if directory.is_relative_to(ROOT) else str(directory),
            evidence_sha256={name: sha256(directory / name) for name in
                ('manifest.json', 'summary.json', 'events.jsonl', 'recovery-history.json', 'coordinator.wal')})
        report_out.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(f'Rendered {output}: {sum(durations)/1000:.2f}s, {width}x{height}, {output.stat().st_size} bytes')
        print(f'Replay: {cast_out}\nEvidence: {report_out}')
        return 0
    except (ImportError, OSError, ValueError, KeyError, IndexError) as error:
        print(f'render failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
