#!/usr/bin/env python3
"""Read-only original-file import/compile census, not musical fidelity scoring."""
import argparse
import collections
import concurrent.futures
import hashlib
import json
import pathlib
import re
import subprocess
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('corpus', type=pathlib.Path)
    ap.add_argument('report', type=pathlib.Path)
    ap.add_argument('--build', type=pathlib.Path, default=pathlib.Path('build'))
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    if args.report.exists():
        ap.error('report already exists; preserve previous evidence')
    binary = (args.build / 'daw_performance_import').resolve()
    files = sorted(args.corpus.rglob('*.musicxml'))
    if not files or not binary.is_file() or not 1 <= args.jobs <= 8:
        ap.error('need a built importer, nonempty corpus, and 1..8 jobs')
    started = time.time()
    def check(path):
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        start = time.monotonic()
        try:
            proc = subprocess.run([str(binary), '--check', str(path)], text=True, capture_output=True, timeout=30)
            output = proc.stdout + proc.stderr
            code = proc.returncode
        except subprocess.TimeoutExpired:
            output, code = 'FAIL stage=timeout reason=30 seconds exceeded', -1
        match = re.search(r'FAIL stage=(\S+) reason=([^\n]+)', output)
        unchanged = sha == hashlib.sha256(path.read_bytes()).hexdigest()
        return dict(file=str(path.relative_to(args.corpus)), sha256=sha, unchanged=unchanged,
                    exit_code=code, stage='pass' if code == 0 else match[1] if match else 'process_failure',
                    reason='' if code == 0 else match[2] if match else output.strip(),
                    elapsed_seconds=time.monotonic()-start, output=output)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(check, files))
    report = dict(scope='original_files_no_normalization; acceptance_not_fidelity', started_unix=started,
                  elapsed_seconds=time.time()-started, source_root=str(args.corpus.resolve()),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  total=len(results), stages=dict(collections.Counter(r['stage'] for r in results)),
                  failures=dict(collections.Counter(r['reason'] for r in results if r['exit_code'])),
                  all_sources_unchanged=all(r['unchanged'] for r in results), files=results)
    with args.report.open('x') as out:
        json.dump(report, out, ensure_ascii=False, indent=2)
        out.write('\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('files','source_root')}, ensure_ascii=False, indent=2))
    if not report['all_sources_unchanged']:
        raise SystemExit('source hash changed during scan')

if __name__ == '__main__':
    main()
