"""Credential-before-transmission and bounded mount provisioning admission."""
import json
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'fskit'))
import mount_session


class Socket:
    def __init__(self, reply):self.reply=reply;self.sent=[]
    def __enter__(self):return self
    def __exit__(self,*args):pass
    def settimeout(self,value):pass
    def connect(self,path):pass
    def fileno(self):return 123
    def sendall(self,value):self.sent.append(value)
    def recv(self,count):value,self.reply=self.reply[:min(count,3)],self.reply[min(count,3):];return value


class MountSession(unittest.TestCase):
    def call(self, uid=70, reply=None):
        if reply is None:
            payload=struct.pack('<I',0)+json.dumps(dict(protocol=1,generation='a'*32)).encode()
            reply=b'FFRSP001'+struct.pack('<I',len(payload))+payload
        peer=Socket(reply)
        class Lookup:
            def __call__(self,fd,user,group):user._obj.value=uid;return 0
        with patch.object(mount_session.socket,'socket',return_value=peer), \
             patch.object(mount_session.ctypes,'CDLL',return_value=SimpleNamespace(getpeereid=Lookup())):
            try:return mount_session.request('/socket',70,['service-hello']),peer
            except Exception as e:return e,peer

    def test_peer_uid_checked_before_any_request_bytes(self):
        result,peer=self.call(uid=71)
        self.assertIsInstance(result,ValueError)
        self.assertEqual(peer.sent,[])

    def test_partial_reads_reassemble_response(self):
        result,peer=self.call()
        self.assertEqual(result,dict(protocol=1,generation='a'*32))
        self.assertEqual(peer.sent[0][:8],b'FFRPC001')

    def test_malformed_truncated_and_oversized_frames(self):
        for reply in (b'FFRSP001'+struct.pack('<I',mount_session.LIMIT+1),
                      b'BADMAGIC'+struct.pack('<I',4),b'FFRSP001'+struct.pack('<I',4)+b'\0'):
            with self.subTest(reply=reply):
                result,_=self.call(reply=reply)
                self.assertIsInstance(result,ValueError)

    def test_tokens_are_fixed_lowercase_hex(self):
        self.assertTrue(mount_session.token('a'*32))
        for value in (None,1,'A'*32,'a'*31,'g'*32,'a'*33):self.assertFalse(mount_session.token(value))

    def test_nonroot_cannot_provision(self):
        with patch.object(mount_session.sys,'platform','darwin'),patch.object(mount_session.os,'getuid',return_value=501):
            with self.assertRaisesRegex(RuntimeError,'root'):mount_session.provision('/absent',70,501,20,'main')


if __name__=='__main__':unittest.main()
