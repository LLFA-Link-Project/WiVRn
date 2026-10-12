# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.machinery
import json
import os
from pathlib import Path
import struct
import gzip
import hashlib
import subprocess
import tempfile
import time
import io
import lzma
import tarfile
import shutil
import unittest
from unittest import mock

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
            self.assertNotIn('PRIVATE_SERIAL', gzip.open(dest/'poses.jsonl.gz', 'rt').read())
            self.assertFalse((dest/'usb.pcap.gz').exists())
            self.assertFalse(r.read(dest/'manifest.json')['usb_replay_available'])

    def test_recording_commands_removed(self):
        result = subprocess.run([str(ROOT/'lh-recorder'), '--help'], capture_output=True, text=True, check=True)
        self.assertIn('validate', result.stdout)
        self.assertNotIn('--survive-cli', result.stdout)
        for command in ('start', 'run', 'stop', 'status'):
            result = subprocess.run([str(ROOT/'lh-recorder'), command], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)



class CompressionTests(unittest.TestCase):
    def test_streaming_gzip_and_legacy_readers(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/'data.jsonl'
            p.write_text('{"test":1}\n')
            self.assertEqual(list(r.lines(p)), [{'test': 1}])
            with gzip.open(p.with_suffix('.jsonl.gz'), 'wt') as f:
                f.write('{"test":2}\n')
            self.assertEqual(list(r.lines(p)), [{'test': 2}])

    def test_batch_archive_roundtrip_and_standard_tools(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            source = base/'sessions'; source.mkdir()
            for name in ('one', 'two'):
                shutil.copytree(ROOT/'samples/synthetic-native-session', source/name)
            # Include a gzip session and a historical plain JSONL session.
            compressed_session = source/'two'
            manifest = r.read(compressed_session/'manifest.json')
            for stream in ('poses.jsonl', 'events.jsonl', 'clock_sync.jsonl', 'usb_devices.jsonl'):
                path = compressed_session/stream
                with path.open('rb') as input_file, gzip.open(str(path)+'.gz', 'wb') as output_file:
                    shutil.copyfileobj(input_file, output_file)
                path.unlink()
                del manifest['files_sha256'][stream]
                manifest['files_sha256'][stream+'.gz'] = r.sha(compressed_session/(stream+'.gz'))
            r.dump(compressed_session/'manifest.json', manifest)
            before = {str(p.relative_to(source)): r.sha(p) for p in source.rglob('*') if p.is_file()}
            def contents(root):
                result = {}
                for path in root.rglob('*'):
                    if not path.is_file() or path.name in ('manifest.json', 'validation.json'):
                        continue
                    name = str(path.relative_to(root))
                    if name.endswith('.gz'):
                        name = name[:-3]
                        with gzip.open(path, 'rb') as f:
                            result[name] = hashlib.sha256(f.read()).hexdigest()
                    else:
                        result[name] = r.sha(path)
                return result
            logical_before = contents(source)
            archives = r.compress_sessions(source, base/'archives')
            self.assertEqual(len(archives), 2)
            for path in archives:
                self.assertEqual(Path(path).read_bytes()[:6], b'\xfd7zXZ\x00')
                with tarfile.open(path, 'r:xz') as archive:
                    members = archive.getnames()
                    self.assertFalse(any(name.endswith('.gz') for name in members))
                    self.assertTrue(any(name.endswith('/usb.pcap') for name in members))
                    archived_manifest = json.load(archive.extractfile(Path(path).name[:-7]+'/manifest.json'))
                    self.assertIn('usb.pcap', archived_manifest['archive_recompression']['gzip_files'])
                    self.assertIn('usb.pcap', archived_manifest['files_sha256'])
                if shutil.which('xz') and shutil.which('tar'):
                    subprocess.run(['xz', '-t', path], check=True, timeout=15)
                    subprocess.run(['tar', '-tf', path], check=True, capture_output=True, timeout=15)
            restored = r.decompress_sessions(base/'archives', base/'restored')
            self.assertEqual(logical_before, contents(base/'restored'))
            self.assertEqual(before, {str(p.relative_to(source)): r.sha(p) for p in source.rglob('*') if p.is_file()})
            for path in restored:
                session = Path(path)
                self.assertEqual(r.read(session/'manifest.json')['archive_restore']['level'], 6)
                for name in ('poses.jsonl', 'events.jsonl', 'clock_sync.jsonl', 'usb_devices.jsonl', 'usb.pcap'):
                    self.assertFalse((session/name).exists())
                    self.assertTrue((session/(name+'.gz')).exists())
                    self.assertEqual((session/(name+'.gz')).read_bytes()[8], 0)  # gzip level 6 XFL
                self.assertTrue(r.validate(session, write=False)['valid'])
                if os.getenv('LH_SURVIVE_CLI'):
                    config = base/'replay-config'/'libsurvive'; config.mkdir(parents=True, exist_ok=True)
                    replay = subprocess.run([os.environ['LH_SURVIVE_CLI'], '--usbmon-playback', str(Path(path)/'usb.pcap.gz'), '--playback-factor', '0'],
                                            env=dict(os.environ, XDG_CONFIG_HOME=str(config.parent)), cwd=base, capture_output=True, text=True, timeout=10)
                    self.assertEqual(replay.returncode, 0, replay.stdout+replay.stderr)
                    self.assertIn('usbmon saw 120/', replay.stdout)
            with self.assertRaises(FileExistsError):
                r.compress_sessions(source, base/'archives')
            with self.assertRaises(FileExistsError):
                r.decompress_sessions(base/'archives', base/'restored')

    def test_active_and_changed_sessions_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d); source = base/'active'; source.mkdir()
            r.dump(source/'manifest.json', {'status': 'recording', 'end_realtime_ns': None})
            with self.assertRaises(ValueError):
                r.compress_sessions(source, base/'archives')
            self.assertFalse(list((base/'archives').glob('*.tar.xz')))
            r.dump(source/'manifest.json', {'status': 'complete', 'end_realtime_ns': 1})
            with self.assertRaises(ValueError):
                r.compress_sessions(source, source/'archives')
            self.assertFalse((source/'archives').exists())
            original_add = tarfile.TarFile.add
            def change_file(archive, *args, **kwargs):
                result = original_add(archive, *args, **kwargs)
                (source/'changed').write_text('modified during compression')
                return result
            with mock.patch.object(tarfile.TarFile, 'add', change_file):
                with self.assertRaises(ValueError):
                    r.compress_sessions(source, base/'archives')
            self.assertFalse(list((base/'archives').glob('*.tar.xz')))
            (source/'link').symlink_to('/tmp')
            with self.assertRaises(ValueError):
                r.compress_sessions(source, base/'archives')

    def test_unsafe_archives_and_corruption_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            for index, kind in enumerate(('traversal', 'link', 'duplicate', 'corrupt')):
                path = base/f'test{index}.tar.xz'
                with tarfile.open(path, 'w:xz') as archive:
                    record = tarfile.TarInfo(f'test{index}/manifest.json')
                    data = b'{"status":"complete"}'
                    record.size = len(data)
                    archive.addfile(record, io.BytesIO(data))
                    bad = tarfile.TarInfo(f'test{index}/../escaped')
                    if kind == 'link':
                        bad.name = f'test{index}/link'; bad.type = tarfile.SYMTYPE; bad.linkname = '/tmp'
                    if kind == 'duplicate':
                        bad.name = record.name
                    if kind != 'corrupt': archive.addfile(bad)
                if kind == 'corrupt':
                    contents = path.read_bytes(); path.write_bytes(contents[:-10])
                with self.assertRaises((ValueError, EOFError, tarfile.TarError, lzma.LZMAError)):
                    r.decompress_sessions(path, base/'restored')
                self.assertFalse((base/'restored'/f'test{index}').exists())
                self.assertFalse((base/'escaped').exists())

    def test_gzip_is_recompressed_with_xz_then_restored(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d); source = base/'session'; source.mkdir()
            data = b'{"value":"' + b'abcdefgh' * 150000 + b'"}\n'
            with gzip.open(source/'data.jsonl.gz', 'wb', compresslevel=1) as f:
                f.write(data)
            r.dump(source/'manifest.json', dict(status='complete', end_realtime_ns=1,
                   files_sha256={'data.jsonl.gz': r.sha(source/'data.jsonl.gz')}))
            archives = r.compress_sessions(source, base/'archives')
            self.assertLess(Path(archives[0]).stat().st_size, (source/'data.jsonl.gz').stat().st_size)
            with tarfile.open(archives[0], 'r:xz') as archive:
                self.assertEqual(archive.extractfile('session/data.jsonl').read(), data)
            restored = Path(r.decompress_sessions(Path(archives[0]), base/'restored')[0])
            with gzip.open(restored/'data.jsonl.gz', 'rb') as f:
                self.assertEqual(f.read(), data)
            manifest = r.read(restored/'manifest.json')
            self.assertEqual(manifest['files_sha256']['data.jsonl.gz'], r.sha(restored/'data.jsonl.gz'))
            self.assertLess((restored/'data.jsonl.gz').stat().st_size, (source/'data.jsonl.gz').stat().st_size)

    def test_legacy_archive_normalization_and_hash_failure(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            source = ROOT/'samples/synthetic-native-session'
            path = base/'legacy.tar.xz'
            with tarfile.open(path, 'w:xz') as archive:
                archive.add(source, arcname='legacy')
            restored = Path(r.decompress_sessions(path, base/'restored')[0])
            self.assertTrue(r.validate(restored, write=False)['valid'])
            self.assertTrue((restored/'poses.jsonl.gz').exists())
            self.assertEqual((restored/'usb.pcap.gz').read_bytes()[8], 0)
            bad = base/'bad'; shutil.copytree(source, bad)
            manifest = r.read(bad/'manifest.json')
            manifest['files_sha256']['usb.pcap.gz'] = '0' * 64
            r.dump(bad/'manifest.json', manifest)
            path = base/'bad.tar.xz'
            with tarfile.open(path, 'w:xz') as archive:
                archive.add(bad, arcname='bad')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                r.decompress_sessions(path, base/'restored')
            self.assertFalse((base/'restored'/'bad').exists())

    def test_cli_output_option_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            source = ROOT/'samples/synthetic-native-session'
            for command, path, output in [('compress', source, base/'archives'),
                                          ('decompress', base/'archives', base/'restored')]:
                subprocess.run([str(ROOT/'lh-recorder'), command, str(path), '--output', str(output)],
                               check=True, capture_output=True, timeout=30)
            restored = base/'restored'/source.name
            self.assertEqual(r.read(source/'manifest.json')['session_id'], r.read(restored/'manifest.json')['session_id'])
            self.assertTrue(r.validate(restored, write=False)['valid'])


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
            self.assertFalse((p/'poses.jsonl').exists())
            self.assertTrue((p/'poses.jsonl.gz').read_bytes().startswith(b'\x1f\x8b'))
            self.assertLess((p/'poses.jsonl.gz').stat().st_size, len(gzip.decompress((p/'poses.jsonl.gz').read_bytes())) // 2)
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
            (p/'poses.jsonl.gz').mkdir()
            self.invoke(p, 30, True)
            self.assertTrue(r.read(p/'pose_status.json')['error'])

@unittest.skipUnless(os.getenv('LH_STARTUP_HARNESS'), 'Set LH_STARTUP_HARNESS to compiled startup test')
class StartupTests(unittest.TestCase):
    def test_permission_failure_keeps_owner_alive(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            conf = base/'config.json'
            conf.write_text(json.dumps({'lighthouse-recording': {'enabled': True, 'output-directory': str(base/'logs')}}))
            result = subprocess.run([os.environ['LH_STARTUP_HARNESS'], str(conf), 'deny'],
                                    env=dict(os.environ, PATH='', LH_RECORDER_ENABLE='1'),
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('ready', result.stdout)
            session = next((base/'logs').iterdir())
            self.assertEqual(r.read(session/'manifest.json')['status'], 'abnormal')
            self.assertTrue(any('permission denied' in e for e in r.read(session/'manifest.json')['errors']))
            records = list(r.lines(session/'poses.jsonl'))
            status = r.read(session/'pose_status.json')
            announcements = sum(event.get('event') == 'device_announced' for event in r.lines(session/'events.jsonl'))
            self.assertEqual(status['attempted'], 121)
            self.assertEqual(status['attempted'], len(records)+status['dropped']+announcements)
            self.assertGreater(records[-1]['capture_time_ns']-records[0]['capture_time_ns'], 2_000_000_000)
            self.assertTrue(r.read(session/'pose_status.json')['stopped'])
            self.assertEqual(list(r.pcap_records(session/'usb.pcap.gz')), [])

    def test_abnormal_owner_exit_closes_both_streams(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            conf = base/'config.json'
            conf.write_text(json.dumps({'lighthouse-recording': {'enabled': True, 'output-directory': str(base/'logs')}}))
            result = subprocess.run([os.environ['LH_STARTUP_HARNESS'], str(conf), 'abnormal'], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0)
            session = next((base/'logs').iterdir())
            manifest = r.read(session/'manifest.json')
            self.assertEqual(manifest['owner_exit_status'], 1)
            self.assertEqual(manifest['status'], 'abnormal')
            self.assertTrue(r.read(session/'pose_status.json')['stopped'])
            self.assertGreater(len(list(r.pcap_records(session/'usb.pcap.gz'))), 30)
            self.assertFalse(r.read(session/'validation.json')['valid'])
            self.assertEqual({name:r.sha(session/name) for name in manifest['files_sha256']}, manifest['files_sha256'])

    def test_snapshot_restart_and_owner_finalization(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            conf = base/'config.json'
            old, new = base/'old', base/'new'
            def configure(enabled, output):
                conf.write_text(json.dumps({'lighthouse-recording': {'enabled': enabled, 'output-directory': str(output)}}))
            def launch():
                return subprocess.Popen([os.environ['LH_STARTUP_HARNESS'], str(conf)],
                                        env=dict(os.environ, PATH='', LH_RECORDER_ENABLE='1', LH_RECORDER_OUTPUT=str(base/'env-must-not-trigger')),
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            configure(False, old)
            off = launch(); stdout, stderr = off.communicate(timeout=10)
            self.assertEqual(off.returncode, 0, stderr)
            self.assertFalse(old.exists())
            self.assertFalse((base/'env-must-not-trigger').exists())
            configure(True, old)
            on = launch()
            deadline = time.monotonic()+10
            while time.monotonic() < deadline and not list(old.glob('*/pose_status.json')):
                time.sleep(0.02)
            self.assertTrue(list(old.glob('*/pose_status.json')))
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
            for name in ('poses', 'events', 'clock_sync', 'usb_devices'):
                self.assertTrue((session/(name+'.jsonl.gz')).is_file())
                self.assertFalse((session/(name+'.jsonl')).exists())
                with gzip.open(session/(name+'.jsonl.gz'), 'rb') as f:
                    self.assertTrue(f.read())
            r.align(session)
            self.assertEqual(len(list(r.lines(session/'usb_timestamps.jsonl.gz'))), r.read(session/'manifest.json')['usb_packet_count'])
            records = list(r.lines(session/'poses.jsonl'))
            status = r.read(session/'pose_status.json')
            announcements = sum(event.get('event') == 'device_announced' for event in r.lines(session/'events.jsonl'))
            self.assertEqual(status['attempted'], 121)
            self.assertEqual(status['attempted'], len(records)+status['dropped']+announcements)
            self.assertGreater(records[-1]['capture_time_ns']-records[0]['capture_time_ns'], 2_000_000_000)
            self.assertGreater(len(list(r.pcap_records(session/'usb.pcap.gz'))), 30)
            self.assertTrue(r.validate(session)['valid'])
            self.assertEqual(r.read(session/'manifest.json')['owner_exit_status'], 0)
            checksums = r.read(session/'manifest.json')['files_sha256']
            time.sleep(0.1)
            self.assertEqual({name:r.sha(session/name) for name in checksums}, checksums)
            if os.getenv('LH_SURVIVE_CLI'):
                config = base/'replay-config'/'libsurvive'
                config.mkdir(parents=True)
                for _ in range(2):
                    replay = subprocess.run([os.environ['LH_SURVIVE_CLI'], '--usbmon-playback', str(session/'usb.pcap.gz'), '--playback-factor', '0'],
                                            env=dict(os.environ, XDG_CONFIG_HOME=str(config.parent)),
                                            cwd=base, capture_output=True, text=True, timeout=10)
                    self.assertEqual(replay.returncode, 0, replay.stdout+replay.stderr)
                    self.assertIn(f"usbmon saw {r.read(session/'manifest.json')['usb_packet_count']}/", replay.stdout)
                    self.assertIn('Loaded drivers: USBMon_Playback', replay.stdout)
                    self.assertTrue(r.validate(session)['valid'])
            restarted = launch(); restarted.communicate(timeout=10)
            self.assertFalse(new.exists())
            configure(True, '~/LH-Dump')
            old_home = os.environ.get('HOME')
            os.environ['HOME'] = str(base)
            try:
                again = launch()
            finally:
                if old_home is None:
                    os.environ.pop('HOME', None)
                else:
                    os.environ['HOME'] = old_home
            new = base/'LH-Dump'; again.communicate(timeout=10)
            self.assertEqual(again.returncode, 0)
            self.assertTrue(list(new.glob('*/poses.jsonl.gz')))
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
