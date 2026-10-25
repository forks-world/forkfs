"""Fail-closed checks for the signed host and FSKit extension bundle."""
import plistlib
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'fskit'))
import mount_probe


HOST_ID = 'world.forks.cow.revision'
EXT_ID = 'world.forks.cow.revision.extension'
HOST_ENTITLEMENTS = {
    'com.apple.developer.fskit.mount': True,
    'com.apple.security.app-sandbox': True,
    'com.apple.security.network.client': True,
}
EXT_ENTITLEMENTS = {
    'com.apple.developer.fskit.fsmodule': True,
    'com.apple.security.app-sandbox': True,
    'com.apple.security.network.client': True,
}


class SignedBundles:
    def __init__(self, *, host_entitlements=None, extension_entitlements=None,
                 host_payload=None, extension_payload=None, host_team='TEAM-A', extension_team='TEAM-A',
                 host_verify=0, extension_verify=0, host_metadata=0, extension_metadata=0,
                 host_entitlement_status=0, extension_entitlement_status=0, timeout_program=None):
        self.host_entitlements = HOST_ENTITLEMENTS if host_entitlements is None else host_entitlements
        self.extension_entitlements = EXT_ENTITLEMENTS if extension_entitlements is None else extension_entitlements
        self.host_payload, self.extension_payload = host_payload, extension_payload
        self.host_team, self.extension_team = host_team, extension_team
        self.host_verify, self.extension_verify = host_verify, extension_verify
        self.host_metadata, self.extension_metadata = host_metadata, extension_metadata
        self.host_entitlement_status = host_entitlement_status
        self.extension_entitlement_status = extension_entitlement_status
        self.timeout_program = timeout_program
        self.calls = []

    def __call__(self, command, **kwargs):
        self.calls.append((command, kwargs))
        executable = Path(command[0]).name
        if executable == self.timeout_program:
            raise subprocess.TimeoutExpired(command, kwargs.get('timeout'))
        if executable == 'security':
            return SimpleNamespace(returncode=0, stdout='0 valid identities found\n', stderr='')
        if executable == 'pluginkit':
            return SimpleNamespace(returncode=0, stdout=EXT_ID, stderr='')
        if executable != 'codesign':
            raise AssertionError('unexpected command: ' + repr(command))
        extension = str(command[-1]).endswith('.appex')
        role_index = 1 if extension else 0
        if '--verify' in command:
            status = (self.host_verify, self.extension_verify)[role_index]
            return SimpleNamespace(returncode=status, stdout=b'', stderr=b'signature verification')
        if '-dv' in command:
            status = (self.host_metadata, self.extension_metadata)[role_index]
            identifier = EXT_ID if extension else HOST_ID
            team = self.extension_team if extension else self.host_team
            metadata = f'Identifier={identifier}\nTeamIdentifier={team}\n'.encode()
            return SimpleNamespace(returncode=status, stdout=b'', stderr=metadata)
        if '--entitlements' in command:
            status = (self.host_entitlement_status, self.extension_entitlement_status)[role_index]
            payload = self.extension_payload if extension else self.host_payload
            if payload is None:
                entitlements = self.extension_entitlements if extension else self.host_entitlements
                payload = plistlib.dumps(entitlements, fmt=plistlib.FMT_XML)
            return SimpleNamespace(returncode=status, stdout=payload, stderr=b'')
        raise AssertionError('unexpected codesign invocation: ' + repr(command))


class MountPreflight(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.app = Path(self.temporary.name) / 'ForkfsRevision.app'
        host = self.app / 'Contents'
        extension = host / 'PlugIns' / 'ForkRevisionExtension.appex'
        (host / 'Info.plist').parent.mkdir(parents=True)
        (host / 'PlugIns').mkdir()
        extension.mkdir()
        (host / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': HOST_ID}))
        (extension / 'Contents').mkdir()
        (extension / 'Contents' / 'Info.plist').write_bytes(
            plistlib.dumps({'CFBundleIdentifier': EXT_ID}))

    def preflight(self, signed):
        with patch.object(mount_probe.platform, 'system', return_value='Darwin'), \
             patch.object(mount_probe.platform, 'mac_ver', return_value=('27.0', ('', '', ''), 'arm64')), \
             patch.object(mount_probe.subprocess, 'run', side_effect=signed):
            return mount_probe.preflight(self.app)

    def test_valid_signed_prerequisites_are_not_mount_acceptance(self):
        signed = SignedBundles()
        report = self.preflight(signed)
        self.assertEqual(report['status'], 'prerequisites_available')
        self.assertTrue(report['signature_verified'])
        self.assertTrue(report['team_identifiers_match'])
        self.assertTrue(report['host_entitlements_valid'])
        self.assertTrue(report['extension_entitlements_valid'])
        self.assertFalse(report['signing_identity_available'])
        self.assertFalse(report['mounted'])
        self.assertFalse(report['isolation_verified'])
        entitlement_commands = [command for command, _ in signed.calls
                               if Path(command[0]).name == 'codesign' and '--entitlements' in command]
        self.assertEqual(len(entitlement_commands), 2)
        self.assertTrue(all(command[1:5] == ['--display', '--entitlements', ':-', '--xml']
                            for command in entitlement_commands))
        self.assertTrue(all(kwargs.get('timeout') == 15 for _, kwargs in signed.calls))
        self.assertTrue(all(Path(command[0]).is_absolute() for command, _ in signed.calls))

    def test_missing_or_false_fskit_module_entitlement_blocks(self):
        cases = (
            {key: value for key, value in EXT_ENTITLEMENTS.items()
             if key != 'com.apple.developer.fskit.fsmodule'},
            dict(EXT_ENTITLEMENTS, **{'com.apple.developer.fskit.fsmodule': False}),
            dict(EXT_ENTITLEMENTS, **{'com.apple.developer.fskit.fsmodule': 1}),
        )
        for entitlements in cases:
            with self.subTest(entitlements=entitlements):
                report = self.preflight(SignedBundles(extension_entitlements=entitlements))
                self.assertEqual(report['status'], 'blocked')
                self.assertFalse(report['extension_entitlements_valid'])
                self.assertTrue(any('fskit.fsmodule' in blocker for blocker in report['blockers']))

    def test_missing_host_mount_entitlement_blocks(self):
        entitlements = dict(HOST_ENTITLEMENTS)
        del entitlements['com.apple.developer.fskit.mount']
        report = self.preflight(SignedBundles(host_entitlements=entitlements))
        self.assertEqual(report['status'], 'blocked')
        self.assertTrue(any('fskit.mount' in blocker for blocker in report['blockers']))

    def test_team_mismatch_blocks_even_when_both_signatures_verify(self):
        report = self.preflight(SignedBundles(extension_team='TEAM-B'))
        self.assertTrue(report['signature_verified'])
        self.assertFalse(report['team_identifiers_match'])
        self.assertEqual(report['status'], 'blocked')

    def test_missing_team_and_wrong_info_identifier_block(self):
        report = self.preflight(SignedBundles(host_team='not set'))
        self.assertEqual(report['status'], 'blocked')
        self.assertTrue(any('no verified signing team' in blocker for blocker in report['blockers']))
        host_info = self.app/'Contents'/'Info.plist'
        host_info.write_bytes(plistlib.dumps({'CFBundleIdentifier':'wrong.identifier'}))
        report = self.preflight(SignedBundles())
        self.assertEqual(report['status'], 'blocked')
        self.assertTrue(any('Info.plist bundle identifier' in blocker for blocker in report['blockers']))

    def test_malformed_and_truncated_signed_entitlements_block(self):
        for payload in (b'<plist><dict>', b'<?xml version="1.0"?><plist><dict><key>'):
            with self.subTest(payload=payload):
                report = self.preflight(SignedBundles(extension_payload=payload))
                self.assertEqual(report['status'], 'blocked')
                self.assertTrue(any('extension signed entitlements' in blocker
                                    for blocker in report['blockers']))

    def test_codesign_failures_block(self):
        report = self.preflight(SignedBundles(extension_verify=1))
        self.assertEqual(report['status'], 'blocked')
        self.assertFalse(report['extension_signature_verified'])
        self.assertTrue(any('extension code signature verification failed' in blocker
                            for blocker in report['blockers']))

    def test_entitlement_codesign_failure_blocks(self):
        report = self.preflight(SignedBundles(extension_entitlement_status=1))
        self.assertEqual(report['status'], 'blocked')
        self.assertTrue(any('extension signed entitlements are malformed or unavailable'
                            in blocker for blocker in report['blockers']))

    def test_preflight_command_timeouts_block(self):
        expected = {
            'codesign': 'code signature verification could not run',
            'security': 'identity inspection timed out',
            'pluginkit': 'registration check timed out',
        }
        for program, message in expected.items():
            with self.subTest(program=program):
                report = self.preflight(SignedBundles(timeout_program=program))
                self.assertEqual(report['status'], 'blocked')
                self.assertTrue(any(message in blocker for blocker in report['blockers']))


if __name__ == '__main__': unittest.main()
