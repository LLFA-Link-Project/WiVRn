# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import gzip
import importlib.machinery
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
r = importlib.machinery.SourceFileLoader('survive_recorder', str(ROOT/'lh-recorder')).load_module()


def tracking_options(mode='new'):
    return {'process-weight-ang-vel':60,'kalman-minimize-state-space':0,
            'process-weight-acc-bias':1e-8,'process-weight-gyro-bias':1e-8,
            'kalman-zvu-stationary-ramp':0 if mode=='legacy' else .5,
            'kalman-zvu-stationary-gyro-scale':0 if mode=='legacy' else .02,
            'kalman-zvu-stationary-velocity-sigma':.1,'kalman-zvu-motion-guard-time':.5,
            'kalman-zvu-motion-all-devices':0,'kalman-align-observation-quaternion':0}


def fixture(path):
    manifest = dict(source='libsurvive', status='complete', configuration={'lh-max-extrapolation': 0},
                    loaded_survive_objects=[{'path': '/synthetic/libsurvive.so', 'sha256': '0'*64}], files_sha256={})
    r.dump(path/'manifest.json', manifest)
    r.dump(path/'build-provenance.json', {'libsurvive': {'revision': 'synthetic'}})
    with gzip.open(path/'clock_sync.jsonl.gz', 'wt') as f:
        for t in (1, 100):
            f.write(json.dumps(dict(monotonic_before_ns=t, monotonic_after_ns=t, realtime_ns=t+1000,
                                   offset_realtime_minus_monotonic_ns=1000, uncertainty_ns=0))+'\n')
    poses = []
    for stage in ('backend_received', 'history_input', 'app_output'):
        poses.append(dict(source='libsurvive', stream='libsurvive_pose', device_id='right-serial', serial='right-serial',
                          role='right_hand', capture_clock='CLOCK_MONOTONIC', capture_time_ns=50, stage=stage,
                          position_m=[0, 0, 0], orientation_xyzw=[0, 0, 0, 1], linear_velocity_m_s=[0, 0, 0],
                          angular_velocity_rad_s=[0, 0, 0], pose_is_valid=True, history_result=3, effective_prediction_ns=0))
    with gzip.open(path/'poses.jsonl.gz', 'wt') as f:
        for p in poses: f.write(json.dumps(p)+'\n')
    with gzip.open(path/'native.rec.gz', 'wt') as f:
        for tag in ('CONFIG', 'i', 'I', 'B', 'POSE', 'POSE_TIMING', 'POSE_STATE', 'POSE_BIAS'):
            payload = '{}' if tag == 'CONFIG' else ' '.join(['0']*r.NATIVE_NUMERIC_FIELDS[tag])
            f.write(f'0.01 WM5 {tag} {payload}\n')
    r.dump(path/'pose_status.json', dict(stopped=True, context_initialized=True, written=3, enqueued=3, attempted=3, dropped=0, error=''))
    return manifest, poses


class ValidationTests(unittest.TestCase):
    def test_native_requires_no_usbmon(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); fixture(path)
            result = r.validate(path, write=False)
            self.assertTrue(result['valid'], result)
            self.assertEqual(result['source'], 'libsurvive')
            self.assertIsNone(result['native_drop_count'])

    def test_missing_diagnostics_and_writer_loss_fail(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); fixture(path)
            with gzip.open(path/'native.rec.gz', 'wt') as f: f.write('0 WM1 POSE 0\n')
            status = r.read(path/'pose_status.json'); status['dropped'] = 1
            r.dump(path/'pose_status.json', status)
            result = r.validate(path, write=False)
            self.assertFalse(result['valid'])
            self.assertIn('missing_native_tag: POSE_STATE', result['issues'])
            self.assertIn('pose_records_dropped', result['issues'])

    def test_backend_mislabelling_and_zero_clamp_fail(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); _, poses = fixture(path)
            poses[-1]['effective_prediction_ns'] = 1
            poses[0]['stream'] = 'valve_driver_pose'
            with gzip.open(path/'poses.jsonl.gz', 'wt') as f:
                for p in poses: f.write(json.dumps(p)+'\n')
            result = r.validate(path, write=False)
            self.assertIn('wrong_pose_stream_or_clock', result['issues'])
            self.assertIn('nonzero_prediction_with_zero_clamp', result['issues'])

    def test_native_crc_failure(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); fixture(path)
            f = path/'native.rec.gz'; f.write_bytes(f.read_bytes()[:-5])
            result = r.validate(path, write=False)
            self.assertFalse(result['valid'])
            self.assertTrue(any(x.startswith('native_read_error') for x in result['issues']))

    def test_native_corruption_with_valid_crc(self):
        for record, reason in (('0 WM1 POSE 0\0 0\n', 'nul_byte'),
                               ('0 WM1 POSE 0\n', 'wrong_numeric_field_count'),
                               ('0 WM1 CONFIG {bad}\n', 'invalid_config_json')):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as d:
                path=Path(d); fixture(path)
                with gzip.open(path/'native.rec.gz', 'at') as f: f.write(record)
                result=r.validate(path,write=False)
                self.assertIn('native_malformed_records',result['issues'])
                self.assertEqual(result['native_record_integrity']['examples'][0]['reason'],reason)

    def test_native_archive_roundtrip_preserves_source_and_tags(self):
        with tempfile.TemporaryDirectory() as d:
            base=Path(d); path=base/'session'; path.mkdir()
            manifest,_=fixture(path); manifest['end_realtime_ns']=1100
            r.dump(path/'manifest.json',manifest)
            original=(path/'native.rec.gz').read_bytes()
            r.compress_sessions(path,base/'archives')
            r.decompress_sessions(base/'archives',base/'restored')
            restored=base/'restored'/'session'
            self.assertTrue(r.validate(restored,write=False)['valid'])
            self.assertEqual(r.native_tags(path),r.native_tags(restored))
            self.assertEqual((path/'native.rec.gz').read_bytes(),original)

    def test_marker_only_active_and_no_unsafe_public_export(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); manifest, _ = fixture(path)
            with self.assertRaises(ValueError): r.mark_scene(path, 'stationary')
            manifest['status'] = 'recording'; r.dump(path/'manifest.json', manifest)
            marker = r.mark_scene(path, 'slow-rotation-start')
            self.assertEqual(marker['capture_clock'], 'CLOCK_MONOTONIC')
            with self.assertRaises(ValueError): r.export_public(path, path/'public')
            with self.assertRaises(ValueError): r.align(path)


@unittest.skipUnless(os.environ.get('LH_SURVIVE_HARNESS'), 'requires built Monado adapter harness')
class AdapterTests(unittest.TestCase):
    def run_harness(self, path, clamp='0', bias=None, angular=None, constraints=None):
        env = dict(os.environ, WIVRN_TEST_SURVIVE_RECORDER=str(path), LH_MAX_EXTRAPOLATION_MS=clamp,
                   SURVIVE_CONFIGFILE=str(path/'config.json'))
        env.pop('WIVRN_SURVIVE_BIAS_COMPARISON', None)
        env.pop('WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON', None)
        env.pop('WIVRN_SURVIVE_TRACKER_CONSTRAINTS', None)
        if constraints: env['WIVRN_SURVIVE_TRACKER_CONSTRAINTS'] = constraints
        if bias: env['WIVRN_SURVIVE_BIAS_COMPARISON'] = bias
        if angular: env['WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON'] = angular
        return subprocess.run([os.environ['LH_SURVIVE_HARNESS'], str(1 if clamp == '-1' else float(clamp)/1000)],
                              env=env, capture_output=True, text=True, timeout=30)

    def test_real_adapter_stages_transforms_and_clamp(self):
        for clamp in ('0', '100', '-1'):
            with self.subTest(clamp=clamp), tempfile.TemporaryDirectory() as d:
                path = Path(d); result = self.run_harness(path, clamp)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                records = list(r.lines(path/'poses.jsonl'))
                self.assertEqual({p['stage'] for p in records}, {'backend_received','history_input','app_output','hand_output'})
                self.assertEqual({p['role'] for p in records}, {'left_hand','right_hand','tracker'})
                self.assertTrue(any(p['position_m'][0] == 0.123456789012 for p in records if p['stage'] == 'backend_received'))
                for p in records:
                    self.assertEqual(p['source'], 'libsurvive')
                    self.assertEqual(p['stream'], 'libsurvive_pose')
                    self.assertIsNone(p['sensor_tick']); self.assertIsNone(p['velocity_time_ns'])
                    if clamp == '0' and p['requested_time_ns']:
                        self.assertEqual(p['effective_prediction_ns'], 0)
                status = r.read(path/'pose_status.json')
                self.assertTrue(status['stopped']); self.assertFalse(status['error'])
                self.assertEqual(status['written'], len(records)); self.assertEqual(status['dropped'], 0)
                settings = r.read(path/'survive-settings.json')
                self.assertFalse(settings['estimator_settings_changed'])
                self.assertNotIn('--process-weight-gyro-bias', settings['arguments'])

    def test_config_names_follow_libsurvive_directory_not_cwd(self):
        for setting, xdg_enabled in ((None, True), (None, False), ('custom.json', True), ('./relative.json', True)):
            with self.subTest(setting=setting, xdg=xdg_enabled), tempfile.TemporaryDirectory() as d:
                path=Path(d); output=path/'recording';output.mkdir()
                env=dict(os.environ,WIVRN_TEST_SURVIVE_RECORDER=str(output),LH_MAX_EXTRAPOLATION_MS='0', HOME=str(path/'home'))
                env.pop('WIVRN_SURVIVE_BIAS_COMPARISON',None);env.pop('SURVIVE_CONFIGFILE',None)
                env.pop('XDG_CONFIG_HOME',None)
                if xdg_enabled:env['XDG_CONFIG_HOME']=str(path/'xdg')
                if setting is not None:env['SURVIVE_CONFIGFILE']=setting
                argument=setting or 'config.json'
                if argument.startswith('./'):expected=path/'relative.json'
                else:expected=(path/'xdg'/'libsurvive' if xdg_enabled else path/'home'/'.config'/'libsurvive')/argument
                expected.parent.mkdir(parents=True,exist_ok=True);expected.write_text('saved-calibration-fixture')
                result=subprocess.run([os.environ['LH_SURVIVE_HARNESS'],'0'],env=env,cwd=path,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                settings=r.read(output/'survive-settings.json');args=settings['arguments']
                self.assertEqual(settings['configfile'],str(expected))
                self.assertEqual(args[args.index('--configfile')+1],argument)
                self.assertEqual((output/'calibration.initial.json').read_text(),'saved-calibration-fixture')
                if not argument.startswith('./'):self.assertFalse((path/'config.json').exists())

    def test_ab_explicit_and_fresh_config(self):
        for bias in ('A', 'B'):
            with self.subTest(bias=bias), tempfile.TemporaryDirectory() as d:
                path = Path(d); result = self.run_harness(path, bias=bias)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                settings = r.read(path/'survive-settings.json'); args = settings['arguments']
                self.assertTrue(settings['estimator_settings_changed'])
                self.assertEqual(settings['configfile'], str(path/'calibration.json'))
                self.assertEqual(args[args.index('--kalman-minimize-state-space')+1], '0')
                self.assertEqual(args[args.index('--process-weight-gyro-bias')+1], '0' if bias == 'A' else '1e-8')

    def test_angular_trials_preserve_fixed_snapshot(self):
        for mode, weight in (('A', 60), ('B', .6)):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as d:
                path=Path(d); (path/'config.json').write_text('immutable baseline fixture')
                result=self.run_harness(path,angular=mode)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                settings=r.read(path/'survive-settings.json'); args=settings['arguments']
                self.assertEqual(settings['angular_noise_comparison'],mode)
                self.assertEqual(settings['configfile'],str(path/'config.json'))
                self.assertEqual((path/'calibration.initial.json').read_text(),'immutable baseline fixture')
                expected={'kalman-minimize-state-space':0,'process-weight-gyro-bias':1e-8,
                          'process-weight-acc-bias':1e-8,'process-weight-ang-vel':weight}
                self.assertEqual(settings['expected_trial_options'],expected)
                for key,value in expected.items(): self.assertEqual(float(args[args.index('--'+key)+1]),value)

    def test_trial_environment_ignored_without_recording(self):
        env=dict(os.environ,WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON='B',WIVRN_SURVIVE_TRACKER_CONSTRAINTS='legacy',LH_MAX_EXTRAPOLATION_MS='50')
        env.pop('WIVRN_TEST_SURVIVE_RECORDER',None)
        result=subprocess.run([os.environ['LH_SURVIVE_HARNESS'],'0.05'],env=env,capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_tracker_constraints_compare_only_two_weights(self):
        for mode in ('new', 'legacy'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as d:
                path=Path(d);(path/'config.json').write_text('fixed calibration')
                result=self.run_harness(path,constraints=mode)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                settings=r.read(path/'survive-settings.json');argv=settings['arguments']
                expected=tracking_options(mode)
                self.assertEqual(settings['tracker_constraints_comparison'],mode)
                self.assertEqual(settings['expected_trial_options'],expected)
                self.assertEqual((path/'calibration.initial.json').read_text(),'fixed calibration')
                for key,value in expected.items():
                    self.assertEqual(argv.count('--'+key),1)
                    self.assertEqual(float(argv[argv.index('--'+key)+1]),value)

    def test_queue_overflow_is_bounded_accounted_and_drained(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            result = subprocess.run([os.environ['LH_SURVIVE_HARNESS'], '--recorder-stress'],
                                    env=dict(os.environ, WIVRN_TEST_SURVIVE_RECORDER=d),
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            status = r.read(path/'pose_status.json')
            self.assertEqual(status['queue_capacity'], 8192)
            self.assertEqual(status['attempted'], 100000)
            self.assertGreater(status['dropped'], 0)
            self.assertEqual(status['attempted'], status['enqueued']+status['dropped'])
            self.assertEqual(status['enqueued'], status['written'])
            self.assertEqual(status['written'], sum(1 for _ in r.lines(path/'poses.jsonl')))
            self.assertTrue(status['stopped']); self.assertFalse(status['error'])

    def test_disk_failure_reported(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d); (path/'poses.jsonl.gz').symlink_to('/dev/full')
            result = self.run_harness(path)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            status = r.read(path/'pose_status.json')
            self.assertTrue(status['stopped']); self.assertTrue(status['error'])


@unittest.skipUnless(os.environ.get('LH_STARTUP_HARNESS'), 'requires built startup harness')
class NativeLifecycleTests(unittest.TestCase):
    def test_native_default_reads_existing_xdg_calibration(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d);calibration=path/'xdg'/'libsurvive'/'config.json'
            calibration.parent.mkdir(parents=True);calibration.write_text('"floor-offset":"0.375"\n')
            r.dump(path/'owner.json',{'use-survive':True,'lighthouse-recording':{'enabled':True,'output-directory':str(path/'sessions')}})
            env=dict(os.environ,XDG_CONFIG_HOME=str(path/'xdg'))
            env.pop('SURVIVE_CONFIGFILE',None);env.pop('WIVRN_SURVIVE_BIAS_COMPARISON',None)
            result=subprocess.run([os.environ['LH_STARTUP_HARNESS'],str(path/'owner.json'),'survive-empty'],env=env,cwd=path,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            session=next((path/'sessions').iterdir());settings=r.read(session/'survive-settings.json')
            self.assertEqual(settings['configfile'],str(calibration))
            self.assertEqual(settings['configfile_argument'],'config.json')
            self.assertEqual((session/'calibration.initial.json').read_text(),'"floor-offset":"0.375"\n')
            final=(session/'calibration.final.json').read_text()
            self.assertAlmostEqual(float(re.search(r'"floor-offset":"([^"]+)"',final).group(1)),.375)
            self.assertNotEqual(final,'"floor-offset":"0.375"\n')
            self.assertFalse((path/'config.json').exists())
            options=r.read(session/'native-options.json')
            for key,value in tracking_options().items(): self.assertEqual(float(options[key]['value']),value)

    def test_native_seal_preserves_and_detects_nul(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)
            r.dump(path/'config.json',{'use-survive':True,'lighthouse-recording':{'enabled':True,'output-directory':str(path/'sessions')}})
            env=dict(os.environ,SURVIVE_CONFIGFILE=str(path/'calibration.json'))
            result=subprocess.run([os.environ['LH_STARTUP_HARNESS'],str(path/'config.json'),'survive-corrupt'],env=env,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            session=next((path/'sessions').iterdir()); manifest=r.read(session/'manifest.json')
            self.assertEqual(manifest['native_recorder']['gzip_crc_and_read'],'passed')
            integrity=manifest['native_record_integrity']
            self.assertEqual(integrity['malformed_lines'],2,integrity)
            self.assertEqual([x['reason'] for x in integrity['examples']],['nul_byte','wrong_numeric_field_count'])
            self.assertEqual(r.native_integrity(session)['malformed_lines'],2)

    def test_real_native_trial_effective_values(self):
        for mode in ('A','B'):
            with tempfile.TemporaryDirectory() as d:
                path=Path(d)
                r.dump(path/'config.json',{'use-survive':True,'lh-max-extrapolation':0,'lighthouse-recording':{'enabled':True,'output-directory':str(path/'sessions')}})
                env=dict(os.environ,SURVIVE_CONFIGFILE=str(path/'calibration.json'),WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON=mode)
                env.pop('WIVRN_SURVIVE_BIAS_COMPARISON',None)
                result=subprocess.run([os.environ['LH_STARTUP_HARNESS'],str(path/'config.json'),'survive-empty'],env=env,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                session=next((path/'sessions').iterdir()); manifest=r.read(session/'manifest.json')
                expected=manifest['survive_settings']['expected_trial_options']
                self.assertEqual(manifest['trial_effective_options'],expected,manifest['errors'])
                self.assertFalse(any('Trial option mismatch' in e for e in manifest['errors']))

    def test_tracker_constraints_native_effective_values(self):
        for mode in ('new','legacy'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as d:
                path=Path(d)
                r.dump(path/'owner.json',{'use-survive':True,'lh-max-extrapolation':0,'lighthouse-recording':{'enabled':True,'output-directory':str(path/'sessions')}})
                env=dict(os.environ,SURVIVE_CONFIGFILE=str(path/'calibration.json'),WIVRN_SURVIVE_TRACKER_CONSTRAINTS=mode)
                result=subprocess.run([os.environ['LH_STARTUP_HARNESS'],str(path/'owner.json'),'survive-empty'],env=env,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                manifest=r.read(next((path/'sessions').iterdir())/'manifest.json')
                self.assertEqual(manifest['trial_effective_options'],tracking_options(mode),manifest['errors'])

    def test_tracker_constraints_reject_conflicting_conditions(self):
        cases=[({'WIVRN_SURVIVE_TRACKER_CONSTRAINTS':'wrong'},'must be new or legacy'),
               ({'WIVRN_SURVIVE_ANGULAR_NOISE_COMPARISON':'A'},'do not combine'),
               ({'SURVIVE_PROCESS-WEIGHT-ANG-VEL':'0.6'},'conflicting trial environment'),
               ({'SURVIVE_KALMAN-ZVU-MOTION-ALL-DEVICES':'1'},'conflicting trial environment'),
               ({'SURVIVE_KALMAN-ALIGN-OBSERVATION-QUATERNION':'1'},'conflicting trial environment')]
        for overrides,reason in cases:
            with self.subTest(overrides=overrides), tempfile.TemporaryDirectory() as d:
                path=Path(d)
                r.dump(path/'owner.json',{'use-survive':True,'lighthouse-recording':{'enabled':True,'output-directory':str(path/'sessions')}})
                env=dict(os.environ,SURVIVE_CONFIGFILE=str(path/'calibration.json'),WIVRN_SURVIVE_TRACKER_CONSTRAINTS='new');env.update(overrides)
                result=subprocess.run([os.environ['LH_STARTUP_HARNESS'],str(path/'owner.json'),'survive-reject'],env=env,capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                session=next((path/'sessions').iterdir())
                self.assertIn(reason,(session/'initialization-error.txt').read_text())
                self.assertEqual(r.read(session/'manifest.json')['status'],'abnormal')

    def test_real_native_no_usb_and_missing_data_not_success(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            r.dump(path/'config.json', {'use-survive': True, 'lighthouse-recording': {'enabled': True, 'output-directory': str(path/'sessions')}})
            env = dict(os.environ, SURVIVE_CONFIGFILE=str(path/'fresh-calibration.json'))
            env.pop('WIVRN_SURVIVE_BIAS_COMPARISON', None)
            result = subprocess.run([os.environ['LH_STARTUP_HARNESS'], str(path/'config.json'), 'survive-empty'],
                                    env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            session = next((path/'sessions').iterdir())
            manifest = r.read(session/'manifest.json')
            self.assertEqual(manifest['source'], 'libsurvive')
            self.assertIn('_libsurvive_', session.name)
            self.assertEqual(manifest['status'], 'abnormal')
            self.assertFalse((session/'usb.pcap.gz').exists())
            self.assertTrue(r.read(session/'pose_status.json')['stopped'])
            self.assertTrue(manifest['loaded_survive_objects'])
            for obj in manifest['loaded_survive_objects']:
                self.assertEqual(obj['sha256'], r.sha(Path(obj['path'])))
            self.assertTrue(r.native_tags(session).get('OPTION'))
            self.assertTrue(any('POSE_STATE' in x for x in manifest['errors']))
            self.assertFalse(r.validate(session, write=False)['valid'])


if __name__ == '__main__': unittest.main()
