/*
 * test_protect.c - 보호된 cgroup 판정 단위 테스트
 *
 * 임시 폴더에 가짜 /proc(<pid>/oom_score_adj)와 가짜 cgroup 트리(cgroup.procs)를 만든다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "protect.h"
#include "tg_test.h"

static char root[64];
static char proc_dir[96];
static char cg_dir[96];

static void write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    TG_CHECK(fd >= 0);
    if (fd >= 0) {
        TG_CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
    }
}

/* 가짜 프로세스: /proc/<pid>/oom_score_adj */
static void mk_proc(int pid, int adj)
{
    char p[160];
    char v[16];

    snprintf(p, sizeof(p), "%s/%d", proc_dir, pid);
    mkdir(p, 0700);
    snprintf(p, sizeof(p), "%s/%d/oom_score_adj", proc_dir, pid);
    snprintf(v, sizeof(v), "%d\n", adj);
    write_file(p, v);
}

/* rel 경로(예: "a/b")의 cgroup 폴더를 만들고 cgroup.procs에 text를 쓴다. 폴더는 상위부터 만든다 */
static void mk_cg(const char *rel, const char *procs_text)
{
    char p[256];
    size_t n = strlen(rel);

    for (size_t i = 1; i <= n; i++) {
        if (rel[i] == '/' || rel[i] == '\0') {
            snprintf(p, sizeof(p), "%s/%.*s", cg_dir, (int)i, rel);
            mkdir(p, 0700);
        }
    }
    if (procs_text != NULL) {
        snprintf(p, sizeof(p), "%s/%s/cgroup.procs", cg_dir, rel);
        write_file(p, procs_text);
    }
}

static int check(const char *name)
{
    int pfd = open(proc_dir, O_RDONLY | O_DIRECTORY);
    int dfd = open(cg_dir, O_RDONLY | O_DIRECTORY);
    int r = tg_protect_check(pfd, dfd, name);

    close(pfd);
    close(dfd);
    return r;
}

static void test_basic(void)
{
    mk_proc(100, 0);
    mk_proc(101, -1000);
    mk_proc(102, -999);
    mk_proc(103, 300);

    mk_cg("empty", "");
    TG_CHECK_EQ_U64(check("empty"), TG_PROTECT_NO);

    mk_cg("normal", "100\n103\n");
    TG_CHECK_EQ_U64(check("normal"), TG_PROTECT_NO);

    mk_cg("near", "102\n"); /* -999는 보호 대상이 아니다 */
    TG_CHECK_EQ_U64(check("near"), TG_PROTECT_NO);

    mk_cg("prot", "100\n101\n103\n");
    TG_CHECK_EQ_U64(check("prot"), TG_PROTECT_YES);

    mk_cg("prot_last_no_newline", "100\n101"); /* 마지막 줄에 개행이 없다 */
    TG_CHECK_EQ_U64(check("prot_last_no_newline"), TG_PROTECT_YES);
}

static void test_exited_process_is_ignored(void)
{
    /* 목록에 있지만 /proc에서 이미 사라진 프로세스는 건너뛴다 */
    mk_cg("gone", "999001\n100\n");
    TG_CHECK_EQ_U64(check("gone"), TG_PROTECT_NO);
}

static void test_unreadable_is_unknown(void)
{
    char p[256];

    mk_cg("noprocs", NULL); /* cgroup.procs가 없다 */
    TG_CHECK_EQ_U64(check("noprocs"), TG_PROTECT_UNKNOWN);

    mk_cg("garbage", "100\nabc\n");
    TG_CHECK_EQ_U64(check("garbage"), TG_PROTECT_UNKNOWN);

    /* 프로세스는 있는데 값이 이상하다 */
    mk_proc(200, 0);
    snprintf(p, sizeof(p), "%s/200/oom_score_adj", proc_dir);
    write_file(p, "not-a-number\n");
    mk_cg("badval", "200\n");
    TG_CHECK_EQ_U64(check("badval"), TG_PROTECT_UNKNOWN);

    TG_CHECK_EQ_U64(check("no_such_cgroup"), TG_PROTECT_UNKNOWN);
    TG_CHECK_EQ_U64(tg_protect_check(-1, -1, "x"), TG_PROTECT_UNKNOWN);
}

static void test_nested(void)
{
    /* 하위 cgroup 안의 보호 프로세스도 찾는다. 중간 단계는 비어 있다 */
    mk_cg("pod", "");
    mk_cg("pod/app", "100\n");
    mk_cg("pod/side", "");
    mk_cg("pod/side/helper", "101\n");
    TG_CHECK_EQ_U64(check("pod"), TG_PROTECT_YES);

    mk_cg("pod2", "");
    mk_cg("pod2/app", "100\n");
    mk_cg("pod2/side", "103\n");
    TG_CHECK_EQ_U64(check("pod2"), TG_PROTECT_NO);
}

static void test_depth_limit(void)
{
    char rel[256];
    int pos;

    /* 깊이 상한(8단계)까지는 끝까지 확인한다 */
    pos = snprintf(rel, sizeof(rel), "deep8");
    mk_cg(rel, "");
    for (int d = 1; d <= TG_PROTECT_MAX_DEPTH; d++) {
        pos += snprintf(rel + pos, sizeof(rel) - (size_t)pos, "/l%d", d);
        mk_cg(rel, d == TG_PROTECT_MAX_DEPTH ? "101\n" : "");
    }
    TG_CHECK_EQ_U64(check("deep8"), TG_PROTECT_YES);

    /* 상한 아래에 하위 cgroup이 더 있으면 확인하지 못한 것으로 본다 */
    pos = snprintf(rel, sizeof(rel), "deep9");
    mk_cg(rel, "");
    for (int d = 1; d <= TG_PROTECT_MAX_DEPTH + 1; d++) {
        pos += snprintf(rel + pos, sizeof(rel) - (size_t)pos, "/l%d", d);
        mk_cg(rel, "");
    }
    TG_CHECK_EQ_U64(check("deep9"), TG_PROTECT_UNKNOWN);

    /* 상한 안쪽 어딘가에서 보호 프로세스를 먼저 찾으면 깊이와 무관하게 YES */
    mk_cg("deep9/l1", "101\n");
    TG_CHECK_EQ_U64(check("deep9"), TG_PROTECT_YES);
}

static void test_many_pids_across_chunks(void)
{
    char *text = malloc(1 << 16);
    size_t len = 0;

    TG_CHECK(text != NULL);
    if (text == NULL)
        return;
    /* 읽기 조각(1 KiB)을 여러 번 넘는 목록. 마지막 프로세스만 보호 대상이다 */
    for (int pid = 500000; pid < 500400; pid++)
        len += (size_t)snprintf(text + len, (1u << 16) - len, "%d\n", pid);
    snprintf(text + len, (1u << 16) - len, "101\n");
    mk_cg("many", text);
    TG_CHECK(len > 2048);
    TG_CHECK_EQ_U64(check("many"), TG_PROTECT_YES);

    mk_cg("many_no", "");
    {
        char p[256];

        snprintf(text + len, (1u << 16) - len, "103\n");
        snprintf(p, sizeof(p), "%s/many_no/cgroup.procs", cg_dir);
        write_file(p, text);
    }
    TG_CHECK_EQ_U64(check("many_no"), TG_PROTECT_NO);
    free(text);
}

static void test_mark_targets(void)
{
    struct tg_targets t;
    int pfd = open(proc_dir, O_RDONLY | O_DIRECTORY);

    tg_targets_init(&t);
    snprintf(t.t[0].name, sizeof(t.t[0].name), "%s", "normal");
    snprintf(t.t[1].name, sizeof(t.t[1].name), "%s", "prot");
    snprintf(t.t[2].name, sizeof(t.t[2].name), "%s", "noprocs");
    t.n = 3;
    t.t[0].protect = 1; /* 이전 값이 남아 있어도 다시 계산한다 */

    TG_CHECK(tg_targets_mark(&t, pfd, cg_dir, 12345) == 0);
    TG_CHECK_EQ_U64(t.t[0].protect, TG_PROTECT_NO);
    TG_CHECK_EQ_U64(t.t[1].protect, TG_PROTECT_YES);
    TG_CHECK_EQ_U64(t.t[2].protect, TG_PROTECT_UNKNOWN);
    TG_CHECK_EQ_U64(t.scanned_ns, 12345);

    t.scanned_ns = 7;
    TG_CHECK(tg_targets_mark(&t, pfd, "/nonexistent/tg_protect", 99) == -ENOENT);
    TG_CHECK_EQ_U64(t.scanned_ns, 7); /* 실패하면 갱신 시각도 바꾸지 않는다 */
    close(pfd);
}

int main(void)
{
    char cmd[160];

    snprintf(root, sizeof(root), "/tmp/tg_protect_XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(proc_dir, sizeof(proc_dir), "%s/proc", root);
    snprintf(cg_dir, sizeof(cg_dir), "%s/cg", root);
    TG_CHECK(mkdir(proc_dir, 0700) == 0);
    TG_CHECK(mkdir(cg_dir, 0700) == 0);

    TG_RUN(test_basic);
    TG_RUN(test_exited_process_is_ignored);
    TG_RUN(test_unreadable_is_unknown);
    TG_RUN(test_nested);
    TG_RUN(test_depth_limit);
    TG_RUN(test_many_pids_across_chunks);
    TG_RUN(test_mark_targets);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0)
        fprintf(stderr, "임시 디렉터리 정리 실패: %s\n", root);
    return tg_test_finish();
}
