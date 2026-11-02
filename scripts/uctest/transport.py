"""Two channels (commands out, events in) carrying JSON lines: Windows named pipes, UNIX domain sockets elsewhere (game/test_pipe_posix.cpp)."""
import os
import socket
import sys
import time


class PipeTransport:
    def __init__(self, name, connect_seconds=None, alive=None):
        self.name = name
        if connect_seconds is None:   # the game opens the channels after its pipeline warm-up: a cold cache on a Vulkan-on-Metal driver takes ~1 min
            connect_seconds = 20.0 if sys.platform == "win32" else 180.0
        deadline = time.monotonic() + connect_seconds
        if sys.platform == "win32":
            self._cmd = self._open(rf"\\.\pipe\uc-test-{name}-cmd", "wb", deadline, alive)
            self._evt = self._open(rf"\\.\pipe\uc-test-{name}-evt", "rb", deadline, alive)
        else:
            base = os.path.join(os.environ.get("TMPDIR") or "/tmp", f"uc-test-{name}")
            self._cmd = self._connect(base + "-cmd.sock", "wb", deadline, alive)
            self._evt = self._connect(base + "-evt.sock", "rb", deadline, alive)

    @classmethod
    def tcp(cls, name, port, connect_seconds=180.0):
        """The channels of a game on the Quest, forwarded by adb (adb forward tcp:PORT localabstract:uc-test-NAME-cmd, PORT+1 the events)."""
        self = cls.__new__(cls)
        self.name = name
        self._cmd = socket.create_connection(("127.0.0.1", port), timeout=connect_seconds).makefile("wb", buffering=0)
        events = socket.create_connection(("127.0.0.1", port + 1), timeout=connect_seconds)
        events.settimeout(None)
        self._evt = events.makefile("rb", buffering=0)
        return self

    def _open(self, path, mode, deadline, alive):
        while True:
            try:
                return open(path, mode, buffering=0)
            except (FileNotFoundError, PermissionError):
                self._wait(deadline, alive)

    def _connect(self, path, mode, deadline, alive):
        while True:
            client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                client.connect(path)
                return client.makefile(mode, buffering=0)
            except (FileNotFoundError, ConnectionRefusedError):
                client.close()
                self._wait(deadline, alive)

    def _wait(self, deadline, alive):
        if time.monotonic() >= deadline or (alive is not None and not alive()):
            raise TimeoutError(f"test pipe uc-test-{self.name} not available")
        time.sleep(0.05)

    def send_line(self, line):
        self._cmd.write(line.encode("utf-8") + b"\n")

    def read_line(self):
        try:
            data = self._evt.readline()
        except (OSError, ValueError):   # ValueError: the channel was closed under the reader at shutdown
            return None
        return data.decode("utf-8").rstrip("\r\n") if data else None

    def close(self):
        for handle in (self._cmd, self._evt):
            try:
                handle.close()
            except OSError:
                pass
