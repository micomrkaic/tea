#define _POSIX_C_SOURCE 200809L
/* tea — tiny econometric assistant
 * Copyright (C) 2026 Mico Mrkaic
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Native replacement for system(3).  Besides preserving ordinary shell
 * command semantics, it verifies that a successful ssconvert invocation
 * actually produced the requested nonempty output file.  Some Gnumeric
 * versions append a sheet name to the requested CSV pathname; when the
 * private tea conversion directory contains exactly one such output, move
 * it to the pathname expected by import excel.
 */
#ifndef __EMSCRIPTEN__
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int trace_enabled(void)
{
    const char *value = getenv("TEA_SUBPROCESS_TRACE");
    return value && value[0] && strcmp(value, "0") != 0;
}

static int successful_status(int status)
{
    return status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int extract_last_single_quoted(const char *command, char *out,
                                      size_t out_size)
{
    const char *start = NULL;
    const char *end = NULL;
    const char *p = command;

    while ((p = strchr(p, '\'')) != NULL) {
        const char *q = strchr(p + 1, '\'');
        if (!q) break;
        start = p + 1;
        end = q;
        p = q + 1;
    }
    if (!start || !end || end <= start) return 0;

    size_t len = (size_t)(end - start);
    if (len >= out_size) return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

static int nonempty_regular_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int normalize_ssconvert_output(const char *command)
{
    char expected[PATH_MAX];
    if (!extract_last_single_quoted(command, expected, sizeof expected)) return 0;
    if (nonempty_regular_file(expected)) return 1;

    char directory[PATH_MAX];
    char basename_buf[PATH_MAX];
    const char *slash = strrchr(expected, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - expected);
        if (dir_len == 0) dir_len = 1;
        if (dir_len >= sizeof directory) return 0;
        memcpy(directory, expected, dir_len);
        directory[dir_len] = '\0';
        snprintf(basename_buf, sizeof basename_buf, "%s", slash + 1);
    } else {
        snprintf(directory, sizeof directory, ".");
        snprintf(basename_buf, sizeof basename_buf, "%s", expected);
    }

    DIR *dir = opendir(directory);
    if (!dir) return 0;

    char candidate[PATH_MAX] = "";
    int candidates = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!strcmp(entry->d_name, basename_buf)) continue;

        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        if (!nonempty_regular_file(path)) continue;

        candidates++;
        if (candidates == 1) snprintf(candidate, sizeof candidate, "%s", path);
    }
    closedir(dir);

    if (candidates != 1) return 0;
    if (rename(candidate, expected) != 0) return 0;

    if (trace_enabled())
        fprintf(stderr, "tea: normalized ssconvert output %s -> %s\n",
                candidate, expected);
    return nonempty_regular_file(expected);
}

int tea_system(const char *command)
{
    if (!command) return access("/bin/sh", X_OK) == 0;

    if (trace_enabled()) fprintf(stderr, "tea: system: %s\n", command);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;

    if (trace_enabled()) {
        if (WIFEXITED(status))
            fprintf(stderr, "tea: system exit status: %d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "tea: system signal: %d\n", WTERMSIG(status));
    }

    const char *p = command;
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "ssconvert ", 10) && successful_status(status)) {
        if (!normalize_ssconvert_output(command)) {
            if (trace_enabled())
                fprintf(stderr,
                        "tea: ssconvert exited successfully but produced no "
                        "unique nonempty output\n");
            return 1 << 8;
        }
    }
    return status;
}
#endif
