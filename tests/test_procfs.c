/*
 * test_procfs.c - procfs 파서 단위 테스트
 *
 * fixture는 실제 커널에서 캡처한 파일이다(tests/fixtures/<환경>/).
 * 경계 조건은 문자열을 직접 만들어 검사한다.
 */
#include "procfs.h"
#include "tg_test.h"

#include <errno.h>
#include <string.h>

#define FX(env, name) TG_FIXTURE_DIR "/" env "/" name

static char buf[64 * 1024];

static size_t load(const char *path)
{
    size_t len = 0;
    int rc = tg_read_path(path, buf, sizeof(buf), &len);
    if (rc != 0) {
        fprintf(stderr, "  fixture 읽기 실패 %s: %d\n", path, rc);
        return 0;
    }
    return len;
}

/* ---------- PSI ---------- */

static void test_psi_values(void)
{
    const char *s = "some avg10=1.23 avg60=45.60 avg300=100.00 total=123456789\n"
                    "full avg10=0.05 avg60=0.00 avg300=7.5 total=42\n";
    struct tg_psi psi;

    TG_CHECK(tg_psi_parse(s, strlen(s), &psi) == 0);
    TG_CHECK_EQ_U64(psi.some.avg10, 123);
    TG_CHECK_EQ_U64(psi.some.avg60, 4560);
    TG_CHECK_EQ_U64(psi.some.avg300, 10000);
    TG_CHECK_EQ_U64(psi.some.total_us, 123456789);
    TG_CHECK(psi.has_full);
    TG_CHECK_EQ_U64(psi.full.avg10, 5);
    TG_CHECK_EQ_U64(psi.full.avg300, 750); /* 소수 한 자리 */
    TG_CHECK_EQ_U64(psi.full.total_us, 42);
}

static void test_psi_without_full(void)
{
    /* full 줄이 없는 형식(오래된 커널의 일부 자원)도 받아들인다. */
    const char *s = "some avg10=0.00 avg60=0.00 avg300=0.00 total=0";
    struct tg_psi psi;

    TG_CHECK(tg_psi_parse(s, strlen(s), &psi) == 0);
    TG_CHECK(!psi.has_full);
}

static void test_psi_malformed(void)
{
    struct tg_psi psi;
    const char *missing_total = "some avg10=0.00 avg60=0.00 avg300=0.00\n";
    const char *bad_prefix = "sone avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
    const char *bad_frac = "some avg10=0.001 avg60=0.00 avg300=0.00 total=0\n";
    const char *only_full = "full avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
    const char *garbage = "some avg10=1x avg60=0.00 avg300=0.00 total=0\n";

    TG_CHECK(tg_psi_parse("", 0, &psi) == -EINVAL);
    TG_CHECK(tg_psi_parse(missing_total, strlen(missing_total), &psi) == -EINVAL);
    TG_CHECK(tg_psi_parse(bad_prefix, strlen(bad_prefix), &psi) == -EINVAL);
    TG_CHECK(tg_psi_parse(bad_frac, strlen(bad_frac), &psi) == -EINVAL);
    TG_CHECK(tg_psi_parse(only_full, strlen(only_full), &psi) == -EINVAL);
    TG_CHECK(tg_psi_parse(garbage, strlen(garbage), &psi) == -EINVAL);
}

static void test_psi_overflow(void)
{
    const char *s = "some avg10=0.00 avg60=0.00 avg300=0.00 total=18446744073709551616\n";
    struct tg_psi psi;

    TG_CHECK(tg_psi_parse(s, strlen(s), &psi) == -EOVERFLOW);
}

static void test_psi_fixtures(void)
{
    struct tg_psi psi;
    size_t len;

    len = load(FX("wsl-6.18", "pressure_memory"));
    TG_CHECK(len > 0);
    TG_CHECK(tg_psi_parse(buf, len, &psi) == 0);
    TG_CHECK(psi.has_full);

    len = load(FX("wsl-6.18", "cgroup_memory.pressure"));
    TG_CHECK(len > 0);
    TG_CHECK(tg_psi_parse(buf, len, &psi) == 0);
    TG_CHECK(psi.has_full);
}

/* ---------- 키 값 형식 ---------- */

static void test_vmstat_fixture(void)
{
    struct tg_vmstat vm;
    uint64_t found = 0;
    size_t len = load(FX("wsl-6.18", "vmstat"));

    TG_CHECK(tg_vmstat_parse(buf, len, &vm, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_vmstat_nfields)); /* 6.18에는 모든 필드가 있다 */
    TG_CHECK_EQ_U64(vm.pgmajfault, 1571);
    TG_CHECK_EQ_U64(vm.oom_kill, 0);
}

static void test_meminfo_fixture(void)
{
    struct tg_meminfo mi;
    uint64_t found = 0;
    size_t len = load(FX("wsl-6.18", "meminfo"));

    TG_CHECK(tg_meminfo_parse(buf, len, &mi, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_meminfo_nfields));
    TG_CHECK_EQ_U64(mi.mem_total_kb, 6068844);
    TG_CHECK(mi.mem_available_kb > 0 && mi.mem_available_kb <= mi.mem_total_kb);
}

static void test_cg_memstat_fixture(void)
{
    struct tg_cg_memstat ms;
    uint64_t found = 0;
    size_t len = load(FX("wsl-6.18", "cgroup_memory.stat"));

    TG_CHECK(tg_cg_memstat_parse(buf, len, &ms, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_cg_memstat_nfields));
    TG_CHECK_EQ_U64(ms.anon, 22568960);
    TG_CHECK_EQ_U64(ms.file, 991731712);
}

/* 길벗과 같은 6.8 계열 커널(실험용 VM)에서 캡처한 파일도 모든 필드를 읽는다. */
static void test_vm68_fixtures(void)
{
    struct tg_psi psi;
    struct tg_vmstat vm;
    struct tg_meminfo mi;
    struct tg_cg_memstat ms;
    uint64_t found = 0;
    size_t len;

    len = load(FX("vm-6.8", "pressure_memory"));
    TG_CHECK(tg_psi_parse(buf, len, &psi) == 0 && psi.has_full);

    len = load(FX("vm-6.8", "cgroup_memory.pressure"));
    TG_CHECK(tg_psi_parse(buf, len, &psi) == 0 && psi.has_full);

    len = load(FX("vm-6.8", "vmstat"));
    TG_CHECK(tg_vmstat_parse(buf, len, &vm, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_vmstat_nfields));
    TG_CHECK_EQ_U64(vm.pgmajfault, 2388);

    len = load(FX("vm-6.8", "meminfo"));
    TG_CHECK(tg_meminfo_parse(buf, len, &mi, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_meminfo_nfields));
    TG_CHECK_EQ_U64(mi.mem_total_kb, 984620);

    len = load(FX("vm-6.8", "cgroup_memory.stat"));
    TG_CHECK(tg_cg_memstat_parse(buf, len, &ms, &found) == 0);
    TG_CHECK_EQ_U64(found, tg_mask_all(tg_cg_memstat_nfields));
}

static void test_kv_edge_cases(void)
{
    struct tg_vmstat vm;
    uint64_t found = 0;

    /* 마지막 줄에 줄바꿈이 없어도 읽는다. 원하지 않는 이상한 줄은 무시한다. */
    const char *s1 = "weird line without value\npswpin 7\npswpout 9";
    TG_CHECK(tg_vmstat_parse(s1, strlen(s1), &vm, &found) == 0);
    TG_CHECK_EQ_U64(vm.pswpin, 7);
    TG_CHECK_EQ_U64(vm.pswpout, 9);
    TG_CHECK_EQ_U64(found, (1u << 10) | (1u << 11));

    /* 원하는 키의 값이 숫자가 아니면 오류다. */
    const char *s2 = "pswpin abc\n";
    TG_CHECK(tg_vmstat_parse(s2, strlen(s2), &vm, &found) == -EINVAL);

    /* 숫자 바로 뒤에 다른 문자가 붙으면 오류다. */
    const char *s3 = "pswpin 12x\n";
    TG_CHECK(tg_vmstat_parse(s3, strlen(s3), &vm, &found) == -EINVAL);

    /* uint64_t 범위를 넘으면 오류다. */
    const char *s4 = "pswpin 99999999999999999999\n";
    TG_CHECK(tg_vmstat_parse(s4, strlen(s4), &vm, &found) == -EOVERFLOW);

    /* 키는 정확히 일치해야 한다. pgscan은 pgscan_kswapd와 다르다. */
    const char *s5 = "pgscan_kswapd_extra 5\npgscan 3\n";
    TG_CHECK(tg_vmstat_parse(s5, strlen(s5), &vm, &found) == 0);
    TG_CHECK_EQ_U64(found, 0);

    /* meminfo의 콜론과 kB 단위를 처리한다. */
    struct tg_meminfo mi;
    const char *s6 = "MemTotal:        1000 kB\nActive(file):   20 kB\n";
    TG_CHECK(tg_meminfo_parse(s6, strlen(s6), &mi, &found) == 0);
    TG_CHECK_EQ_U64(mi.mem_total_kb, 1000);
    TG_CHECK_EQ_U64(mi.active_file_kb, 20);
}

/* ---------- 파일 읽기 ---------- */

static void test_read_truncation(void)
{
    char small[16];
    size_t len = 0;

    TG_CHECK(tg_read_path(FX("wsl-6.18", "vmstat"), small, sizeof(small), &len) == -ENOBUFS);
    TG_CHECK(tg_read_path(FX("wsl-6.18", "no_such_file"), small, sizeof(small), &len) == -ENOENT);
    TG_CHECK(tg_read_path(FX("wsl-6.18", "vmstat"), small, 1, &len) == -EINVAL);
}

static void test_read_proc_live(void)
{
    /* 실행 중인 커널의 파일을 실제로 읽는다. PSI가 없는 환경이면 건너뛴다. */
    struct tg_psi psi;
    size_t len = 0;
    int rc = tg_read_path("/proc/pressure/memory", buf, sizeof(buf), &len);

    if (rc == -ENOENT || rc == -EOPNOTSUPP) {
        printf("  (PSI 없음, 건너뜀)\n");
        return;
    }
    TG_CHECK(rc == 0);
    TG_CHECK(tg_psi_parse(buf, len, &psi) == 0);
}

int main(void)
{
    TG_RUN(test_psi_values);
    TG_RUN(test_psi_without_full);
    TG_RUN(test_psi_malformed);
    TG_RUN(test_psi_overflow);
    TG_RUN(test_psi_fixtures);
    TG_RUN(test_vmstat_fixture);
    TG_RUN(test_meminfo_fixture);
    TG_RUN(test_cg_memstat_fixture);
    TG_RUN(test_vm68_fixtures);
    TG_RUN(test_kv_edge_cases);
    TG_RUN(test_read_truncation);
    TG_RUN(test_read_proc_live);
    return tg_test_finish();
}
