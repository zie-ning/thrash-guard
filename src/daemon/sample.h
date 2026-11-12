/*
 * sample.h - 판단 함수에 넘길 샘플 수집
 *
 * 시작할 때 /proc/pressure/memory와 /proc/meminfo를 한 번 열어 두고, 250 ms마다 pread로
 * 다시 읽는다. open()은 스래싱 중 느려질 수 있기 때문이다. 후보별 memory.current는
 * 후보 표(targets)가 열어 둔 fd를 그대로 쓴다.
 *
 * 힙 할당과 stdio를 쓰지 않는다. 경로를 인자로 받는 것은 테스트에서 가짜 파일을 쓰기 위해서다.
 */
#ifndef TG_SAMPLE_H
#define TG_SAMPLE_H

#include <stdint.h>

#include "judge.h"
#include "targets.h"

#define TG_READER_BUF 8192

struct tg_reader {
    int psi_fd;
    int meminfo_fd;
    char buf[TG_READER_BUF];
};

/* 두 파일을 연다. 실패하면 음수 errno이고 아무것도 열려 있지 않다. */
int tg_reader_open(struct tg_reader *r, const char *psi_path, const char *meminfo_path);
void tg_reader_close(struct tg_reader *r);

struct tg_sample_buf {
    struct tg_cand_sample cand[TG_MAX_TARGETS];
    struct tg_sample s;
    uint32_t read_failed; /* memory.current를 읽지 못해 이번 샘플에서 뺀 후보 수 */
};

/*
 * 시스템 PSI full 누적, MemAvailable, MemTotal, 후보별 memory.current를 읽어 out->s를 채운다.
 * 후보의 이름 포인터는 t의 항목을 가리키므로 t를 놓기 전까지만 유효하다.
 * 시스템 값(PSI, meminfo)을 읽지 못하면 음수 errno를 돌려주고 판단에 쓸 수 없다.
 * 후보 하나를 읽지 못하면(cgroup이 사라짐 등) 그 후보만 뺀다. 보호 여부(is_protected)는
 * 아직 채우지 않는다(false).
 */
int tg_sample_collect(struct tg_reader *r, const struct tg_targets *t, uint64_t now_ns,
                      struct tg_sample_buf *out);

#endif /* TG_SAMPLE_H */
