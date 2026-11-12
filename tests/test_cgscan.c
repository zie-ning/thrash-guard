/*
 * test_cgscan.c - 감시 대상 cgroup 표 테스트
 *
 * 실제 cgroup 대신 임시 디렉터리에 가짜 cgroup 트리를 만들어 쓴다.
 */
#include "cgscan.h"
#include "fmt.h"
#include "procfs.h"
#include "tg_test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[256];

static void write_file(const char *dir, const char *file, const char *content)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, file);
    f = fopen(path, "w");
    TG_CHECK(f != NULL);
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

static void make_cg(const char *name, int with_memory)
{
    char dir[512];

    snprintf(dir, sizeof(dir), "%s/%s", root, name);
    TG_CHECK(mkdir(dir, 0755) == 0);
    if (with_memory) {
        write_file(dir, "memory.pressure",
                   "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\n"
                   "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
        write_file(dir, "memory.stat", "anon 4096\nfile 8192\n");
        write_file(dir, "memory.events", "low 0\nhigh 0\nmax 3\noom 1\noom_kill 1\n");
    }
}

static void set_desc(unsigned n)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "nr_descendants %u\nnr_dying_descendants 0\n", n);
    write_file(root, "cgroup.stat", buf);
}

static void rm_cg(const char *name)
{
    char path[512];
    static const char *files[] = { "memory.pressure", "memory.stat", "memory.events" };
    size_t i;

    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", root, name, files[i]);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/%s", root, name);
    rmdir(path);
}

static int has_name(const struct tg_cg_set *s, const char *name)
{
    size_t i;

    for (i = 0; i < s->n; i++)
        if (strcmp(s->t[i].name, name) == 0)
            return 1;
    return 0;
}

static void cleanup(void)
{
    char cmd[300];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "임시 디렉터리 정리 실패: %s\n", root);
}

static void test_scan_and_rescan(void)
{
    static struct tg_cg_set s; /* 4 KiB 버퍼를 포함하므로 정적으로 둔다 */
    char buf[1024];
    size_t len = 0;
    struct tg_cg_events ev;

    snprintf(root, sizeof(root), "/tmp/tg-cgscan-XXXXXX");
    TG_CHECK(mkdtemp(root) != NULL);

    set_desc(2);
    make_cg("docker-aaa.scope", 1);
    make_cg("docker-bbb.scope", 1);
    make_cg("nomem.scope", 0);      /* memory 파일이 없다 → 대상 아님 */
    make_cg("bad name", 1);         /* 공백이 있는 이름 → 줄 형식을 깨므로 제외 */
    write_file(root, "regular-file", "x"); /* 디렉터리가 아님 */

    TG_CHECK(tg_cg_set_open(&s, root) == 0);
    TG_CHECK_EQ_U64(s.n, 2);
    TG_CHECK(has_name(&s, "docker-aaa.scope"));
    TG_CHECK(has_name(&s, "docker-bbb.scope"));

    /* 대상의 파일을 실제로 읽을 수 있고 파서가 해석한다. */
    TG_CHECK(tg_read_fd(s.t[0].events_fd, buf, sizeof(buf), &len) == 0);
    TG_CHECK(tg_cg_events_parse(buf, len, &ev, NULL) == 0);
    TG_CHECK_EQ_U64(ev.max, 3);
    TG_CHECK_EQ_U64(ev.oom_kill, 1);

    /* 변화가 없으면 다시 스캔하지 않는다. */
    TG_CHECK(!tg_cg_set_refresh(&s));

    /* 컨테이너가 재시작된 상황: 하나가 사라지고 새 이름이 생기면 nr_descendants가 바뀐다. */
    rm_cg("docker-aaa.scope");
    make_cg("docker-ccc.scope", 1);
    make_cg("docker-ddd.scope", 1);
    set_desc(3);
    TG_CHECK(tg_cg_set_refresh(&s));
    TG_CHECK_EQ_U64(s.n, 3);
    TG_CHECK(!has_name(&s, "docker-aaa.scope"));
    TG_CHECK(has_name(&s, "docker-ccc.scope"));

    /* 읽기 실패를 알리면 개수가 그대로여도 다시 스캔한다. */
    tg_cg_set_mark_stale(&s);
    TG_CHECK(tg_cg_set_refresh(&s));
    TG_CHECK(!tg_cg_set_refresh(&s));

    tg_cg_set_close(&s);
    cleanup();
}

static void test_table_full(void)
{
    static struct tg_cg_set s;
    char name[32];
    int i;

    snprintf(root, sizeof(root), "/tmp/tg-cgscan-XXXXXX");
    TG_CHECK(mkdtemp(root) != NULL);
    set_desc(TG_CG_MAX + 2);
    for (i = 0; i < TG_CG_MAX + 2; i++) {
        snprintf(name, sizeof(name), "c%d.scope", i);
        make_cg(name, 1);
    }
    TG_CHECK(tg_cg_set_open(&s, root) == 0);
    TG_CHECK_EQ_U64(s.n, TG_CG_MAX);
    TG_CHECK_EQ_U64(s.skipped, 2);
    tg_cg_set_close(&s);
    cleanup();
}

static void test_open_missing_parent(void)
{
    static struct tg_cg_set s;

    TG_CHECK(tg_cg_set_open(&s, "/tmp/tg-no-such-dir-xyz") == -ENOENT);
    TG_CHECK(!tg_cg_set_refresh(&s)); /* 비활성이면 아무 일도 하지 않는다 */
    TG_CHECK_EQ_U64(s.n, 0);
    tg_cg_set_close(&s);
}

static void test_events_and_cgstat_parse(void)
{
    struct tg_cg_events ev;
    struct tg_cg_cgstat cs;
    uint64_t found = 0;
    const char *e = "low 0\nhigh 5\nmax 7\noom 2\noom_kill 1\n";
    const char *c = "nr_descendants 32\nnr_subsys_cpu 15\nnr_dying_descendants 8\n";

    TG_CHECK(tg_cg_events_parse(e, strlen(e), &ev, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_cg_events_nfields));
    TG_CHECK_EQ_U64(ev.high, 5);
    TG_CHECK_EQ_U64(ev.max, 7);
    TG_CHECK_EQ_U64(ev.oom, 2);
    TG_CHECK_EQ_U64(ev.oom_kill, 1);

    TG_CHECK(tg_cg_cgstat_parse(c, strlen(c), &cs, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_cg_cgstat_nfields));
    TG_CHECK_EQ_U64(cs.nr_descendants, 32);
    TG_CHECK_EQ_U64(cs.nr_dying_descendants, 8);
}

static void test_kv_str(void)
{
    char mem[64];
    struct tg_buf b;

    tg_buf_init(&b, mem, sizeof(mem));
    tg_buf_str(&b, "tg1");
    tg_buf_kv_str(&b, "name", "docker-aaa.scope");
    tg_buf_kv_u64(&b, "idx", 1);
    TG_CHECK(strcmp(mem, "tg1 name=docker-aaa.scope idx=1") == 0);
}

int main(void)
{
    TG_RUN(test_events_and_cgstat_parse);
    TG_RUN(test_kv_str);
    TG_RUN(test_open_missing_parent);
    TG_RUN(test_scan_and_rescan);
    TG_RUN(test_table_full);
    return tg_test_finish();
}
