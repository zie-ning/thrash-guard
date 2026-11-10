/*
 * selfprot.c - 자기 보호 구현
 */
#include "selfprot.h"
#include "fmt.h"
#include "procfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

/*
 * 스택을 size 바이트만큼 미리 접근한다. 컴파일러가 지우지 못하도록 volatile을 쓰고,
 * 인라인되면 호출자 프레임과 합쳐질 수 있어 noinline으로 둔다.
 */
static __attribute__((noinline)) void prefault_stack(size_t size)
{
    volatile char area[64 * 1024];
    size_t n = size < sizeof(area) ? size : sizeof(area);
    size_t i;

    for (i = 0; i < n; i += 4096)
        area[i] = 0;
    if (n > 0)
        area[n - 1] = 0;
}

static int write_oom_score_adj(int value)
{
    char mem[16];
    struct tg_buf b;
    int fd;
    ssize_t n;

    tg_buf_init(&b, mem, sizeof(mem));
    tg_buf_i64(&b, value);
    fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    n = write(fd, mem, b.len);
    if (n < 0) {
        int e = errno;
        close(fd);
        return e;
    }
    close(fd);
    return 0;
}

void tg_selfprot_apply(const struct tg_selfprot_opts *opts, struct tg_selfprot_result *res)
{
    res->mlock_err = -1;
    res->oom_adj_err = -1;

#ifdef __GLIBC__
    /* 스레드마다 malloc arena가 생기면 mlockall 아래에서 고정 메모리가 커진다. */
    mallopt(M_ARENA_MAX, 1);
#endif

    if (opts->oom_score_adj != TG_OOM_ADJ_KEEP)
        res->oom_adj_err = write_oom_score_adj(opts->oom_score_adj);

    /* 스택을 먼저 키워 둔 뒤 고정해야 그 영역까지 함께 고정된다. */
    if (opts->stack_prefault > 0)
        prefault_stack(opts->stack_prefault);

    if (opts->mlock)
        res->mlock_err = mlockall(MCL_CURRENT | MCL_FUTURE) == 0 ? 0 : errno;
}

int tg_selfmem_read(struct tg_selfmem *out)
{
    static const struct tg_kv_field fields[] = {
        { "VmRSS", offsetof(struct tg_selfmem, vm_rss_kb) },
        { "VmLck", offsetof(struct tg_selfmem, vm_lck_kb) },
    };
    char buf[4096];
    size_t len = 0;
    int rc = tg_read_path("/proc/self/status", buf, sizeof(buf), &len);

    memset(out, 0, sizeof(*out));
    if (rc != 0)
        return rc;
    return tg_kv_parse(buf, len, fields, sizeof(fields) / sizeof(fields[0]), out, NULL);
}
