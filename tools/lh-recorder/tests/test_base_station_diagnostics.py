# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('base_station_diagnostics', Path(__file__).parents[1]/'base_station_diagnostics.py')
d = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(d)


def sample(serial, t, pose, cls=3, **extra):
    return dict(device_id=serial, device_class=cls, pose_time_estimate_ns=t, device_is_connected=True,
                pose_is_valid=True, tracking_result=200,
                composed_pose=dict(position_m=pose[0], orientation_xyzw=pose[1]), **extra)


class DiagnosticsTests(unittest.TestCase):
    def test_common_transform_preserves_full_relative_pose(self):
        b = ((1, 2, 3), (math.sqrt(.5), 0, 0, math.sqrt(.5)))
        p = ((4, 0, 2), (0, math.sqrt(.5), 0, math.sqrt(.5)))
        change = ((-3, 4, 1), (0, 0, math.sqrt(.5), math.sqrt(.5)))
        rows = [sample('base', 0, b, 4), sample('tracker', 1, p), sample('base2', 2, p, 4),
                sample('base', 10, d.compose(change, b), 4),
                sample('tracker', 11, d.compose(change, p)), sample('base2', 12, d.compose(change, p), 4)]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'poses.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            trace = io.StringIO()
            report = d.analyze(path, 'base', 1, trace)
            pairs = [json.loads(line) for line in trace.getvalue().splitlines()]
            self.assertEqual(len(pairs), 4)
            self.assertEqual(pairs[0]['reference_pose_time_estimate_ns'], 0)
            self.assertEqual(pairs[2]['reference_pose_time_estimate_ns'], 10)
        for serial in ('tracker', 'base2'):
            s = report['devices'][serial]
            self.assertEqual(s['paired'], 2)
            self.assertGreater(s['max_absolute_displacement_m'], 1)
            self.assertGreater(s['max_absolute_rotation_deg'], 80)
            self.assertLess(s['max_relative_displacement_m'], 1e-12)
            self.assertLess(s['max_relative_rotation_deg'], 1e-5)

    def test_stale_invalid_future_and_missing_composition_are_counted(self):
        p = ((0, 0, 0), (0, 0, 0, 1))
        rows = [sample('tracker', 0, p), sample('base', 10, p, 4),
                sample('tracker', 20, p), sample('tracker', 2_000_000, p),
                sample('base', 3_000_000, p, 4), sample('tracker', 3_000_001, p),
                sample('tracker', 3_000_002, p)]
        rows[4]['pose_is_valid'] = False
        rows[6].pop('composed_pose')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'poses.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            s = d.analyze(path, 'base', 1)['devices']['tracker']
            for key in ('paired', 'no_reference', 'stale_reference', 'invalid_reference', 'invalid'):
                self.assertEqual(s[key], 1, key)
            with self.assertRaises(ValueError):
                d.analyze(path, 'absent', 1)
            with self.assertRaises(ValueError):
                d.analyze(path, 'tracker', 1)

    def test_tracker_only_jump_is_not_canceled(self):
        p = ((0, 0, 0), (0, 0, 0, 1))
        rows = [sample('base', 0, p, 4), sample('tracker', 1, p),
                sample('tracker', 2, ((2, 0, 0), p[1]))]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path/'poses.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            s = d.analyze(path, 'base', 1)['devices']['tracker']
        self.assertEqual(s['max_relative_step_m'], 2)


@unittest.skipUnless(os.getenv('LH_REFERENCE_HARNESS'), 'Set LH_REFERENCE_HARNESS to Monado tests_steamvr_lh_references')
class HostTests(unittest.TestCase):
    def test_callbacks_and_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            subprocess.run([os.environ['LH_REFERENCE_HARNESS'], 'on'], check=True, timeout=15,
                           env=dict(os.environ, HOME='/nonexistent-lh-test', LH_RECORDER_ENABLE='1', LH_RECORDER_OUTPUT=tmp),
                           capture_output=True)
            rows = list(d.records(path))
            self.assertGreater(len(rows), 0)
            for row in rows:
                self.assertEqual(row['device_class'], 4)
                self.assertEqual(row['universe_id'], '1234567890123456789')
                self.assertEqual(row['composed_coordinate_frame'], 'lighthouse_chaperone_before_tracking_origin')
                self.assertEqual(row['position_m'], [2, 0, 0])
                for actual, expected in zip(row['composed_pose']['position_m'], [10, 21, 28]):
                    self.assertAlmostEqual(actual, expected, places=5)
                for actual in row['composed_pose']['orientation_xyzw']:
                    self.assertAlmostEqual(actual, .5, places=6)
