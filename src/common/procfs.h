/*
 * procfs.h - /proc, cgroup 메모리 통계 파서
 *
 * 스래싱 중에도 동작해야 하므로 이 모듈의 함수는 모두 다음을 지킨다.
 *   - 힙 할당을 하지 않는다. 버퍼는 호출자가 준비한다.
 *   - stdio를 쓰지 않는다. 읽기는 read/pread 시스템 콜만 쓴다.
 *   - 부동소수점을 쓰지 않는다. PSI 평균값은 100배한 정수로 보관한다.
 *
 * 반환값 규약: 성공 0, 실패 시 음수 errno.
 *   -EINVAL    : 형식이 예상과 다르다
 *   -EOVERFLOW : 숫자가 uint64_t 범위를 넘는다
 *   -ENOBUFS   : 파일이 버퍼보다 크다
 *   그 밖의 값 : 시스템 콜이 돌려준 errno
 */
#ifndef TG_PROCFS_H
#define TG_PROCFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- 파일 읽기 ---------- */

/*
 * 열린 fd를 오프셋 0부터 다시 읽어 buf에 NUL 종료 문자열로 담는다.
 * /proc 파일은 fd를 열어 둔 채 pread로 반복해 읽으면 open() 비용을 줄일 수 있다.
 */
int tg_read_fd(int fd, char *buf, size_t cap, size_t *len);

/* 경로를 열어 한 번 읽고 닫는다. 초기화나 테스트에서 쓴다. */
int tg_read_path(const char *path, char *buf, size_t cap, size_t *len);

/* ---------- PSI (/proc/pressure/memory, cgroup memory.pressure) ---------- */

struct tg_psi_line {
    uint32_t avg10;   /* 최근 10초 평균 stall 비율(%) × 100. 예: 12.34% → 1234 */
    uint32_t avg60;
    uint32_t avg300;
    uint64_t total_us; /* 누적 stall 시간(마이크로초) */
};

struct tg_psi {
    struct tg_psi_line some; /* 일부 작업이 메모리 때문에 멈춘 시간 */
    struct tg_psi_line full; /* 모든 작업이 메모리 때문에 멈춘 시간 */
    bool has_full;
};

int tg_psi_parse(const char *buf, size_t len, struct tg_psi *out);

/* ---------- "키 값" 형식 파서 (vmstat, meminfo, memory.stat 공용) ---------- */

struct tg_kv_field {
    const char *key; /* meminfo의 끝 콜론(:)은 빼고 적는다 */
    size_t offset;   /* 출력 구조체 안의 uint64_t 멤버 오프셋 */
};

/*
 * 각 줄의 첫 토큰을 키로, 이어지는 10진수를 값으로 읽는다. 값 뒤의 단위(kB)는 무시한다.
 * 원하는 키만 out에 기록하고, 찾은 필드는 found 비트마스크(i번째 필드 → i번 비트)에 표시한다.
 * 원하지 않는 줄은 형식이 이상해도 건너뛴다. 원하는 키의 값이 숫자가 아니면 -EINVAL.
 * nfields는 64 이하여야 한다.
 */
int tg_kv_parse(const char *buf, size_t len, const struct tg_kv_field *fields,
                size_t nfields, void *out, uint64_t *found);

/* ---------- /proc/vmstat ---------- */

struct tg_vmstat {
    uint64_t pgscan_kswapd;           /* kswapd가 스캔한 페이지 */
    uint64_t pgscan_direct;           /* 할당 경로에서 직접 스캔한 페이지 */
    uint64_t pgsteal_kswapd;          /* kswapd가 실제 회수한 페이지 */
    uint64_t pgsteal_direct;          /* 직접 회수한 페이지 */
    uint64_t pgscan_anon;
    uint64_t pgscan_file;
    uint64_t pgsteal_anon;
    uint64_t pgsteal_file;
    uint64_t workingset_refault_anon; /* 쫓아냈다가 곧 다시 읽은 익명 페이지 */
    uint64_t workingset_refault_file; /* 쫓아냈다가 곧 다시 읽은 파일 페이지 */
    uint64_t pswpin;
    uint64_t pswpout;
    uint64_t pgmajfault;              /* 디스크 읽기가 필요한 페이지 폴트 */
    uint64_t allocstall_normal;       /* 할당이 직접 회수로 멈춘 횟수 */
    uint64_t allocstall_movable;
    uint64_t oom_kill;
};

extern const struct tg_kv_field tg_vmstat_fields[];
extern const size_t tg_vmstat_nfields;

int tg_vmstat_parse(const char *buf, size_t len, struct tg_vmstat *out, uint64_t *found);

/* ---------- /proc/meminfo (단위: kB) ---------- */

struct tg_meminfo {
    uint64_t mem_total_kb;
    uint64_t mem_free_kb;
    uint64_t mem_available_kb;
    uint64_t buffers_kb;
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    uint64_t active_anon_kb;
    uint64_t inactive_anon_kb;
    uint64_t active_file_kb;
    uint64_t inactive_file_kb;
    uint64_t dirty_kb;
    uint64_t writeback_kb;
};

extern const struct tg_kv_field tg_meminfo_fields[];
extern const size_t tg_meminfo_nfields;

int tg_meminfo_parse(const char *buf, size_t len, struct tg_meminfo *out, uint64_t *found);

/* ---------- cgroup v2 memory.stat (anon/file 단위: 바이트) ---------- */

struct tg_cg_memstat {
    uint64_t anon;
    uint64_t file;
    uint64_t shmem;
    uint64_t workingset_refault_anon;
    uint64_t workingset_refault_file;
    uint64_t pgscan;
    uint64_t pgsteal;
    uint64_t pgmajfault;
};

extern const struct tg_kv_field tg_cg_memstat_fields[];
extern const size_t tg_cg_memstat_nfields;

int tg_cg_memstat_parse(const char *buf, size_t len, struct tg_cg_memstat *out,
                        uint64_t *found);

/* 0..n-1 비트가 모두 켜진 마스크. "필수 필드를 다 찾았는가" 검사에 쓴다. */
static inline uint64_t tg_mask_all(size_t n)
{
    return n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
}

#endif /* TG_PROCFS_H */
