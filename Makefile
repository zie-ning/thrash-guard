# Thrash Guard 빌드
#
#   make                 # glibc 정적 링크로 tg-sampler, thrash-guard 빌드
#   make LIBC=musl       # musl 정적 링크 빌드 (musl-tools 필요)
#   make test            # 단위 테스트 (ASan/UBSan, 동적 링크)
#   make lint            # cppcheck 정적 분석
#
# D2 결정: glibc·musl 두 빌드를 모두 지원하고, M3에서 실측해 기본값을 고른다.

LIBC ?= glibc
ifeq ($(LIBC),musl)
CC := musl-gcc
else ifneq ($(LIBC),glibc)
$(error LIBC는 glibc 또는 musl 이어야 한다: $(LIBC))
endif

CSTD   := -std=c11 -D_GNU_SOURCE -pthread
WARN   := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes \
          -Wmissing-prototypes -Wcast-align -Wpointer-arith -Werror
CFLAGS ?= -O2 -g
INC    := -Isrc/common -Isrc/daemon

BUILD  := build/$(LIBC)
TBUILD := build/test

COMMON_SRC := src/common/procfs.c src/common/selfprot.c src/common/fmt.c
SAMPLER_SRC := src/sampler/main.c
# 데몬 모듈. main.c를 뺀 나머지는 테스트에도 함께 링크한다.
DAEMON_LIB_SRC := src/daemon/config.c src/daemon/policy.c src/daemon/targets.c \
                  src/daemon/scanner.c src/daemon/judge.c src/daemon/log.c
DAEMON_SRC := src/daemon/main.c

SAMPLER := $(BUILD)/tg-sampler
DAEMON  := $(BUILD)/thrash-guard

.PHONY: all test test-tsan lint clean

all: $(SAMPLER) $(DAEMON)

$(SAMPLER): $(COMMON_SRC) $(SAMPLER_SRC) $(wildcard src/common/*.h)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INC) -static -o $@ $(COMMON_SRC) $(SAMPLER_SRC)

$(DAEMON): $(COMMON_SRC) $(DAEMON_LIB_SRC) $(DAEMON_SRC) $(wildcard src/common/*.h src/daemon/*.h)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(CFLAGS) $(INC) -static -o $@ $(COMMON_SRC) $(DAEMON_LIB_SRC) $(DAEMON_SRC)

# 테스트는 sanitizer를 쓰기 위해 동적 링크로 빌드한다. musl-gcc는 sanitizer를
# 지원하지 않으므로 LIBC와 무관하게 시스템 컴파일러를 쓴다.
TEST_CC ?= cc
TEST_CFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
               -fno-sanitize-recover=all -DTG_FIXTURE_DIR='"tests/fixtures"'
TESTS := $(TBUILD)/test_procfs $(TBUILD)/test_fmt $(TBUILD)/test_config \
         $(TBUILD)/test_policy $(TBUILD)/test_targets $(TBUILD)/test_judge $(TBUILD)/test_log

$(TBUILD)/test_%: tests/test_%.c $(COMMON_SRC) $(DAEMON_LIB_SRC) tests/tg_test.h \
                  $(wildcard src/common/*.h src/daemon/*.h)
	@mkdir -p $(@D)
	$(TEST_CC) $(CSTD) $(WARN) $(TEST_CFLAGS) $(INC) -Itests -o $@ $< $(COMMON_SRC) $(DAEMON_LIB_SRC)

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

# 스레드를 쓰는 모듈(로그 링버퍼, 스캔 스레드)의 데이터 경쟁 검사. ThreadSanitizer는
# AddressSanitizer와 함께 쓸 수 없어 별도 빌드로 돌린다.
TSBUILD := build/tsan
TSAN_TESTS := $(TSBUILD)/test_log $(TSBUILD)/test_targets

$(TSBUILD)/test_%: tests/test_%.c $(COMMON_SRC) $(DAEMON_LIB_SRC) tests/tg_test.h \
                   $(wildcard src/common/*.h src/daemon/*.h)
	@mkdir -p $(@D)
	$(TEST_CC) $(CSTD) $(WARN) -O1 -g -fno-omit-frame-pointer -fsanitize=thread \
	    -fno-sanitize-recover=all -DTG_FIXTURE_DIR='"tests/fixtures"' $(INC) -Itests \
	    -o $@ $< $(COMMON_SRC) $(DAEMON_LIB_SRC)

# 최신 커널의 주소 무작위화(vm.mmap_rnd_bits)가 ThreadSanitizer와 충돌하는 환경이 있어,
# 시스템 설정은 건드리지 않고 프로세스 단위로 무작위화를 끄고(setarch -R) 실행한다.
test-tsan: $(TSAN_TESTS)
	@set -e; for t in $(TSAN_TESTS); do echo "== $$t"; setarch $$(uname -m) -R ./$$t; done

lint:
	cppcheck --enable=warning,performance,portability --std=c11 --error-exitcode=1 \
	    --inline-suppr --quiet -Isrc/common src tests

clean:
	rm -rf build
