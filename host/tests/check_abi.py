#!/usr/bin/env python3
"""Exercise rejection paths on the real module without issuing a DMA transfer."""

import errno
import fcntl
import mmap
import os
import struct
import sys

INFO = struct.Struct("=IIIIQ")
RUN = struct.Struct("=IIIIQQ")
INFO_CMD = (2 << 30) | (INFO.size << 16) | (ord("F") << 8)
RUN_CMD = (3 << 30) | (RUN.size << 16) | (ord("F") << 8) | 1


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
        assert (version, slots, slot_bytes, reserved) == (1, 8, 1048576, 0)
        assert map_bytes == slots * slot_bytes
        expect_errno(errno.EBUSY, lambda: os.open(device, os.O_RDWR | os.O_CLOEXEC))
        expect_errno(errno.ENOTTY, lambda: fcntl.ioctl(fd, (ord("F") << 8) | 2))
        expect_errno(errno.EFAULT, lambda: fcntl.ioctl(fd, INFO_CMD, 0))
        expect_errno(errno.EFAULT, lambda: fcntl.ioctl(fd, RUN_CMD, 0))

        # All of these must fail validation before touching either DMA channel.
        invalid = [
            (0, 1, 1, 1000),
            (65, 1, 1, 1000),
            (slot_bytes + 64, 1, 1, 1000),
            (64, 0, 1, 1000),
            (64, slots + 1, 1, 1000),
            (64, 1, 0, 1000),
            (64, 1, 100001, 1000),
            (64, 1, 1, 0),
            (64, 1, 1, 60001),
        ]
        for args in invalid:
            request = bytearray(RUN.pack(*args, 0, 0))
            expect_errno(errno.EINVAL, lambda: fcntl.ioctl(fd, RUN_CMD, request, True))

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
