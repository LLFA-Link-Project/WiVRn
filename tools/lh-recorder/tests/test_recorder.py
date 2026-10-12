# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.machinery
import json
import os
from pathlib import Path
import struct
import gzip
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
r = importlib.machinery.SourceFileLoader('recorder', str(ROOT/'lh-recorder')).load_module()


def synthetic_pcap(path, realtime_ns, count=3, truncated=False):
    with gzip.open(path, 'wb') as f:
        f.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 220))
        for i in range(count):
            t = realtime_ns+i*1000
            # Explicitly synthetic interrupt completion: unrecognized report 0xff.
            payload = b'\xff\x00\x00\x00'
            header = struct.pack('<QBBBBHBBqiiII8siiII', i+1, ord('C'), 1, 0x81, 7, 1,
                                 ord('-'), 0, t//10**9, t//1000%10**6, 0, len(payload),
                                 len(payload), b'\x00'*8, 1, 0, 0, 0)
            packet = header+payload
            f.write(struct.pack('<IIII', t//10**9, t//1000%10**6, len(packet), len(packet)+(100 if truncated else 0)))
            f.write(packet)
    path.with_name(path.name+'.usbdevs').write_text('10462 8449 1 7 WM\n')


def sample(mono, real, width=100):
    return dict(monotonic_before_ns=mono-width, monotonic_after_ns=mono+width,
                realtime_ns=real, offset_realtime_minus_monotonic_ns=real-mono,
                uncertainty_ns=width)


class ClockTests(unittest.TestCase):
    def test_constant_offset(self):
        c = r.ClockMap([sample(10**9, 10**18), sample(2*10**9, 10**18+10**9)])
        p = c.convert(10**18+500_000_000)
        self.assertEqual(p['monotonic_estimate_ns'], 1_500_000_000)
        self.assertEqual(p['estimated_error_ns'], 1100)

    def test_jump_refuses_mapping(self):
        c = r.ClockMap([sample(10**9, 10**18), sample(2*10**9, 10**18+2*10**9)])
        self.assertEqual(len(c.jumps), 1)
        self.assertIsNone(c.convert(10**18+500_000_000)['monotonic_estimate_ns'])

    def test_backward_jump_ambiguous(self):
        c = r.ClockMap([sample(10**9, 10**18), sample(2*10**9, 10**18+10**9),
                        sample(3*10**9, 10**18+500_000_000), sample(4*10**9, 10**18+1_500_000_000)])
        self.assertIsNone(c.convert(10**18+750_000_000)['monotonic_estimate_ns'])

    def test_no_extrapolation(self):
        c = r.ClockMap([sample(1, 101), sample(11, 111)])
        self.assertIsNone(c.convert(112)['monotonic_estimate_ns'])

    def test_measured_brackets(self):
        s = r.clock_sample('test')
        self.assertLessEqual(s['monotonic_before_ns'], s['monotonic_after_ns'])
        self.assertLessEqual(s['raw_monotonic_before_ns'], s['raw_monotonic_after_ns'])
        self.assertEqual(s['uncertainty_ns'], (s['monotonic_after_ns']-s['monotonic_before_ns']+1)//2)


class FileTests(unittest.TestCase):
    def test_usb_roundtrip_and_truncation(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'usb.pcap.gz'
            synthetic_pcap(p, time.time_ns(), truncated=True)
            a, b = list(r.pcap_records(p)), list(r.pcap_records(p))
            self.assertEqual(a, b)
            self.assertEqual(len(a), 3)
            self.assertEqual(a[0]['direction'], 'in')
            self.assertTrue(a[0]['pcap_truncated'])
            self.assertEqual(a[0]['payload_captured_bytes'], 4)

    def test_malformed_pcap_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'usb.pcap.gz'
            with gzip.open(p, 'wb') as f:
                f.write(b'invalid')
            with self.assertRaises(ValueError):
                list(r.pcap_records(p))

    def test_public_export_has_no_raw_usb_or_serial(self):
        with tempfile.TemporaryDirectory() as d:
            source, dest = Path(d)/'in', Path(d)/'public'
            source.mkdir()
            for name in ('poses.jsonl', 'events.jsonl'):
                (source/name).write_text('{"device_id":"PRIVATE_SERIAL"}\n')
            (source/'clock_sync.jsonl').write_text('{}\n')
            r.export_public(source, dest)
            self.assertNotIn('PRIVATE_SERIAL', (dest/'poses.jsonl').read_text())
            self.assertFalse((dest/'usb.pcap.gz').exists())
            self.assertFalse(r.read(dest/'manifest.json')['usb_replay_available'])

    def test_capture_command_passive(self):
        cmd = r.usb_command('/bin/survive-cli', Path('/tmp/session'))
        self.assertIn('--usbmon-only-record', cmd)
        self.assertIn('--usbmon', cmd)
        self.assertNotIn('--htcvive', cmd)

    def test_missing_binary_finalizes_abnormally(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'session'
            result = subprocess.run([str(ROOT/'lh-recorder'), 'run', '--output', str(p),
                                     '--survive-cli', '/not/a/survive-cli'], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(r.read(p/'manifest.json')['status'], 'abnormal')
            self.assertFalse((p/'active').exists())
            self.assertFalse(r.read(p/'validation.json')['valid'])


@unittest.skipUnless(os.getenv('LH_POSE_HARNESS'), 'Set LH_POSE_HARNESS to compiled pose_harness')
class PoseTests(unittest.TestCase):
    def invoke(self, path, count, enabled):
        env = dict(os.environ, LH_RECORDER_ENABLE='1' if enabled else '0', LH_RECORDER_OUTPUT=str(path))
        return subprocess.run([env['LH_POSE_HARNESS'], str(count)], env=env, capture_output=True, text=True, check=True, timeout=30)

    def test_disabled_no_files(self):
        with tempfile.TemporaryDirectory() as d:
            self.invoke(Path(d), 30, False)
            self.assertEqual(list(Path(d).iterdir()), [])

    def test_enabled_raw_values_states_and_flush(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            self.invoke(p, 30, True)
            records = list(r.lines(p/'poses.jsonl'))
            self.assertEqual(len(records), 30)
            self.assertEqual({x['device_id'] for x in records}, {'tracker-A', 'controller-B'})
            self.assertEqual(records[0]['position_m'][0], 1.25)
            self.assertEqual(records[0]['pose_time_offset_s'], -0.004)
            self.assertIsNone(records[25]['position_m'][1])
            self.assertIsNone(records[25]['angular_velocity_rad_s'][0])
            events = list(r.lines(p/'events.jsonl'))
            self.assertTrue(any(x.get('device_is_connected') is False for x in events))
            self.assertTrue(any(x.get('pose_is_valid') is False for x in events))
            self.assertTrue(r.read(p/'pose_status.json')['stopped'])

    def test_json_ignores_process_number_locale(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            subprocess.run([os.environ['LH_POSE_HARNESS'], '30'],
                           env=dict(os.environ, LH_RECORDER_ENABLE='1', LH_RECORDER_OUTPUT=str(path), LH_TEST_COMMA_LOCALE='1'),
                           check=True, capture_output=True, timeout=10)
            records = list(r.lines(path/'poses.jsonl'))
            self.assertEqual(records[0]['position_m'][0], 1.25)
            self.assertTrue(r.read(path/'pose_status.json')['stopped'])

    def test_overflow_is_bounded_and_reported(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            perf = json.loads(self.invoke(p, 100000, True).stdout)
            status = r.read(p/'pose_status.json')
            self.assertGreater(status['dropped'], 0)
            self.assertEqual(status['queue_capacity'], 4096)
            self.assertEqual(status['attempted'], status['written']+status['dropped']+2)
            self.assertLess(perf['callback_total_ns']/perf['samples'], 100000)
            self.assertLess(perf['peak_rss_kib'], 64000)

    def test_io_failure_does_not_crash_driver(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p/'poses.jsonl').mkdir()
            self.invoke(p, 30, True)
            self.assertTrue(r.read(p/'pose_status.json')['error'])

@unittest.skipUnless(os.getenv('LH_STARTUP_HARNESS'), 'Set LH_STARTUP_HARNESS to compiled startup test')
class StartupTests(unittest.TestCase):
    def test_permission_failure_keeps_owner_alive(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            conf = base/'config.json'
            conf.write_text(json.dumps({'lighthouse-recording': {'enabled': True, 'output-directory': str(base/'logs')}}))
            result = subprocess.run([os.environ['LH_STARTUP_HARNESS'], str(conf)],
                                    env=dict(os.environ, LH_RECORDER_SURVIVE_CLI=str(ROOT/'tests/fake_usb.py'), LH_TEST_DENY_CAPTURE='1'),
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('ready', result.stdout)
            session = next((base/'logs').iterdir())
            self.assertEqual(r.read(session/'manifest.json')['status'], 'abnormal')
            self.assertIn('permission denied', (session/'usb_capture.log').read_text())
            self.assertFalse((session/'poses.jsonl').exists())

    def test_snapshot_restart_and_owner_finalization(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            conf = base/'config.json'
            old, new = base/'old', base/'new'
            def configure(enabled, output):
                conf.write_text(json.dumps({'lighthouse-recording': {'enabled': enabled, 'output-directory': str(output)}}))
            def launch():
                return subprocess.Popen([os.environ['LH_STARTUP_HARNESS'], str(conf)],
                                        env=dict(os.environ, LH_RECORDER_SURVIVE_CLI=str(ROOT/'tests/fake_usb.py')),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            configure(False, old)
            off = launch(); stdout, stderr = off.communicate(timeout=10)
            self.assertEqual(off.returncode, 0, stderr)
            self.assertFalse(old.exists())
            configure(True, old)
            on = launch()
            deadline = time.monotonic()+10
            while time.monotonic() < deadline and not list(old.glob('*/ready.json')):
                time.sleep(0.02)
            self.assertTrue(list(old.glob('*/ready.json')))
            configure(False, new)  # Current immutable snapshot must keep both capture streams alive.
            stdout, stderr = on.communicate(timeout=10)
            self.assertEqual(on.returncode, 0, stderr)
            self.assertIn('enabled', stdout)
            self.assertIn(str(old), stdout)
            self.assertFalse(new.exists())
            session = next(old.iterdir())
            deadline = time.monotonic()+10
            while time.monotonic() < deadline and r.read(session/'manifest.json', {}).get('status') == 'recording':
                time.sleep(0.05)
            self.assertEqual(r.read(session/'manifest.json')['status'], 'complete', (session/'manifest.json').read_text())
            self.assertEqual(len(list(r.lines(session/'poses.jsonl'))), 120)
            self.assertGreater(len(list(r.pcap_records(session/'usb.pcap.gz'))), 30)
            self.assertTrue(r.validate(session)['valid'])
            restarted = launch(); restarted.communicate(timeout=10)
            self.assertFalse(new.exists())
            configure(True, new)
            again = launch(); again.communicate(timeout=10)
            self.assertEqual(again.returncode, 0)
            self.assertTrue(list(new.glob('*/poses.jsonl')))
            self.assertEqual(len(list(old.iterdir())), 1)


@unittest.skipUnless(os.getenv('LH_SURVIVE_CLI'), 'Set LH_SURVIVE_CLI to patched survive-cli')
class ReplayTests(unittest.TestCase):
    def test_libsurvive_reads_original_twice(self):
        with tempfile.TemporaryDirectory() as d:
            session = Path(d)
            p = session/'usb.pcap.gz'
            synthetic_pcap(p, time.time_ns())
            checksum = r.sha(p)
            config = session/'private-config'/'libsurvive'
            config.mkdir(parents=True)
            for repeat in range(2):
                result = subprocess.run([os.environ['LH_SURVIVE_CLI'], '--usbmon-playback', str(p), '--playback-factor', '0'],
                                        env=dict(os.environ, XDG_CONFIG_HOME=str(config.parent)),
                                        cwd=session, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn('usbmon saw 3/', result.stdout)
                self.assertIn('Loaded drivers: USBMon_Playback', result.stdout)
                self.assertNotIn('Loaded drivers: HTCVive', result.stdout)
                self.assertEqual(r.sha(p), checksum)


class DiskClockTests(unittest.TestCase):
    def test_disk_and_memory_mapping_match(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)/'clock_sync.jsonl'
            samples = [sample(10**9, 10**18), sample(2*10**9, 10**18+10**9),
                       sample(3*10**9, 10**18+500_000_000), sample(4*10**9, 10**18+1_500_000_000)]
            path.write_text(''.join(json.dumps(s)+'\n' for s in samples))
            disk, memory = r.ClockMap.from_file(path), r.ClockMap(samples)
            for t in [10**18-1, 10**18+10, 10**18+750_000_000, 10**18+1_400_000_000, 10**18+1_600_000_000]:
                self.assertEqual(disk.convert(t), memory.convert(t))
            self.assertEqual(disk.report()['sample_count'], 4)
            disk.close()


if __name__ == '__main__':
    unittest.main()
