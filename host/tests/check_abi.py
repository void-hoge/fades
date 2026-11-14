#!/usr/bin/env python3
"""Real-board ABI v8 validation, per-buffer mappings and file/VMA lifetime."""
import errno
import fcntl
import mmap
import os
import sys

from fades_device import (BASE, BUF, CONFIG, CONFIG_CMD, DQBUF, INFO_CMD,
                          OFF, ON, QBUF, QUERY, QUERY_CMD, STRIDE,
                          expect_errno, info, query)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/dev/fades"
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    maps = []
    try:
        assert info(fd) == (8, STRIDE, 4096, 0, 0, 0)
        expect_errno(errno.EBUSY, lambda: os.open(path, os.O_RDWR))
        expect_errno(errno.ENOTTY, lambda: fcntl.ioctl(fd, BASE | 2))
        for cmd in (INFO_CMD, CONFIG_CMD, QUERY_CMD, QBUF, DQBUF):
            expect_errno(errno.EFAULT, lambda cmd=cmd: fcntl.ioctl(fd, cmd, 0))
        expect_errno(errno.ENOTTY, lambda: fcntl.ioctl(fd, BASE | 7))
        expect_errno(errno.EINVAL, lambda: fcntl.ioctl(fd, ON))
        expect_errno(errno.EINVAL, lambda: query(fd, 0, 0))
        expect_errno(errno.EINVAL, lambda: mmap.mmap(fd, STRIDE, flags=mmap.MAP_SHARED))
        for cmd in (QBUF, DQBUF):
            expect_errno(errno.EINVAL, lambda cmd=cmd:
                         fcntl.ioctl(fd, cmd, BUF.pack(0, 0, 1, 0)))
        for cfg in ((0, 1, 0, 0), (1, 0, 0, 0), (4097, 1, 0, 0),
                    (1, 4097, 0, 0), (1, 1, 1, 0), (1, 1, 0, 1)):
            expect_errno(errno.EINVAL, lambda cfg=cfg:
                         fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(*cfg)))
            assert info(fd)[3:5] == (0, 0)
        # KV260 has 4 GiB: reject an impossible 8 GiB request without touching
        # the allocator; then successfully configure the same descriptor.
        if os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE") < 8192 * STRIDE:
            expect_errno(errno.ENOMEM, lambda:
                         fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(4096, 4096, 0, 0)))
            assert info(fd)[3:5] == (0, 0)
        fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(3, 65, 0, 0))
        assert info(fd) == (8, STRIDE, 4096, 3, 65, 0)
        expect_errno(errno.EBUSY, lambda:
                     fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(1, 1, 0, 0)))
        offsets = set()
        for q, count in enumerate((3, 65)):
            expect_errno(errno.EINVAL, lambda q=q, count=count: query(fd, q, count))
            for i in range(count):
                offset, size = query(fd, q, i)
                assert size == STRIDE and offset not in offsets
                offsets.add(offset)
                maps.append(mmap.mmap(fd, size, flags=mmap.MAP_SHARED, offset=offset))
                maps[-1][:8] = (q * 1000 + i).to_bytes(8, "little")
            for cmd in (QBUF, DQBUF):
                expect_errno(errno.EINVAL, lambda cmd=cmd, q=q:
                             fcntl.ioctl(fd, cmd, BUF.pack(q, 0, 0, 1)))
            expect_errno(errno.EAGAIN, lambda q=q:
                         fcntl.ioctl(fd, DQBUF, BUF.pack(q, 0, 0, 0)))
            b = BUF.pack(q, count - 1, 1 if q == 0 else 0, 0)
            fcntl.ioctl(fd, QBUF, b)
            expect_errno(errno.EBUSY, lambda b=b: fcntl.ioctl(fd, QBUF, b))
            expect_errno(errno.EINVAL, lambda q=q, count=count:
                         fcntl.ioctl(fd, QBUF, BUF.pack(q, count, 1 if q == 0 else 0, 0)))
        expect_errno(errno.EINVAL, lambda: query(fd, 2, 0))
        for b in ((2, 0, 1, 0), (0, 0, 0, 0), (0, 0, 16385, 0), (1, 0, 1, 0)):
            expect_errno(errno.EINVAL, lambda b=b: fcntl.ioctl(fd, QBUF, BUF.pack(*b)))
        fcntl.ioctl(fd, OFF)
        fcntl.ioctl(fd, QBUF, BUF.pack(0, 2, 1, 0))
        fcntl.ioctl(fd, OFF)
        for size, flags, offset in ((STRIDE // 2, mmap.MAP_SHARED, 0),
                                    (STRIDE * 2, mmap.MAP_SHARED, 0),
                                    (STRIDE, mmap.MAP_PRIVATE, 0),
                                    (STRIDE, mmap.MAP_SHARED, mmap.PAGESIZE),
                                    (STRIDE, mmap.MAP_SHARED, 3 * STRIDE),
                                    (STRIDE, mmap.MAP_SHARED, 8192 * STRIDE)):
            expect_errno(errno.EINVAL, lambda size=size, flags=flags, offset=offset:
                         mmap.mmap(fd, size, flags=flags, offset=offset))
        for i, m in enumerate(maps):
            expected = i if i < 3 else 1000 + i - 3
            assert int.from_bytes(m[:8], "little") == expected, "Mapping alias"
        # A VMA (also across fork) pins its file: close(fd) must not release
        # ownership, free storage or allow another open while mappings exist.
        os.close(fd)
        fd = None
        expect_errno(errno.EBUSY, lambda: os.open(path, os.O_RDWR))
        pid = os.fork()
        if pid == 0:
            try:
                assert int.from_bytes(maps[-1][:8], "little") == 1064
                expect_errno(errno.EBUSY, lambda: os.open(path, os.O_RDWR))
            except BaseException:
                os._exit(1)
            os._exit(0)
        assert os.waitpid(pid, 0)[1] == 0
        print("PASS: ABI v8, old STREAMOFF rejected, asymmetric bounds, all individual mappings, ownership, close/fork lifetime")
    finally:
        for m in maps:
            m.close()
        if fd is not None:
            os.close(fd)
    fd = os.open(path, os.O_RDWR)
    try:
        assert info(fd)[3:5] == (0, 0)
        fcntl.ioctl(fd, CONFIG_CMD, CONFIG.pack(1, 1, 0, 0))
    finally:
        os.close(fd)
    print("PASS: final unmap releases ownership; reopen starts unconfigured")


if __name__ == "__main__":
    main()
