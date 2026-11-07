#!/usr/bin/env python3
"""Exercise ABI v2 rejection/ownership paths without issuing a DMA transfer."""

import errno
import fcntl
import mmap
import os
import struct
import sys

INFO = struct.Struct("=IIIIQ")
BUF = struct.Struct("=IIII")
INFO_CMD = (2 << 30) | (INFO.size << 16) | (ord("F") << 8)
QBUF = (1 << 30) | (BUF.size << 16) | (ord("F") << 8) | 3
DQBUF = (3 << 30) | (BUF.size << 16) | (ord("F") << 8) | 4
END = (ord("F") << 8) | 6
OFF = (ord("F") << 8) | 7


def expect_errno(expected, action):
    try:
        value = action()
    except OSError as exc:
        if exc.errno != expected:
            raise AssertionError(f"Expected errno {expected}, got {exc.errno}") from exc
    else:
        if isinstance(value, int):
            os.close(value)
        elif hasattr(value, "close"):
            value.close()
        raise AssertionError(f"Expected errno {expected}, operation succeeded")


def main():
    device = sys.argv[1] if len(sys.argv) > 1 else "/dev/fades"
    fd = os.open(device, os.O_RDWR | os.O_CLOEXEC)
    try:
        info = bytearray(INFO.size)
        fcntl.ioctl(fd, INFO_CMD, info, True)
        version, slots, slot_bytes, reserved, map_bytes = INFO.unpack(info)
        assert (version, slots, slot_bytes, reserved) == (2, 8, 1048576, 0)
        assert map_bytes == slots * slot_bytes
        expect_errno(errno.EBUSY, lambda: os.open(device, os.O_RDWR | os.O_CLOEXEC))
        expect_errno(errno.ENOTTY, lambda: fcntl.ioctl(fd, (ord("F") << 8) | 2))
        expect_errno(errno.EFAULT, lambda: fcntl.ioctl(fd, INFO_CMD, 0))
        expect_errno(errno.EFAULT, lambda: fcntl.ioctl(fd, QBUF, 0))
        expect_errno(errno.EFAULT, lambda: fcntl.ioctl(fd, DQBUF, 0))
        expect_errno(errno.EINVAL, lambda: fcntl.ioctl(fd, END))

        # All of these must fail validation before touching either DMA channel.
        invalid = [
            (2, 0, 1, 0),
            (0, slots, 1, 0),
            (0, 0, 0, 0),
            (0, 0, slot_bytes // 64 + 1, 0),
            (1, 0, 1, 0),
            (0, 0, 1, 1),
        ]
        for args in invalid:
            request = bytearray(BUF.pack(*args))
            expect_errno(errno.EINVAL, lambda: fcntl.ioctl(fd, QBUF, request))
        for queue in (0, 1):
            request = bytearray(BUF.pack(queue, 0, 0, 0))
            expect_errno(errno.EAGAIN, lambda: fcntl.ioctl(fd, DQBUF, request, True))
            request = bytearray(BUF.pack(queue, 0, 1 if queue == 0 else 0, 0))
            fcntl.ioctl(fd, QBUF, request)
            expect_errno(errno.EBUSY, lambda: fcntl.ioctl(fd, QBUF, request))
        fcntl.ioctl(fd, OFF)
        # STREAMOFF returns every buffer to CPU ownership.
        fcntl.ioctl(fd, QBUF, BUF.pack(0, 0, 1, 0))
        fcntl.ioctl(fd, OFF)

        expect_errno(errno.EINVAL, lambda: mmap.mmap(fd, slot_bytes, flags=mmap.MAP_SHARED))
        expect_errno(errno.EINVAL, lambda: mmap.mmap(fd, map_bytes, flags=mmap.MAP_PRIVATE))
        expect_errno(errno.EINVAL, lambda: mmap.mmap(
            fd, map_bytes, flags=mmap.MAP_SHARED, offset=mmap.PAGESIZE))
        print("PASS: ABI, exclusive open, ioctl validation, mmap bounds/shared-only")
    finally:
        os.close(fd)
    # A failed second open must not clear ownership or leak the exclusive lock.
    os.close(os.open(device, os.O_RDWR | os.O_CLOEXEC))
    print("PASS: close/reopen")


if __name__ == "__main__":
    main()
