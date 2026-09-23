#!/usr/bin/env python3
"""Offline, amplitude-relative SWAM onset observations; never a track-delay setter.

48 kHz stereo f32le; NoteOn at 1 s, NoteOff at 4 s. Trailing 5 ms stereo RMS,
sampled at 1 ms hops. Onset is the END of the first window of 10 consecutive
windows >= 0.1 reference amplitude (-20 dB). Confirmation is 9 ms later.
References: peak envelope in [0,1] s after NoteOn (all articulations); raw RMS
in [0.8,1.3] s (late-window proxy, NOT proof of a steady state). Plucks have no
steady state; their second metric must not be called a steady-state onset.
"""
import argparse
from array import array
import csv
import hashlib
import json
from pathlib import Path
import math
import sys


def crossing(envelope, reference, pre_rms):
    if reference < 1e-7:
        return None
    threshold = reference * 0.1
    if pre_rms >= threshold:
        return None  # onset cannot be distinguished from preceding sound
    for i in range(len(envelope) - 9):
        if all(v >= threshold for _, v in envelope[i:i + 10]):
            return envelope[i][0]
    return None


def measure(samples):
    if len(samples) != 240000 * 2 or not all(math.isfinite(x) for x in samples):
        raise ValueError("expected finite 5-second stereo 48 kHz capture")
    # One bin contains both channels of one millisecond. RMS never averages L/R
    # before squaring (which could cancel an out-of-phase stereo signal).
    bins = [sum(x*x for x in samples[i:i+96])/96 for i in range(0, len(samples), 96)]
    envelope = [(end - 1000, math.sqrt(sum(bins[end-5:end])/5)) for end in range(1005, 3001)]
    peak = max(v for t, v in envelope if t <= 1000)
    late = math.sqrt(sum(bins[1800:2300])/500)
    pre = math.sqrt(sum(bins[500:1000])/500)
    return dict(peak_reference_rms=peak, late_reference_rms=late, preceding_rms=pre,
                peak_relative_ms=crossing(envelope, peak, pre),
                late_relative_ms=crossing(envelope, late, pre))


def self_test():
    def step(ms, gain=1):
        data = array('f', [0]) * (240000*2)
        for i in range((1000+ms)*96, 192000*2):
            data[i] = gain if i % 2 else -gain
        return measure(data)
    a, b, c = step(40), step(57), step(40, .01)
    assert a['peak_relative_ms'] == 41 and b['peak_relative_ms'] == 58
    assert c['peak_relative_ms'] == a['peak_relative_ms']
    assert a['late_relative_ms'] == 41
    assert measure(array('f', [0])*(240000*2))['peak_relative_ms'] is None
    assert crossing([(i, .1) for i in range(9)], 1, 0) is None
    assert crossing([(i, .1) for i in range(10)], 1, .1) is None
    print('PASS onset analyzer: timing, scale invariance, stereo polarity, silence, persistence and background rejection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--verify', action='store_true', help='recompute and compare existing JSON without changing it')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.directory:
        parser.error('capture directory required')
    entries = list(csv.DictReader((args.directory/'manifest.tsv').open(), delimiter='\t'))
    if len(entries) != 48 or len({e['file'] for e in entries}) != 48:
        raise ValueError('incomplete/duplicate 48-case capture manifest')
    results = []
    for entry in entries:
        path = args.directory/entry['file']
        if Path(entry['file']).name != entry['file']:
            raise ValueError('invalid capture filename')
        raw = path.read_bytes()
        samples = array('f'); samples.frombytes(raw)
        if sys.byteorder != 'little':
            samples.byteswap()
        result = {**entry, **measure(samples), 'sha256': hashlib.sha256(raw).hexdigest()}
        for metric in ('peak_relative_ms', 'late_relative_ms'):
            result[metric+'_minus_reported_latency'] = None if result[metric] is None else result[metric] - float(entry['latency_seconds'])*1000
        results.append(result)
    output = args.directory/'attack-analysis.json'
    serialized = json.dumps(dict(method=__doc__, results=results), indent=2)+'\n'
    if args.verify:
        if output.read_text() != serialized:
            raise ValueError('recomputed analysis differs from retained JSON')
    else:
        if output.exists():
            raise ValueError('analysis output already exists; use --verify to compare')
        output.write_text(serialized)
    print('| setting | peak-relative t20 ms | t20 minus reported 20 ms | late-window-relative t20 ms |')
    print('|---|---:|---:|---:|')
    for case in dict.fromkeys(e['case'] for e in results):
        rows = [r for r in results if r['case'] == case]
        def extent(key):
            values = [r[key] for r in rows if r[key] is not None]
            span = f'{min(values):g}–{max(values):g}' if values else 'unresolved'
            return span + (f' ({len(values)}/4)' if len(values) != 4 else '')
        print(f"| {case} | {extent('peak_relative_ms')} | {extent('peak_relative_ms_minus_reported_latency')} | {extent('late_relative_ms')} |")
    print('coverage=12_settings_2_pitches_2_repetitions; no universal attack constant; no automatic track delay')


if __name__ == '__main__':
    main()
