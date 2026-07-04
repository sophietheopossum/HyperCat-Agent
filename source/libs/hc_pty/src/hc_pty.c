/* hc_pty.c — POSIX pseudo-terminal child spawn. See hc_pty.h for the contract + the security posture.
 *
 * The load-bearing security step is the SCRUBBED env in the child (build_child_env + execve with it):
 * the parent's environment — which holds OPENROUTER_API_KEY — is NOT inherited, so the spawned shell
 * cannot read the key. The child also gets a fresh session (setsid) with the pty slave as its
 * controlling tty + stdio, and runs at the same uid (no escalation). */

#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1

#include "hc_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct hc_pty {
    int   master; /* the pty master fd (non-blocking); the host reads/writes it */
    pid_t pid;    /* the child shell pid                                        */
    bool  dead;   /* set once the child has been reaped                         */
};

/* Open a pty master + unlock its slave; copies the slave path into `slave_out`. -1 on failure. */
static int open_master(char *slave_out, size_t cap)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    /* CLOEXEC on the master is load-bearing, not hygiene: the host posix_spawns agent workers (incl.
     * RESPAWNS after this pty exists), and without CLOEXEC a worker would inherit the operator terminal's
     * master fd — an agent gaining read/write on the human's shell. The child shell also closes it. */
    fcntl(master, F_SETFD, FD_CLOEXEC);
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    const char *name = ptsname(master); /* static storage — copy before any other ptsname call */
    if (!name || (size_t)snprintf(slave_out, cap, "%s", name) >= cap) {
        close(master);
        return -1;
    }
    return master;
}

hc_pty *hc_pty_spawn(const char *const argv[], const char *cwd)
{
    if (!argv || !argv[0]) return NULL;
    char slave_path[256];
    int  master = open_master(slave_path, sizeof slave_path);
    if (master < 0) return NULL;

    /* Build the SCRUBBED child env in the PARENT, BEFORE fork: getenv + snprintf are NOT
     * async-signal-safe, so they must not run between fork() and execve() in the child. ONLY these
     * safe vars carry over — the parent's env (which holds OPENROUTER_API_KEY) is dropped, so the
     * shell cannot read the key or any other inherited secret. The child inherits a copy of env[]. */
    const char *home = getenv("HOME");
    const char *path = getenv("PATH");
    char        homebuf[1024], pathbuf[4096];
    snprintf(homebuf, sizeof homebuf, "HOME=%s", (home && home[0]) ? home : "/");
    snprintf(pathbuf, sizeof pathbuf, "PATH=%s", (path && path[0]) ? path : "/usr/bin:/bin");
    char *const env[] = {homebuf, pathbuf, (char *)"TERM=xterm", (char *)"PS1=hypercat$ ", NULL};

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return NULL;
    }
    if (pid == 0) {
        /* --- child: ONLY async-signal-safe calls from here (env is already built) --- */
        close(master);
        if (setsid() < 0) _exit(127); /* new session so the slave can become the controlling tty */
        int slave = open(slave_path, O_RDWR); /* no O_NOCTTY -> becomes our controlling tty */
        if (slave < 0) _exit(127);
        if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0)
            _exit(127); /* never run blind — a failed stdio dup is fatal, not a half-alive shell */
        if (slave > STDERR_FILENO) close(slave);
        if (cwd && cwd[0]) {
            if (chdir(cwd) != 0) { /* a bad cwd is non-fatal (cwd is cosmetic, not a jail) */
            }
        }
        execve(argv[0], (char *const *)argv, env);
        _exit(127); /* exec failed */
    }

    /* --- parent --- */
    int fl = fcntl(master, F_GETFL, 0);
    if (fl >= 0) fcntl(master, F_SETFL, fl | O_NONBLOCK);
    hc_pty *p = calloc(1, sizeof *p);
    if (!p) {
        close(master);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    p->master = master;
    p->pid = pid;
    return p;
}

long hc_pty_read(hc_pty *p, char *buf, size_t cap)
{
    if (!p || p->master < 0 || !buf || cap == 0) return -1;
    ssize_t n = read(p->master, buf, cap);
    if (n > 0) return (long)n;
    if (n == 0) return -1;                                  /* EOF — the child closed the pty */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* nothing available yet            */
    if (errno == EINTR) return 0;                           /* transient                        */
    return -1; /* a real error (Linux returns EIO on the master once the child exits) */
}

long hc_pty_write(hc_pty *p, const char *data, size_t len)
{
    if (!p || p->master < 0 || !data) return -1;
    if (len == 0) return 0;
    ssize_t n = write(p->master, data, len);
    if (n >= 0) return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

bool hc_pty_alive(hc_pty *p)
{
    if (!p) return false;
    if (p->dead) return false;
    int   status = 0;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == 0) return true;     /* still running */
    p->dead = true;              /* reaped (r == pid) or gone (r < 0) */
    return false;
}

void hc_pty_close(hc_pty *p)
{
    if (!p) return;
    if (!p->dead && p->pid > 0) {
        kill(p->pid, SIGHUP); /* ask the shell to exit (its controlling tty hung up) */
        for (int i = 0; i < 50; i++) {
            if (waitpid(p->pid, NULL, WNOHANG) != 0) {
                p->dead = true;
                break;
            }
            struct timespec ts = {0, 2 * 1000 * 1000}; /* 2 ms */
            nanosleep(&ts, NULL);
        }
        if (!p->dead) { /* still here — force it */
            kill(p->pid, SIGKILL);
            waitpid(p->pid, NULL, 0);
        }
    }
    if (p->master >= 0) close(p->master);
    free(p);
}
