#include "ss.h"
#include "backend.h"
#include <errno.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <net/if.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

static void probe(const char *path) {
    struct passwd *account = getpwnam(SS_BACKEND_USER);
    g_assert_nonnull(account);
    g_assert_cmpuint(getuid(), ==, account->pw_uid);
    g_assert_cmpuint(geteuid(), ==, account->pw_uid);
    g_assert_cmpuint(getgid(), ==, account->pw_gid);
    g_assert_cmpuint(getegid(), ==, account->pw_gid);
    g_assert_cmpint(getgroups(0, NULL), ==, 0);
    g_autofree char *status = NULL;
    g_assert_true(g_file_get_contents("/proc/self/status", &status, NULL, NULL));
    const char *fields[] = {"CapInh:", "CapPrm:", "CapEff:", "CapAmb:"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        const char *value = strstr(status, fields[i]);
        g_assert_nonnull(value);
        g_assert_cmpuint(g_ascii_strtoull(value + strlen(fields[i]), NULL, 16), ==, 0x1000);
    }
    const char *bounding = strstr(status, "CapBnd:");
    g_assert_nonnull(bounding);
    guint64 ceiling = g_ascii_strtoull(bounding + strlen("CapBnd:"), NULL, 16);
    g_assert_cmpuint(ceiling & ((1ULL << CAP_CHOWN) | (1ULL << CAP_SETPCAP)), ==, 0);
    g_assert_nonnull(strstr(status, "NoNewPrivs:\t1"));
    g_assert_cmpint(setuid(0), ==, -1);
    g_assert_cmpint(errno, ==, EPERM);
    g_autofree char *secret = NULL;
    g_assert_true(g_file_get_contents(path, &secret, NULL, NULL));
    g_assert_cmpstr(secret, ==, "test configuration");
    g_assert_cmpint(open(path, O_WRONLY), ==, -1);
    g_assert_cmpint(errno, ==, EACCES);
    struct stat st;
    g_assert_cmpint(stat(path, &st), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0640);
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    g_assert_cmpint(sock, >=, 0);
    unsigned int mark = 0x5353;
    g_assert_cmpint(setsockopt(sock, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)), ==, 0);
    close(sock);
}
int main(int argc, char **argv) {
    if (argc == 3) {
        probe(argv[2]);
        if (!strcmp(argv[1], "-c")) {
            /* A SIP003-style descendant must inherit the same non-root identity. */
            g_autoptr(GError) error = NULL;
            g_autoptr(GSubprocess) child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
                                                            argv[0], "--plugin", argv[2], NULL);
            g_assert_no_error(error);
            g_assert_true(g_subprocess_wait_check(child, NULL, &error));
            g_assert_no_error(error);
        }
        return 0;
    }
    if (geteuid() != 0) {
        g_print("Requires root (or a user namespace mapping the service account).\n");
        return 77;
    }
    if (!g_getenv("SS_TEST_NM_CAPS")) {
        g_assert_cmpint(prctl(PR_CAPBSET_DROP, CAP_CHOWN, 0, 0, 0), ==, 0);
        g_assert_cmpint(prctl(PR_CAPBSET_DROP, CAP_SETPCAP, 0, 0, 0), ==, 0);
        g_setenv("SS_TEST_NM_CAPS", "1", TRUE);
        execv("/proc/self/exe", argv);
        g_error("Cannot re-exec restricted privilege test");
    }
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("nmss-privileges-XXXXXX", &error);
    g_assert_no_error(error);
    g_autofree char *path = g_build_filename(dir, "backend.json", NULL);
    g_assert_true(g_file_set_contents(path, "test configuration", -1, &error));
    g_assert_no_error(error);
    g_autofree char *executable = g_file_read_link("/proc/self/exe", &error);
    g_assert_no_error(error);
    g_autoptr(GSubprocess) child = ss_backend_spawn(executable, path, &error);
    g_assert_no_error(error);
    gboolean passed = g_subprocess_wait_check(child, NULL, &error);
    if (passed && argc == 2 && !strcmp(argv[1], "--tun")) {
        /* Explicit opt-in: run only inside a private network namespace. */
        g_autoptr(SsProfile) profile =
            ss_profile_parse("{\"ip_mode\":\"ipv4\",\"dns\":[\"1.1.1.1\"],\"tcp\":{"
                             "\"server\":\"127.0.0.1\",\"server_port\":8388,"
                             "\"method\":\"aes-256-gcm\",\"password\":\"test\"}}",
                             &error);
        g_assert_no_error(error);
        g_autofree char *config = ss_backend_json(profile);
        g_assert_true(g_file_set_contents(path, config, -1, &error));
        g_assert_no_error(error);
        g_autoptr(GSubprocess) backend = ss_backend_spawn(SS_LOCAL, path, &error);
        g_assert_no_error(error);
        gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        while (!if_nametoindex("nmss-tun") && g_get_monotonic_time() < deadline)
            g_usleep(50000);
        gboolean tun_ready = if_nametoindex("nmss-tun") != 0;
        kill(-(pid_t)g_ascii_strtoll(g_subprocess_get_identifier(backend), NULL, 10), SIGKILL);
        g_subprocess_wait(backend, NULL, NULL);
        unlink(path);
        rmdir(dir);
        g_assert_true(tun_ready);
    }
    unlink(path);
    rmdir(dir);
    g_assert_no_error(error);
    g_assert_true(passed);
    return 0;
}
