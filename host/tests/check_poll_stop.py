#!/usr/bin/env python3
"""Verify direction-specific poll waiters wake on STREAMOFF and restart works."""
import select
import sys
import threading
import time

from fades_device import Device


def poll_once(fd, events, timeout):
    poller = select.poll()
    poller.register(fd, events)
    return poller.poll(timeout)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/dev/fades"
    device = Device(path, 1, 1)
    try:
        # No HUP on a fresh open; even a pre-start STREAMOFF is explicit stop.
        assert not poll_once(device.fd, select.POLLIN | select.POLLOUT, 0)
        device.stop()
        assert poll_once(device.fd, select.POLLIN, 0)[0][1] & select.POLLHUP
        device.start()
        assert not poll_once(device.fd, select.POLLIN | select.POLLOUT, 0)

        ready = threading.Barrier(3)
        results = {}

        def wait_for(direction, events):
            poller = select.poll()
            poller.register(device.fd, events)
            ready.wait()
            results[direction] = poller.poll(3000)

        workers = [threading.Thread(target=wait_for, args=(direction, events))
                   for direction, events in (("tx", select.POLLOUT),
                                             ("rx", select.POLLIN))]
        for worker in workers:
            worker.start()
        ready.wait(timeout=3)
        time.sleep(0.05)
        assert all(worker.is_alive() for worker in workers), "poll returned before stop"
        device.stop()
        for worker in workers:
            worker.join(timeout=3)
            assert not worker.is_alive(), "poll did not wake on STREAMOFF"
        for direction in ("tx", "rx"):
            assert results[direction] and results[direction][0][1] & select.POLLHUP, results
            assert not results[direction][0][1] & select.POLLERR, results

        # HUP is level-triggered, independent of requested events, until restart.
        for events in (select.POLLIN, select.POLLOUT, select.POLLIN | select.POLLOUT):
            for _ in range(2):
                assert poll_once(device.fd, events, 0)[0][1] & select.POLLHUP
        device.start()
        assert not poll_once(device.fd, select.POLLIN | select.POLLOUT, 20)
        device.stop()
        assert poll_once(device.fd, select.POLLIN, 0)[0][1] & select.POLLHUP
    finally:
        device.close()

    # A new open starts without the previous session's stop notification.
    device = Device(path, 1, 1)
    try:
        assert not poll_once(device.fd, select.POLLIN | select.POLLOUT, 0)
    finally:
        device.close()
    print("PASS: TX/RX poll waiters wake on STREAMOFF; HUP persists until restart")


if __name__ == "__main__":
    main()
