"""Small real-board ABI v8 test helper (no record copies on dequeue)."""
import errno
import fcntl
import mmap
import os
import select
import struct

BASE = ord("F") << 8
INFO = struct.Struct("=6I")
CONFIG = BUF = struct.Struct("=4I")
QUERY = struct.Struct("=IIQQ")
INFO_CMD = (2 << 30) | (INFO.size << 16) | BASE
CONFIG_CMD = (1 << 30) | (CONFIG.size << 16) | BASE | 8
QUERY_CMD = (3 << 30) | (QUERY.size << 16) | BASE | 9
QBUF = (1 << 30) | (BUF.size << 16) | BASE | 3
DQBUF = (3 << 30) | (BUF.size << 16) | BASE | 4
STATS = (2 << 30) | (352 << 16) | BASE | 2
ON, OFF = (BASE | i for i in (5, 6))
STRIDE = 1024 * 1024
RECORDS = STRIDE // 64


def expect_errno(code, action):
    try:
        value = action()
    except OSError as exc:
        assert exc.errno == code, (code, exc.errno)
    else:
        if hasattr(value, "close"):
            value.close()
        elif isinstance(value, int):
            os.close(value)
        raise AssertionError(f"Expected errno {code}")


def info(fd):
    result = bytearray(INFO.size)
    fcntl.ioctl(fd, INFO_CMD, result, True)
    return INFO.unpack(result)


def query(fd, q, index):
    result = bytearray(QUERY.pack(q, index, 0, 0))
    fcntl.ioctl(fd, QUERY_CMD, result, True)
    return QUERY.unpack(result)[2:]


class Device:
    def __init__(self, path, ntx, nrx):
        self.fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        self.maps = [[], []]
        try:
            assert info(self.fd) == (8, STRIDE, 4096, 0, 0, 0)
            fcntl.ioctl(self.fd, CONFIG_CMD, CONFIG.pack(ntx, nrx, 0, 0))
            for q, count in enumerate((ntx, nrx)):
                for i in range(count):
                    offset, size = query(self.fd, q, i)
                    assert size == STRIDE and offset % mmap.PAGESIZE == 0
                    self.maps[q].append(mmap.mmap(self.fd, size,
                        flags=mmap.MAP_SHARED, offset=offset))
            self.poll = select.poll()
            self.poll.register(self.fd, select.POLLIN | select.POLLOUT)
        except BaseException:
            self.close()
            raise

    def queue(self, q, index, records=0):
        fcntl.ioctl(self.fd, QBUF, BUF.pack(q, index, records, 0))

    def dequeue(self, q):
        result = bytearray(BUF.pack(q, 0, 0, 0))
        try:
            fcntl.ioctl(self.fd, DQBUF, result, True)
        except OSError as exc:
            if exc.errno == errno.EAGAIN:
                return None
            raise
        return BUF.unpack(result)[1:3]

    def wait(self, events=select.POLLIN | select.POLLOUT):
        self.poll.modify(self.fd, events)
        ready = self.poll.poll(10000)
        assert ready, "DMA progress timeout"
        assert not ready[0][1] & (select.POLLERR | select.POLLHUP | select.POLLNVAL)

    def stats(self):
        result = bytearray(352)
        fcntl.ioctl(self.fd, STATS, result, True)
        return struct.unpack("=44Q", result)

    def start(self):
        fcntl.ioctl(self.fd, ON)

    def stop(self):
        fcntl.ioctl(self.fd, OFF)

    def close(self):
        try:
            self.stop()
        finally:
            for queue in self.maps:
                for buffer in queue:
                    buffer.close()
            os.close(self.fd)
