"""Exercise the real console server without UART access or a usbip-win2 attachment.

Run after tools\\build.cmd: python tools/usbip_selftest.py
DECKBT_USBIP_EXE selects another build (for example one made with DECKBT_BUILD_OUT).
"""

import contextlib
import os
import pathlib
import socket
import struct
import subprocess
import tempfile
import time
import unittest

EXE = pathlib.Path(os.environ.get("DECKBT_USBIP_EXE") or
                   pathlib.Path(__file__).resolve().parent / "_build" / "deckbt-usbip.exe")
OP = struct.Struct(">HHI")
IMPORT = OP.pack(0x111, 0x8003, 0) + b"1-1".ljust(32, b"\0")
DEVLIST = OP.pack(0x111, 0x8005, 0)


def receive(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError(f"closed at {len(data)}/{length}")
        data.extend(chunk)
    return bytes(data)


def submit(seq, direction, endpoint, length, packets=0xFFFFFFFF, setup=b"\0" * 8):
    return struct.pack(">10I8s", 1, seq, 0x10002, direction, endpoint,
                       0, length, 0, packets, 1, setup)


def unlink(seq, target):
    return struct.pack(">6I24x", 2, seq, 0x10002, 0, 0, target)


def hci_command(seq, opcode):
    command = struct.pack("<HB", opcode, 0)
    setup = struct.pack("<BBHHH", 0x20, 0, 0, 0, len(command))
    return submit(seq, 0, 0, len(command), setup=setup) + command


def reply(sock, out=()):
    """(command, seqnum, status, payload) of one RET_SUBMIT / RET_UNLINK without iso descriptors.
    Replies to the OUT transfers listed in out carry no payload."""
    header = receive(sock, 48)
    command, seq = struct.unpack_from(">II", header)
    status, actual = struct.unpack_from(">iI", header, 20)
    payload = receive(sock, actual) if command == 3 and seq not in out else b""
    return command, seq, status, payload


@contextlib.contextmanager
def server(allow_import=False):
    with tempfile.TemporaryDirectory(prefix="deckbt-usbip-test-") as temp:
        stop = pathlib.Path(temp) / "stop"
        with socket.socket() as reserve:
            reserve.bind(("127.0.0.1", 0))
            port = reserve.getsockname()[1]
        args = [str(EXE), "--backend", "stub", "--no-attach", "--port", str(port),
                "--stop-file", str(stop), "--quiet"]
        if allow_import:
            args.append("--allow-user-import")
        process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError("server did not listen")
                    time.sleep(0.02)
            yield port
        finally:
            stop.touch()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("server did not stop with pending clients")
            finally:
                process.stderr.close()
            if process.returncode:
                raise AssertionError(f"server exited {process.returncode}")


class SocketTests(unittest.TestCase):
    def connect(self, port):
        sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.addCleanup(sock.close)
        return sock

    def import_device(self, port):
        sock = self.connect(port)
        sock.sendall(IMPORT)
        self.assertEqual(OP.unpack(receive(sock, 8)), (0x111, 3, 0))
        receive(sock, 312)
        return sock

    def test_silent_and_fragmented_clients_do_not_block_discovery(self):
        with server() as port:
            silent = self.connect(port)
            fragment = self.connect(port)
            fragment.sendall(DEVLIST[:3])
            client = self.connect(port)
            start = time.monotonic()
            client.sendall(DEVLIST)
            self.assertEqual(OP.unpack(receive(client, 8)), (0x111, 5, 0))
            self.assertEqual(struct.unpack(">I", receive(client, 4))[0], 1)
            self.assertLess(time.monotonic() - start, 1.5)
            fragment.sendall(DEVLIST[3:])
            self.assertEqual(OP.unpack(receive(fragment, 8)), (0x111, 5, 0))
            self.assertEqual(silent.recv(1), b"")

    def test_slow_import_has_absolute_deadline(self):
        with server() as port:
            sock = self.connect(port)
            sock.sendall(IMPORT[:8])
            start = time.monotonic()
            for byte in IMPORT[8:12]:
                time.sleep(0.35)
                sock.sendall(bytes([byte]))
            try:
                self.assertEqual(sock.recv(1), b"")
            except ConnectionResetError:
                pass
            self.assertLess(time.monotonic() - start, 2.8)

    def test_user_import_denied_even_without_active_session(self):
        with server() as port:
            sock = self.connect(port)
            sock.sendall(IMPORT)
            version, code, status = OP.unpack(receive(sock, 8))
            self.assertEqual((version, code), (0x111, 3))
            self.assertNotEqual(status, 0)
            self.assertEqual(sock.recv(1), b"")

    def test_rejected_iso_does_not_consume_next_reply(self):
        with server(allow_import=True) as port:
            sock = self.import_device(port)
            # Voice is disabled (alternate setting 0); both directions must reject cleanly.
            for seq, direction in ((1, 0), (2, 1)):
                sock.sendall(submit(seq, direction, 3, 0, packets=1) + b"\0" * 16)
                reply = receive(sock, 48)
                self.assertEqual(struct.unpack_from(">II", reply), (3, seq))
                self.assertNotEqual(struct.unpack_from(">i", reply, 20)[0], 0)
                self.assertEqual(struct.unpack_from(">I", reply, 24)[0], 0)
                self.assertEqual(struct.unpack_from(">I", reply, 32)[0], 0)
            sock.sendall(submit(3, 1, 0, 18, setup=bytes.fromhex("8006000100001200")))
            reply = receive(sock, 48)
            self.assertEqual(struct.unpack_from(">II", reply), (3, 3))
            self.assertEqual(struct.unpack_from(">iI", reply, 20), (0, 18))
            self.assertEqual(receive(sock, 18)[:2], b"\x12\x01")
            sock.close()

    def test_reply_unlinked_in_flight_is_delivered_again(self):
        # usbip-win2 completes a transfer as cancelled when it sends CMD_UNLINK and drops a
        # RET_SUBMIT that crossed it, so an event already sent for it never reached Windows.
        with server(allow_import=True) as port:
            sock = self.import_device(port)
            sock.sendall(submit(10, 1, 1, 64) + submit(11, 1, 1, 64))
            sock.sendall(hci_command(20, 0x1009) + hci_command(21, 0x1001))
            events = {}
            for _ in range(4):
                command, seq, status, payload = reply(sock, out=(20, 21))
                self.assertEqual((command, status), (3, 0))
                if seq in (10, 11):
                    events[seq] = payload
            self.assertEqual(sorted(events), [10, 11])
            self.assertEqual(events[10][3:5], b"\x09\x10")
            self.assertEqual(events[11][3:5], b"\x01\x10")

            sock.sendall(unlink(30, 11) + unlink(31, 10))
            self.assertEqual(reply(sock)[:3], (4, 30, 0))
            self.assertEqual(reply(sock)[:3], (4, 31, 0))

            sock.sendall(submit(40, 1, 1, 64) + submit(41, 1, 1, 64) + submit(42, 1, 1, 64))
            self.assertEqual(reply(sock), (3, 40, 0, events[10]))
            self.assertEqual(reply(sock), (3, 41, 0, events[11]))
            # Nothing else is owed: the third read stays parked.
            sock.settimeout(0.3)
            with self.assertRaises(socket.timeout):
                sock.recv(1)
            sock.close()

    def test_stop_with_full_handshake_pool(self):
        with server() as port:
            sockets = [self.connect(port) for _ in range(8)]
            for sock in sockets:
                sock.sendall(b"\x01")
        # The context manager requires a clean exit, not a forced process kill.

    def test_stop_with_live_session(self):
        with server(allow_import=True) as port:
            sock = self.import_device(port)
            sock.sendall(submit(1, 1, 1, 64))
        # The context manager required a prompt exit; the server closed the session itself.
        sock.settimeout(1)
        self.assertEqual(sock.recv(1), b"")


if __name__ == "__main__":
    unittest.main(verbosity=2)
