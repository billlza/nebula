from __future__ import annotations

import errno
import fcntl
import os
import stat
import sys


ERROR_MESSAGE_MAX_BYTES = 4096
ANCHOR_FDS_ENV = "_NEBULA_POSIX_CONTAINMENT_ANCHOR_FDS"
ANCHOR_STACK_MAX_DEPTH = 64
ANCHOR_STACK_MAX_BYTES = 8192
READY_STATUS = b"READY\n"
ERROR_STATUS_PREFIX = b"ERROR "


def _write_all(fd: int, payload: bytes) -> None:
    view = memoryview(payload)
    while view:
        try:
            written = os.write(fd, view)
        except InterruptedError:
            continue
        if written <= 0:
            raise OSError(errno.EIO, os.strerror(errno.EIO))
        view = view[written:]


def _write_error(error_fd: int, message: str) -> None:
    payload = ERROR_STATUS_PREFIX + (message.rstrip("\n") + "\n").encode(
        "utf-8",
        errors="replace",
    )[: ERROR_MESSAGE_MAX_BYTES - len(ERROR_STATUS_PREFIX)]
    try:
        _write_all(error_fd, payload)
    except OSError as exc:
        fallback = (
            f"POSIX exec gate could not report failure on fd {error_fd}: {exc}\n"
        ).encode("utf-8", errors="replace")[:ERROR_MESSAGE_MAX_BYTES]
        try:
            _write_all(sys.stderr.fileno(), fallback)
        except OSError:
            return


def main() -> int:
    if len(sys.argv) < 5:
        return 125
    try:
        gate_fd = int(sys.argv[1])
        error_fd = int(sys.argv[2])
        new_anchor_fd = int(sys.argv[3])
    except ValueError:
        return 125
    target_argv = sys.argv[4:]
    if gate_fd < 0 or error_fd < 0 or new_anchor_fd < 3 or not target_argv:
        _write_error(error_fd, "POSIX exec gate received invalid launch arguments")
        return 125

    raw_anchor_stack = os.environ.get(ANCHOR_FDS_ENV, "")
    try:
        encoded_anchor_stack = raw_anchor_stack.encode("ascii")
    except UnicodeEncodeError:
        _write_error(error_fd, "POSIX exec gate anchor stack is not ASCII")
        return 125
    raw_anchor_fds = tuple(raw_anchor_stack.split(":")) if raw_anchor_stack else ()
    if (
        not raw_anchor_fds
        or len(encoded_anchor_stack) > ANCHOR_STACK_MAX_BYTES
        or len(raw_anchor_fds) > ANCHOR_STACK_MAX_DEPTH
    ):
        _write_error(error_fd, "POSIX exec gate anchor stack exceeded its bounds")
        return 125
    anchor_fds: list[int] = []
    for raw_anchor_fd in raw_anchor_fds:
        if (
            not raw_anchor_fd.isascii()
            or not raw_anchor_fd.isdecimal()
            or raw_anchor_fd != str(int(raw_anchor_fd))
        ):
            _write_error(error_fd, "POSIX exec gate anchor stack is malformed")
            return 125
        anchor_fd = int(raw_anchor_fd)
        if anchor_fd < 3 or anchor_fd in anchor_fds:
            _write_error(error_fd, "POSIX exec gate anchor descriptor is unsafe")
            return 125
        try:
            descriptor_stat = os.fstat(anchor_fd)
            descriptor_flags = fcntl.fcntl(anchor_fd, fcntl.F_GETFL)
        except OSError as exc:
            _write_error(
                error_fd,
                f"POSIX exec gate anchor descriptor is not open: {exc}",
            )
            return 125
        if (
            not stat.S_ISFIFO(descriptor_stat.st_mode)
            or (descriptor_flags & os.O_ACCMODE) != os.O_WRONLY
        ):
            _write_error(error_fd, "POSIX exec gate anchor descriptor is not writable pipe")
            return 125
        try:
            os.set_inheritable(anchor_fd, True)
        except OSError as exc:
            _write_error(
                error_fd,
                f"POSIX exec gate could not preserve anchor descriptor: {exc}",
            )
            return 125
        anchor_fds.append(anchor_fd)
    if anchor_fds[-1] != new_anchor_fd:
        _write_error(error_fd, "POSIX exec gate new anchor is not stack tail")
        return 125

    # The error channel remains writable if execvpe fails, but closes atomically
    # on successful exec so the parent can distinguish setup failure from target
    # process exit without interpreting the target's return code.
    try:
        os.set_inheritable(error_fd, False)
    except OSError as exc:
        _write_error(
            error_fd,
            f"POSIX exec gate could not protect its status descriptor: {exc}",
        )
        return 125
    try:
        release = os.read(gate_fd, 1)
    except OSError as exc:
        _write_error(
            error_fd,
            f"POSIX exec gate read failed: [{exc.errno}] {exc.strerror}",
        )
        try:
            os.close(gate_fd)
        except OSError as close_exc:
            _write_error(
                error_fd,
                f"POSIX exec gate release descriptor cleanup failed: {close_exc}",
            )
        return 125
    try:
        os.close(gate_fd)
    except OSError as exc:
        _write_error(
            error_fd,
            f"POSIX exec gate could not close its release descriptor: {exc}",
        )
        return 125
    if release != b"\x01":
        _write_error(error_fd, "POSIX exec gate closed without a valid release")
        return 125

    try:
        _write_all(error_fd, READY_STATUS)
    except OSError as exc:
        _write_error(
            error_fd,
            f"POSIX exec gate readiness handshake failed: {exc}",
        )
        return 125
    try:
        os.execvpe(target_argv[0], target_argv, os.environ)
    except OSError as exc:
        error_number = exc.errno if exc.errno is not None else errno.EIO
        _write_error(
            error_fd,
            f"POSIX target exec failed: [{error_number}] {os.strerror(error_number)}",
        )
        return 125
    raise AssertionError("os.execvpe returned unexpectedly")


if __name__ == "__main__":
    raise SystemExit(main())
