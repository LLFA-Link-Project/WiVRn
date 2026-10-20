#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare fixed devices against an explicit Lighthouse reference; no live correction."""
import argparse
from bisect import bisect_right
import gzip
import json
import math
from pathlib import Path


def records(session):
    path = session / 'poses.jsonl.gz'
    if path.exists():
        stream = gzip.open(path, 'rt', encoding='utf-8')
    else:
        stream = (session / 'poses.jsonl').open(encoding='utf-8')
    with stream:
        for line in stream:
            yield json.loads(line)


def multiply(a, b):
    x, y, z, w = a
    u, v, s, t = b
    return (w*u+x*t+y*s-z*v, w*v-x*s+y*t+z*u,
            w*s+x*v-y*u+z*t, w*t-x*u-y*v-z*s)


def rotate(q, v):
    return multiply(multiply(q, (*v, 0)), (-q[0], -q[1], -q[2], q[3]))[:3]


def compose(a, b):
    p, q = a
    r, s = b
    rotated = rotate(q, r)
    return tuple(x+y for x, y in zip(p, rotated)), multiply(q, s)


def inverse(a):
    p, q = a
    q = (-q[0], -q[1], -q[2], q[3])
    return rotate(q, tuple(-x for x in p)), q


def pose(record):
    if not record.get('device_is_connected') or not record.get('pose_is_valid'):
        return None
    # Running_OK only: rotation-only/out-of-range must not count as measured 6DoF.
    if record.get('tracking_result') != 200:
        return None
    raw = record.get('composed_pose', {})
    p, q = raw.get('position_m', []), raw.get('orientation_xyzw', [])
    if len(p) != 3 or len(q) != 4:
        return None
    if not all(isinstance(x, (float, int)) and math.isfinite(x) for x in (*p, *q)):
        return None
    norm = math.sqrt(sum(x*x for x in q))
    if not 0.99 <= norm <= 1.01:
        return None
    return tuple(p), tuple(x/norm for x in q)


def distance(a, b):
    p = math.dist(a[0], b[0])
    dot = min(1.0, abs(sum(x*y for x, y in zip(a[1], b[1]))))
    return p, math.degrees(2*math.acos(dot))


def timestamp(record):
    t = record.get('pose_time_estimate_ns')
    return t if isinstance(t, int) and t >= 0 else None


def analyze(session, reference, max_age_ms, samples_output=None):
    if not math.isfinite(max_age_ms) or max_age_ms < 0:
        raise ValueError('max-age-ms must be finite and nonnegative')
    # Retain only low-rate reference records; stream the high-rate tracker data.
    refs = [r for r in records(session) if r.get('device_id') == reference and timestamp(r) is not None]
    if not refs:
        raise ValueError('reference serial has no timestamped samples')
    if any(r.get('device_class') != 4 for r in refs):
        raise ValueError('reference must be a TrackingReference (device_class 4)')
    refs.sort(key=lambda r: (timestamp(r), r.get('sequence', 0)))
    times = [timestamp(r) for r in refs]
    states, baselines, previous = {}, {}, {}
    for r in records(session):
        serial = r.get('device_id')
        if serial == reference:
            continue
        state = states.setdefault(serial, dict(samples=0, paired=0, invalid=0, no_reference=0,
                                               stale_reference=0, invalid_reference=0, out_of_order=0,
                                               max_reference_age_ms=0.0,
                                               max_absolute_displacement_m=0.0, max_absolute_rotation_deg=0.0,
                                               max_relative_displacement_m=0.0, max_relative_rotation_deg=0.0,
                                               max_relative_step_m=0.0, max_relative_step_deg=0.0))
        state['samples'] += 1
        t, p = timestamp(r), pose(r)
        if t is None or p is None:
            state['invalid'] += 1
            previous.pop(serial, None)
            continue
        if serial in previous and t <= previous[serial][0]:
            state['out_of_order'] += 1
            continue
        i = bisect_right(times, t) - 1
        reason = None
        if i < 0:
            reason = 'no_reference'
        elif (t-times[i])/1e6 > max_age_ms:
            reason = 'stale_reference'
        elif pose(refs[i]) is None:
            reason = 'invalid_reference'
        if reason:
            state[reason] += 1
            previous.pop(serial, None)
            continue
        relative = compose(inverse(pose(refs[i])), p)
        if samples_output is not None:
            samples_output.write(json.dumps(dict(
                device_id=serial, reference=reference, pose_time_estimate_ns=t,
                reference_pose_time_estimate_ns=times[i], capture_time_ns=r.get('capture_time_ns'),
                reference_capture_time_ns=refs[i].get('capture_time_ns'),
                reference_age_ms=(t-times[i])/1e6, universe_id=r.get('universe_id'),
                reference_universe_id=refs[i].get('universe_id'),
                absolute_pose=r['composed_pose'], reference_pose=refs[i]['composed_pose'],
                relative_pose=dict(position_m=relative[0], orientation_xyzw=relative[1])),
                allow_nan=False)+'\n')
        state['paired'] += 1
        state['max_reference_age_ms'] = max(state['max_reference_age_ms'], (t-times[i])/1e6)
        initial = baselines.setdefault(serial, (p, relative))
        for key, a, b in [('absolute', initial[0], p), ('relative', initial[1], relative)]:
            position, angle = distance(a, b)
            state[f'max_{key}_displacement_m'] = max(state[f'max_{key}_displacement_m'], position)
            state[f'max_{key}_rotation_deg'] = max(state[f'max_{key}_rotation_deg'], angle)
        if serial in previous:
            position, angle = distance(previous[serial][1], relative)
            state['max_relative_step_m'] = max(state['max_relative_step_m'], position)
            state['max_relative_step_deg'] = max(state['max_relative_step_deg'], angle)
        previous[serial] = t, relative
    return dict(schema_version=1, reference=reference, reference_samples=len(refs),
                valid_reference_samples=sum(pose(r) is not None for r in refs), max_age_ms=max_age_ms,
                pairing='latest past pose_time_estimate_ns; no interpolation or prediction',
                interpretation='Stationary devices only. This report does not establish a common coordinate generation.',
                devices=states)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--reference', required=True, help='explicit base station serial')
    parser.add_argument('--max-age-ms', required=True, type=float, help='maximum held reference age')
    parser.add_argument('--samples-output', type=Path, help='write paired absolute and relative poses as JSONL')
    args = parser.parse_args()
    try:
        if args.samples_output is None:
            report = analyze(args.session, args.reference, args.max_age_ms)
        else:
            # Exclusive create prevents overwriting a session's original streams.
            with args.samples_output.open('x', encoding='utf-8') as output:
                report = analyze(args.session, args.reference, args.max_age_ms, output)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(1, f'{exc}\n')
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
