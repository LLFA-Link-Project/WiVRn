#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Embed source revisions, diffs and patch hashes in the recording owner."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def git(path, *args):
    result = subprocess.run(['git', '-C', str(path), *args], capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else None


def repository(path):
    return dict(path=str(path), revision=(git(path, 'rev-parse', 'HEAD') or '').strip() or None,
                tracked_diff=git(path, 'diff', '--binary', 'HEAD'),
                status=git(path, 'status', '--short', '--untracked-files=no'))


def main():
    wivrn, monado, survive, output = map(Path, sys.argv[1:])
    patches = {str(p.relative_to(wivrn)): hashlib.sha256(p.read_bytes()).hexdigest()
               for directory in ('monado', 'libsurvive')
               for p in sorted((wivrn/'patches'/directory).glob('*.patch'))}
    result = dict(schema_version=1, wivrn=repository(wivrn), monado=repository(monado),
                  libsurvive=repository(survive), patches_sha256=patches,
                  note='Build source evidence; actual loaded library/plugin mappings and hashes are captured at runtime.')
    output.write_text('#pragma once\nstatic constexpr const char *lighthouse_build_provenance = R"WIVRN_PROVENANCE(' +
                      json.dumps(result, ensure_ascii=True) + ')WIVRN_PROVENANCE";\n')


if __name__ == '__main__':
    main()
