#!/usr/bin/env python3
"""Summarize MATTER_GEOMETRY_PAGES_PROFILE windows without adding overlapping stages."""
import argparse
import json
import re
from pathlib import Path


def summarize(text):
    stages = {}
    io = dict(requests=0, hits=0, reads=0, bytes=0, cpu_payload_peak=0, prefetched=0)
    queues = {}
    bank = {}
    windows = 0
    for line in text.splitlines():
        match = re.search(r'paging_(stage|io|queue|bank)\s+(.*)', line)
        if not match:
            continue
        kind, fields = match.groups()
        fields = dict(re.findall(r'(\w+)=([^\s]+)', fields))
        if kind == 'stage':
            name = fields['name']
            stage = stages.setdefault(name, dict(count=0, total_ms=0., max_ms=0.))
            stage['count'] += int(fields['count'])
            stage['total_ms'] += float(fields['total_ms'])
            stage['max_ms'] = max(stage['max_ms'], float(fields['max_ms']))
        elif kind == 'io':
            windows += 1
            for key in ('requests', 'hits', 'reads', 'bytes'):
                io[key] += int(fields[key])
            io['prefetched'] += int(fields.get('prefetched', 0))
            io['cpu_payload_peak'] = max(io['cpu_payload_peak'], int(fields['cpu_payload_bytes']))
        elif kind == 'bank':
            for key in ('capacity', 'occupied', 'backing_allocations'):
                bank[key + '_peak'] = max(bank.get(key + '_peak', 0), int(fields[key]))
            bank['largest_free_last'] = int(fields['largest_free'])
            bank['occupied_last'] = int(fields['occupied'])
        else:
            for key, value in fields.items():
                if key in ('reads', 'decode_queue', 'completions', 'prepared', 'uploads'):
                    key += '_sampled_peak'
                    queues[key] = max(queues.get(key, 0), int(value))
                else:
                    queues[key] = queues.get(key, 0) + int(value)
    for stage in stages.values():
        stage['mean_ms'] = stage['total_ms'] / stage['count'] if stage['count'] else 0.
    return dict(windows=windows, stages=stages, io=io, queues=queues, bank=bank)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='one editor/controller log (avoid concatenating duplicate logs)')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    result = summarize(args.log.read_text(encoding='utf-8', errors='replace'))
    if not result['windows']:
        parser.error('no paging profile windows found; enable MATTER_GEOMETRY_PAGES_PROFILE=1')
    if args.json:
        print(json.dumps(result, indent=2))
        return
    print(f"Profile windows: {result['windows']}")
    print('Stage                      samples       total ms      mean ms       max ms')
    for name, stage in result['stages'].items():
        print(f"{name:26} {stage['count']:8d} {stage['total_ms']:14.3f} {stage['mean_ms']:12.3f} {stage['max_ms']:12.3f}")
    print('I/O:', json.dumps(result['io'], sort_keys=True))
    print('CPU payload bank:', json.dumps(result['bank'], sort_keys=True))
    print('Queues/counters:', json.dumps(result['queues'], sort_keys=True))
    print('Stages overlap; do not sum them. Wait times are per-page elapsed latency, not CPU/GPU execution time.')
    print('Page reads are per batch and include validation. Disk bytes count storage API reads, which may hit the OS cache.')
    print('Queue peaks are sampled at log windows. Unfinished requests and the unlogged tail are excluded from timing totals.')


if __name__ == '__main__':
    main()
