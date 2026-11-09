"""The loopback bridge must authenticate before touching the daemon socket."""
import hashlib
import hmac
import importlib.util
from pathlib import Path
import secrets
import socket
import struct
import tempfile
import threading
import time
import unittest

spec = importlib.util.spec_from_file_location('bridge', Path(__file__).resolve().parents[1] / 'fskit-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeAuthentication(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='ff-bridge-', dir='/tmp')
        self.backend = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.path = str(Path(self.tmp.name) / 'control.sock')
        self.backend.bind(self.path)
        self.backend.listen(4)
        self.backend.settimeout(0.1)
        self.key = secrets.token_bytes(32)
        self.workers = []

    def tearDown(self):
        for worker in self.workers:
            worker.join(timeout=2)
            self.assertFalse(worker.is_alive())
        self.backend.close()
        self.tmp.cleanup()

    def connect(self):
        client, server = socket.socketpair()
        client.settimeout(2)
        worker = threading.Thread(target=bridge.handle, args=(server, self.path, self.key))
        worker.start()
        self.workers.append(worker)
        return client

    def authenticate(self, peer, *, key=None, proof=None):
        key = self.key if key is None else key
        nonce = secrets.token_bytes(32)
        # Split the handshake to exercise partial network reads.
        peer.sendall(b'FFBR001!')
        peer.sendall(nonce)
        reply = bridge.receive(peer, 64, time.monotonic() + 2)
        nonces = nonce + reply[:32]
        self.assertTrue(hmac.compare_digest(reply[32:], hmac.digest(self.key, b'FFBR001S' + nonces, hashlib.sha256)))
        client_proof = hmac.digest(key, b'FFBR001C' + nonces, hashlib.sha256) if proof is None else proof
        peer.sendall(client_proof)
        return client_proof

    def no_backend(self):
        with self.assertRaises(socket.timeout):
            self.backend.accept()

    def test_valid_request_and_binary_response(self):
        with self.connect() as peer:
            self.authenticate(peer)
            request = b'FFRPC001' + struct.pack('<II', 4, 0)
            for part in (request[:3], request[3:11], request[11:]):
                peer.sendall(part)
            backend, _ = self.backend.accept()
            with backend:
                self.assertEqual(bridge.receive(backend, len(request), time.monotonic() + 2), request)
                payload = struct.pack('<I', 0) + b'binary\x00\xff'
                response = b'FFRSP001' + struct.pack('<I', len(payload)) + payload
                backend.sendall(response)
            self.assertEqual(bridge.receive(peer, len(response), time.monotonic() + 2), response)

    def test_wrong_key_is_rejected_before_daemon_connection(self):
        with self.connect() as peer:
            self.authenticate(peer, key=secrets.token_bytes(32))
            self.assertEqual(peer.recv(1), b'')
        self.no_backend()

    def test_replaying_a_previous_proof_is_rejected(self):
        with self.connect() as peer:
            proof = self.authenticate(peer)
        with self.connect() as peer:
            self.authenticate(peer, proof=proof)
            self.assertEqual(peer.recv(1), b'')
        self.no_backend()

    def test_oversized_frame_is_rejected_before_daemon_connection(self):
        with self.connect() as peer:
            self.authenticate(peer)
            peer.sendall(b'FFRPC001' + struct.pack('<I', bridge.LIMIT + 1))
            self.assertEqual(peer.recv(1), b'')
        self.no_backend()

    def test_unauthenticated_rpc_is_rejected(self):
        with self.connect() as peer:
            peer.sendall(b'FFRPC001' + b'\0' * 32)
            self.assertEqual(peer.recv(1), b'')
        self.no_backend()


if __name__ == '__main__':
    unittest.main()
