/*
 * selfprot.h - 자기 보호
 *
 * 스래싱 중에도 프로세스가 멈추지 않도록 초기화 단계에서 한 번 호출한다.
 *   - mlockall: 이 프로세스의 메모리가 회수(디스크로 쫓겨남)되지 않게 고정한다
 *   - 스택 사전 접근: 나중에 스택이 자라며 페이지 폴트가 나지 않게 미리 만진다
 *   - glibc malloc arena를 1개로 제한해 고정 메모리가 불필요하게 늘지 않게 한다
 *   - oom_score_adj: 커널 OOM killer의 대상에서 제외한다
 *
 * 각 단계의 성공 여부를 결과 구조체에 남긴다. 권한이 없어 실패한 단계가 있어도
 * 호출자가 계속할지 정할 수 있도록 여기서 종료하지 않는다.
 */
#ifndef TG_SELFPROT_H
#define TG_SELFPROT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tg_selfprot_opts {
    bool mlock;            /* mlockall 수행 여부 */
    size_t stack_prefault; /* 미리 접근할 스택 크기(바이트). 0이면 생략 */
    int oom_score_adj;     /* 설정할 값. -1001이면 건드리지 않는다 */
};

struct tg_selfprot_result {
    int mlock_err;   /* 0 성공, 양수 errno, -1 생략 */
    int oom_adj_err; /* 0 성공, 양수 errno, -1 생략 */
};

#define TG_OOM_ADJ_KEEP (-1001)

void tg_selfprot_apply(const struct tg_selfprot_opts *opts, struct tg_selfprot_result *res);

/* /proc/self/status에서 VmRSS, VmLck(kB)를 읽는다. 자기 보호 효과 확인용. */
struct tg_selfmem {
    uint64_t vm_rss_kb;
    uint64_t vm_lck_kb;
};

int tg_selfmem_read(struct tg_selfmem *out);

#endif /* TG_SELFPROT_H */
