#!/usr/bin/env python3
"""Real-board streaming, no-dequeue accumulation, backpressure and restart."""
import argparse
import errno
import fcntl
import select
import struct
import time

from fades_device import BASE, Device, RECORDS, STRIDE, expect_errno

DIGITS = "534678912672195348198342567859761423426853791713924856961537284287419635345286179"
CELLS = sum(int(d) << (4 * i) for i, d in enumerate(DIGITS))
SOLVED = struct.pack("<QB", 0, 0) + CELLS.to_bytes(41, "little") + bytes(14)


def fill(buffer, first, n):
    data = bytearray(n * 64)
    for j in range(n):
        rid = first + j
        cells = 15 if rid % 13 == 1 else 0x11 if rid % 13 == 2 else CELLS
        struct.pack_into("<Q", data, j * 64, rid)
        data[j * 64 + 8] = rid & 255  # Input status is ignored.
        data[j * 64 + 9:j * 64 + 50] = cells.to_bytes(41, "little")
    buffer[:len(data)] = data


def verify(buffer, n, seen):
    data = buffer[:n * 64]
    for j in range(n):
        r = data[j * 64:(j + 1) * 64]
        rid = int.from_bytes(r[:8], "little")
        assert rid < len(seen) and not seen[rid], ("duplicate/stale ID", rid)
        seen[rid] = 1
        status = 2 if rid % 13 == 1 else 1 if rid % 13 == 2 else 0
        assert r[8] == status, (rid, r[8], status)
        assert int.from_bytes(r[9:50], "little") == (0 if status else CELLS)
        assert r[50:] == bytes(14)


def totals(d, total):
    s = d.stats()
    assert all(s[i] == total for i in (0, 1, 3, 4)), s[:6]
    assert sum(s[6:12]) == sum(s[12:18]) == total
    assert all(v <= 2 for v in s[38:44]), s[38:44]


def accumulate(path, ntx, nrx):
    """All input prequeued; do not dequeue ANY RX until HW has finished.

    One full-capacity output per TX, plus a short tail, makes 65/65 exercise
    pending DMA work beyond both hardware ring lengths.
    """
    d = Device(path, ntx, nrx)
    try:
        used = min(ntx, nrx)
        lengths = [RECORDS] * (used - 1) + [19]
        total = sum(lengths)
        first = 0
        for i, n in enumerate(lengths):
            fill(d.maps[0][i], first, n)
            first += n
            d.queue(0, i, n)
        for i in range(nrx):
            d.maps[1][i][:] = b"\xa5" * STRIDE
            d.queue(1, i)
        d.start()
        deadline = time.monotonic() + 10
        while d.stats()[1] != total:
            assert time.monotonic() < deadline, "No-dequeue accumulation stalled"
            time.sleep(0.001)
        totals(d, total)
        # Waiting observes completions without consuming either queue.
        for events in (select.POLLOUT, select.POLLIN, select.POLLOUT | select.POLLIN):
            d.wait(events)
            d.wait(events)
        seen = bytearray(total)
        received = 0
        outputs = []
        while received < total:
            d.wait(select.POLLIN)
            while (done := d.dequeue(1)) is not None:
                i, n = done
                verify(d.maps[1][i], n, seen)
                received += n
                outputs.append(n)
        assert received == total and all(seen)
        assert outputs == lengths, outputs
        # Uncollected TX completions must not make an RX-only poll ready.
        d.poll.modify(d.fd, select.POLLIN)
        assert not d.poll.poll(20), "RX-only poll reported TX completions"
        d.wait(select.POLLOUT)
        assert sum(1 for _ in iter(lambda: d.dequeue(0), None)) == used
        print(f"PASS: TX/RX={ntx}/{nrx}, {total} IDs accumulated without any RX dequeue/read")
    finally:
        d.close()


def stream(path, ntx, nrx):
    d = Device(path, ntx, nrx)
    try:
        assert not d.poll.poll(0), "CPU-owned initial buffers reported as completed"
        d.start()
        expect_errno(errno.EBUSY, d.start)
        expect_errno(errno.ENOTTY, lambda: fcntl.ioctl(d.fd, BASE | 7))
        d.stop()
        # TX must wait for receive capacity. Stop cancels queued work.
        d.queue(0, 0, 1)
        d.start()
        d.poll.modify(d.fd, select.POLLOUT)
        assert not d.poll.poll(20), "TX ran without RX"
        d.stop()

        # Merge two inputs into a full output + tail, without END_INPUT.
        # Works even with a single TX/RX buffer by reusing completions.
        d.maps[0][0][:9000 * 64] = SOLVED * 9000
        d.queue(0, 0, 9000)
        if ntx > 1:
            d.maps[0][1][:9000 * 64] = SOLVED * 9000
            d.queue(0, 1, 9000)
        for i in range(nrx):
            d.queue(1, i)
        d.start()
        if ntx == 1:
            while d.dequeue(0) is None:
                d.wait(select.POLLOUT)
            # The first idle tail may already have been emitted.
            d.queue(0, 0, 9000)
        sizes = []
        while sum(sizes) < 18000:
            d.wait(select.POLLIN)
            while (done := d.dequeue(1)) is not None:
                i, n = done
                sizes.append(n)
                assert d.maps[1][i][:n * 64] == SOLVED * n
                d.queue(1, i)
        if ntx > 1:
            assert sizes == [16384, 1616], sizes
        while d.dequeue(0) is not None:
            pass
        d.maps[0][0][:3 * 64] = SOLVED * 3
        d.queue(0, 0, 3)
        d.wait(select.POLLIN)
        _, n = d.dequeue(1)
        assert n == 3
        d.stop()

        # Enough work to fill every RX and wrap both queues/rings repeatedly.
        lengths = [1, 7, 10000, RECORDS, 31, 3] * max(16, nrx * 2) + [19]
        total = sum(lengths)
        seen = bytearray(total)
        submitted = received = next_packet = 0
        input_done = False
        held = []
        released = False

        def submit(i):
            nonlocal submitted, next_packet, input_done
            if next_packet == len(lengths):
                return
            n = lengths[next_packet]
            fill(d.maps[0][i], submitted, n)
            d.queue(0, i, n)
            expect_errno(errno.EBUSY, lambda: d.queue(0, i, n))
            submitted += n
            next_packet += 1
            input_done = next_packet == len(lengths)

        for i in range(nrx):
            d.maps[1][i][:] = b"\xa5" * STRIDE
            d.queue(1, i)
        for i in range(ntx):
            submit(i)
        d.start()
        while received != total:
            d.wait(select.POLLIN | (0 if input_done else select.POLLOUT))
            while (done := d.dequeue(1)) is not None:
                i, n = done
                assert 0 < n <= RECORDS
                verify(d.maps[1][i], n, seen)
                received += n
                if not released:
                    # Hold every RX; ensure completed data remains unchanged
                    # while TX eventually stalls, then return capacity.
                    held.append((i, n, bytes(d.maps[1][i][:n * 64])))
                    if len(held) == nrx:
                        time.sleep(0.02)
                        before = d.stats()[0]
                        time.sleep(0.02)
                        assert d.stats()[0] == before, "RX starvation did not stall TX"
                        for index, count, snapshot in held:
                            assert d.maps[1][index][:count * 64] == snapshot
                            d.maps[1][index][:] = b"\xa5" * STRIDE
                            d.queue(1, index)
                        held.clear()
                        released = True
                else:
                    d.maps[1][i][:] = b"\xa5" * STRIDE
                    d.queue(1, i)
            if not input_done:
                while (done := d.dequeue(0)) is not None:
                    submit(done[0])
                    if input_done:
                        break
        assert released and all(seen)
        totals(d, total)
        d.stop()
        # Reset active work/unread output; prove no stale results on restart.
        d.queue(1, 0)
        fill(d.maps[0][0], 0, 10000)
        d.queue(0, 0, 10000)
        d.start()
        d.stop()
        d.queue(1, 0)
        fill(d.maps[0][0], 0, 1)
        d.queue(0, 0, 1)
        d.start()
        d.wait(select.POLLIN)
        i, n = d.dequeue(1)
        assert n == 1
        verify(d.maps[1][i], n, bytearray(1))
        totals(d, 1)
        print(f"PASS: TX/RX={ntx}/{nrx}, {total} mixed IDs, idle tails/resume, SG wrap, held RX stability, backpressure, dirty reuse, restart")
    finally:
        d.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", nargs="?", default="/dev/fades")
    parser.add_argument("--tx", type=int, default=8)
    parser.add_argument("--rx", type=int, default=8)
    args = parser.parse_args()
    accumulate(args.device, args.tx, args.rx)
    stream(args.device, args.tx, args.rx)


if __name__ == "__main__":
    main()
