/* libtty.c — clean-room implementation of the `libtty` helper library the
 * Pharo image's LibTTY package FFI-loads (libtty.dylib / libtty.so).  The
 * stock Pharo VM distribution ships this next to its plugins; our VM stages
 * it next to test_load_image the same way TestLibrary is staged.
 *
 * The entire API is one function (see LibTTY>>ttySpawn:path:argv:envs:):
 *
 *     pid_t tty_spawn(int fdm, const char *path, void *argv, void *envp)
 *
 * Semantics (derived from LibTTYTest's assertions):
 *   - fdm is a pseudo-terminal MASTER the image already opened and prepared
 *     (posix_openpt + grantpt + unlockpt — see LibTTYTest>>openPseudoTerminal).
 *   - Fork a child that starts a new session, opens the pty SLAVE as its
 *     controlling terminal, wires it to stdin/stdout/stderr, and execve()s
 *     path with argv/envp (both NULL-terminated char* arrays).
 *   - The parent returns the child pid (-1 only if fork itself fails: the
 *     image asserts `deny: pid equals: -1` even for doomed spawns).
 *   - EVERY post-fork failure exits the child with status 127: LibTTYTest
 *     test1 passes a plain mkstemp() file descriptor as fdm and asserts the
 *     spawn reports exit status 127 (ptsname() on a non-pty fails only in
 *     the child, never at the tty_spawn call site).
 *
 * POSIX-only: the image's LibTTY>>win32LibraryName is shouldNotImplement and
 * LibTTYTest skips on Windows.
 */

/* glibc guards ptsname()/grantpt()/unlockpt() behind feature macros.  Without
 * the declaration, C implicit-int rules TRUNCATE ptsname()'s returned pointer
 * to 32 bits on x86-64 — the child's open(slave) failed with EFAULT and the
 * parent's read() on a never-opened pty master blocked the whole VM (x86 box,
 * LibTTYTest hang, 2026-07-06).  macOS declares them unconditionally. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__linux__)
#include <termios.h> /* TIOCSCTTY on some libcs */
#endif

/* Child-side failure: report which call failed (LibTTYTest test2 asserts the
 * exact execve message reaches the pty) and exit 127. */
static void tty_child_fail(const char *call) {
    fprintf(stderr, "Error in tty_spawn at %s: %s\n", call, strerror(errno));
    _exit(127);
}

pid_t tty_spawn(int fdm, const char *path, void *argv, void *envp) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid != 0)
        return pid; /* parent: the image waitpid()s and reads the master */

    /* Child.  Any failure from here on must surface as exit status 127. */
    {
        const char *slaveName;
        int fds;

        if (setsid() < 0)
            tty_child_fail("setsid()");
        slaveName = ptsname(fdm);
        if (slaveName == NULL)
            tty_child_fail("ptsname(fdm)"); /* not a pty master (test1) */
        fds = open(slaveName, O_RDWR);
        if (fds < 0)
            tty_child_fail("open(slave_name, O_RDWR)");
#ifdef TIOCSCTTY
        /* Make the slave our controlling terminal.  On BSD/macOS opening a
         * tty after setsid() already acquires it; the explicit ioctl keeps
         * Linux deterministic.  Failure is non-fatal on platforms where the
         * open() was sufficient. */
        (void)ioctl(fds, TIOCSCTTY, 0);
#endif
        if (dup2(fds, STDIN_FILENO) < 0 || dup2(fds, STDOUT_FILENO) < 0 ||
            dup2(fds, STDERR_FILENO) < 0)
            tty_child_fail("dup2(fds, ...)");
        if (fds > STDERR_FILENO)
            close(fds);
        close(fdm);

        /* The VM ignores SIGPIPE process-wide and ignored dispositions
         * survive execve(); restore the default so spawned shells behave
         * like they would under the stock VM. */
        signal(SIGPIPE, SIG_DFL);

        execve(path, (char *const *)argv, (char *const *)envp);
        tty_child_fail("execve(path, argv, envp)");
    }
    return -1; /* unreachable */
}
