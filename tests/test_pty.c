#define _XOPEN_SOURCE 700
/* Exercise a genuine interactive Bash on a PTY, with the same emulator
 * used by scos-terminal.  No X server is needed for this integration test. */
#include "term_emulation.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
static char transcript[65536];
static size_t transcript_len;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "test_pty: FAIL: %s\n", what);
    }
}

static long long milliseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int contains_from(size_t start, const char *needle)
{
    size_t n = strlen(needle);
    if (!n || start > transcript_len || n > transcript_len) return 0;
    for (size_t i = start; i + n <= transcript_len; i++)
        if (!memcmp(transcript + i, needle, n)) return 1;
    return 0;
}

static int wait_for(term_emulator *em, int fd, size_t start,
                    const char *needle, int timeout_ms)
{
    long long deadline = milliseconds() + timeout_ms;
    while (milliseconds() < deadline) {
        if (contains_from(start, needle)) return 1;
        int remaining = (int)(deadline - milliseconds());
        if (remaining < 0) remaining = 0;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ready = poll(&pfd, 1, remaining > 100 ? 100 : remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        unsigned char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            term_feed(em, buf, (size_t)n);
            size_t room = sizeof(transcript) - 1 - transcript_len;
            size_t copy = (size_t)n < room ? (size_t)n : room;
            memcpy(transcript + transcript_len, buf, copy);
            transcript_len += copy;
            transcript[transcript_len] = 0;
        } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN &&
                              errno != EWOULDBLOCK)) {
            break;
        }
    }
    return contains_from(start, needle);
}

static void send_command(int fd, const char *command)
{
    size_t len = strlen(command);
    for (size_t done = 0; done < len;) {
        ssize_t n = write(fd, command + done, len - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else {
            check(0, "write command to Bash PTY");
            return;
        }
    }
}

static int screen_has_colored_text(const term_emulator *em, const char *text,
                                   uint32_t color)
{
    size_t len = strlen(text);
    for (int row = 0; row < em->screen->rows; row++) {
        const term_line *line = &em->screen->lines[row];
        for (int col = 0; col + (int)len <= line->len; col++) {
            int match = 1;
            for (size_t i = 0; i < len; i++)
                if (line->cells[col + (int)i].ch != (unsigned char)text[i] ||
                    line->cells[col + (int)i].fg != color) {
                    match = 0;
                    break;
                }
            if (match) return 1;
        }
    }
    return 0;
}

int main(void)
{
    if (access("/bin/bash", X_OK) != 0) {
        puts("test_pty: SKIP: /bin/bash is not installed");
        return 0;
    }

    term_emulator em;
    term_init(&em, 80, 24, 0x39ff14, 0x031403);
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 80;
    ws.ws_row = 24;
    int fd = -1;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        term_free(&em);
        return 1;
    }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "scos-pty$ ", 1);
        setenv("PROMPT_COMMAND", "", 1);
        char *const args[] = { (char *)"/bin/bash", (char *)"--noprofile",
                               (char *)"--norc", (char *)"-i", NULL };
        execv(args[0], args);
        _exit(127);
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    check(wait_for(&em, fd, 0, "scos-pty$ ", 5000), "interactive Bash prompt");

    size_t start = transcript_len;
    send_command(fd, "printf '__SCOS_%d__\\n' \"$((6 * 7))\"\r");
    check(wait_for(&em, fd, start, "__SCOS_42__", 5000),
          "Bash executes arithmetic and printf through the PTY");

    start = transcript_len;
    send_command(fd, "stty size\r");
    check(wait_for(&em, fd, start, "24 80\r\n", 5000),
          "Bash sees the initial 80x24 terminal size");

    ws.ws_col = 100;
    ws.ws_row = 30;
    check(ioctl(fd, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ resize succeeds");
    term_resize(&em, 100, 30);
    start = transcript_len;
    send_command(fd, "stty size\r");
    check(wait_for(&em, fd, start, "30 100\r\n", 5000),
          "Bash sees the resized 100x30 terminal");

    start = transcript_len;
    send_command(fd, "printf '\\033[31mSCOS_COLOR\\033[0m\\n'\r");
    check(wait_for(&em, fd, start, "\033[31mSCOS_COLOR", 5000),
          "Bash emits ANSI color output");
    check(screen_has_colored_text(&em, "SCOS_COLOR", 0xcc0000),
          "PTY output reaches the emulator with its color intact");

    send_command(fd, "exit 0\r");
    int status = 0, exited = 0;
    long long deadline = milliseconds() + 5000;
    while (milliseconds() < deadline) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) { exited = 1; break; }
        if (result < 0 && errno != EINTR) break;
        struct timespec pause = { 0, 10000000 };
        nanosleep(&pause, NULL);
    }
    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "interactive Bash exits cleanly");

    if (!exited) {
        kill(pid, SIGHUP);
        waitpid(pid, &status, 0);
    }
    close(fd);
    term_free(&em);
    printf("test_pty: %d checks, %d failures\n", checks, failures);
    if (failures) {
        fprintf(stderr, "test_pty transcript:\n%s\n", transcript);
        return 1;
    }
    puts("test_pty: all checks passed");
    return 0;
}
