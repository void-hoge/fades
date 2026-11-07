#!/usr/bin/env python3
"""Real-board ABI v2 tests: mixed packets, ring wrap, ownership, stalls, reset."""
import errno
import fcntl
import mmap
import os
import select
import struct
import sys
import time

BUF = struct.Struct("=IIII")
INFO = struct.Struct("=IIIIQ")
BASE = ord("F") << 8
INFO_CMD = (2 << 30) | (INFO.size << 16) | BASE
QBUF = (1 << 30) | (BUF.size << 16) | BASE | 3
DQBUF = (3 << 30) | (BUF.size << 16) | BASE | 4
STATS = (2 << 30) | (352 << 16) | BASE | 2
ON, END, OFF = BASE | 5, BASE | 6, BASE | 7
DIGITS = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
CELLS = sum(int(d) << (4 * i) for i, d in enumerate(DIGITS))


def expect_errno(code, action):
    try:
        action()
    except OSError as exc:
        assert exc.errno == code, (code, exc.errno)
    else:
        raise AssertionError(f"Expected errno {code}")


def main():
    fd = os.open(sys.argv[1] if len(sys.argv) > 1 else "/dev/fades", os.O_RDWR)
    tx = rx = None
    try:
        info = bytearray(INFO.size)
        fcntl.ioctl(fd, INFO_CMD, info, True)
        version, slots, stride, _, size = INFO.unpack(info)
        assert version == 2
        tx = mmap.mmap(fd, size, flags=mmap.MAP_SHARED)
        rx = mmap.mmap(fd, size, flags=mmap.MAP_SHARED, offset=size)

        def queue(q, index, records=0):
            fcntl.ioctl(fd, QBUF, BUF.pack(q, index, records, 0))

        def dequeue(q):
            b = bytearray(BUF.pack(q, 0, 0, 0))
            try:
                fcntl.ioctl(fd, DQBUF, b, True)
                return BUF.unpack(b)[1:3]
            except OSError as exc:
                if exc.errno == errno.EAGAIN:
                    return None
                raise

        # No input, clean finish, then restart without module/FPGA reload.
        fcntl.ioctl(fd, ON)
        expect_errno(errno.EBUSY, lambda: fcntl.ioctl(fd, ON))
        fcntl.ioctl(fd, END)
        expect_errno(errno.EPIPE, lambda: queue(0, 0, 1))
        fcntl.ioctl(fd, OFF)

        # Queued input must wait for RX capacity. STREAMOFF cancels it.
        queue(0, 0, 1)
        fcntl.ioctl(fd, ON)
        poll = select.poll()
        poll.register(fd, select.POLLOUT)
        assert not poll.poll(20), "TX ran without an RX reservation"
        fcntl.ioctl(fd, OFF)

        lengths = [1, 7, 10000, 16384, 31, 3] * 16 + [19]
        total = sum(lengths)
        expected_packets = sorted(lengths)
        seen = bytearray(total)
        submitted = received = next_packet = 0
        sealed = False
        held = []
        delay_done = False
        packet_lengths = []

        def submit(index):
            nonlocal submitted, next_packet, sealed
            if next_packet == len(lengths):
                if not sealed:
                    fcntl.ioctl(fd, END)
                    sealed = True
                return
            n = lengths[next_packet]
            data = bytearray(n * 64)
            for j in range(n):
                rid = submitted + j
                cells = CELLS
                if rid % 13 == 1:  # invalid cell -> status 2
                    cells = 15
                elif rid % 13 == 2:  # duplicate givens -> status 1
                    cells = 0x11
                struct.pack_into("<Q", data, j * 64, rid)
                data[j * 64 + 8:j * 64 + 49] = cells.to_bytes(41, "little")
            tx[index * stride:index * stride + len(data)] = data
            queue(0, index, n)
            expect_errno(errno.EBUSY, lambda: queue(0, index, n))
            submitted += n
            next_packet += 1

        for i in range(slots):
            queue(1, i)
            submit(i)
        fcntl.ioctl(fd, ON)
        while not sealed or received != submitted:
            poll.modify(fd, select.POLLIN | (0 if sealed else select.POLLOUT))
            events = poll.poll(10000)
            assert events, "stream progress timeout"
            assert not events[0][1] & (select.POLLERR | select.POLLHUP | select.POLLNVAL)
            while (done := dequeue(1)) is not None:
                index, n = done
                packet_lengths.append(n)
                data = rx[index * stride:index * stride + n * 64]
                for j in range(n):
                    r = data[j * 64:(j + 1) * 64]
                    rid = int.from_bytes(r[:8], "little")
                    assert rid < submitted and not seen[rid], ("ID", rid)
                    seen[rid] = 1
                    status = 2 if rid % 13 == 1 else 1 if rid % 13 == 2 else 0
                    assert r[8] == status
                    assert int.from_bytes(r[9:50], "little") == (0 if status else CELLS)
                    assert r[50:] == bytes(14)
                received += n
                if not delay_done:
                    held.append(index)
                    if len(held) == slots:
                        # Exhaust RX while more TX is queued, then resume.
                        time.sleep(0.02)
                        for s in held:
                            queue(1, s)
                        held.clear()
                        delay_done = True
                else:
                    queue(1, index)
            if not sealed:
                while (done := dequeue(0)) is not None:
                    submit(done[0])
                    if sealed:
                        break
        assert received == total and all(seen)
        assert sorted(packet_lengths) == expected_packets
        stats = bytearray(352)
        fcntl.ioctl(fd, STATS, stats, True)
        counts = struct.unpack("=44Q", stats)
        assert all(counts[i] == total for i in (0, 1, 3, 4)), counts[:6]
        assert sum(counts[6:12]) == total and sum(counts[12:18]) == total
        fcntl.ioctl(fd, OFF)

        # Reset with active work and unread results, then run an empty session.
        queue(1, 0)
        queue(0, 0, 10000)
        fcntl.ioctl(fd, ON)
        fcntl.ioctl(fd, OFF)
        fcntl.ioctl(fd, ON)
        fcntl.ioctl(fd, END)
        fcntl.ioctl(fd, OFF)
        print(f"PASS: {total} IDs, {len(lengths)} variable packets, SG wrap, RX starvation, cancel/restart")
    finally:
        fcntl.ioctl(fd, OFF)
        if rx is not None:
            rx.close()
        if tx is not None:
            tx.close()
        os.close(fd)


if __name__ == "__main__":
    main()
