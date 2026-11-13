/*
 * match.c - cgroup 이름 패턴 구현
 */
#include "match.h"

#include <string.h>

/* pat[0..plen)이 name 전체에 맞는지. '*'만 특별하다(되돌아가기 한 단계). */
static bool glob_match(const char *pat, size_t plen, const char *name)
{
    size_t p = 0;
    const char *n = name;
    size_t star = (size_t)-1;
    const char *star_n = NULL;

    while (*n != '\0') {
        if (p < plen && pat[p] == '*') {
            star = p++;
            star_n = n;
        } else if (p < plen && pat[p] == *n) {
            p++;
            n++;
        } else if (star != (size_t)-1) {
            p = star + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (p < plen && pat[p] == '*')
        p++;
    return p == plen;
}

/* 다음 패턴의 [*start, *end)를 앞뒤 공백을 떼고 구한다. 끝에 닿으면 false. */
static bool next_pattern(const char **cursor, const char **start, const char **end)
{
    const char *p = *cursor;
    const char *e;

    if (p == NULL || *p == '\0')
        return false;
    e = p;
    while (*e != '\0' && *e != ',')
        e++;
    *cursor = (*e == ',') ? e + 1 : e;
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *start = p;
    *end = e;
    return true;
}

bool tg_match_name(const char *patterns, const char *name)
{
    const char *cur = patterns;
    const char *s;
    const char *e;

    if (patterns == NULL)
        return true;
    while (next_pattern(&cur, &s, &e)) {
        if (e > s && glob_match(s, (size_t)(e - s), name))
            return true;
    }
    return false;
}

bool tg_match_valid(const char *patterns)
{
    const char *cur = patterns;
    const char *s;
    const char *e;
    bool any = false;

    if (patterns == NULL)
        return false;
    while (next_pattern(&cur, &s, &e)) {
        if (e == s)
            return false; /* 빈 패턴(쉼표가 연달아 있거나 끝에 있다) */
        if ((size_t)(e - s) > TG_MATCH_NAME_MAX)
            return false;
        if (memchr(s, '/', (size_t)(e - s)) != NULL)
            return false;
        any = true;
    }
    /* 끝이 쉼표로 끝나면 마지막 빈 패턴이 있다 */
    {
        size_t len = strlen(patterns);

        while (len > 0 && (patterns[len - 1] == ' ' || patterns[len - 1] == '\t'))
            len--;
        if (len > 0 && patterns[len - 1] == ',')
            return false;
    }
    return any;
}
