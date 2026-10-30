#include "dottr/ignore.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/host.h"

typedef enum {
    MATCH_LITERAL,
    MATCH_ANY,
    MATCH_BEGIN,
    MATCH_END,
} MatchKind;

typedef struct {
    MatchKind kind;
    unsigned char literal;
    size_t minimum;
    bool repeat;
} MatchAtom;

struct IgnoreRule {
    MatchAtom *atoms;
    size_t count;
    bool path;
};

/*
 * A deliberately bounded regular-expression subset: literals, escaped
 * punctuation, ., ^, $, *, +, and ?. Unsupported Perl constructs are errors,
 * never silently interpreted as literal characters or ignored.
 */
static bool compileRule(Arena *arena, const char *text, IgnoreRule *rule,
                        char *message, size_t messageSize)
{
    size_t capacity = 0;
    rule->path = strchr(text, '/') != NULL;
    for (size_t i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        MatchAtom atom = {MATCH_LITERAL, c, 1, false};
        if (c == '\\') {
            c = (unsigned char)text[++i];
            if (!c || isalnum(c)) {
                snprintf(message, messageSize,
                         "unsupported or incomplete escape");
                return false;
            }
            atom.literal = c;
        } else if (strchr("[](){}|*+?", c)) {
            snprintf(message, messageSize, "unsupported expression at '%c'", c);
            return false;
        } else if (c == '.') {
            atom.kind = MATCH_ANY;
        } else if (c == '^') {
            atom.kind = MATCH_BEGIN;
        } else if (c == '$') {
            atom.kind = MATCH_END;
        }
        char quantifier = text[i + 1];
        if (quantifier && strchr("*+?", quantifier)) {
            if (atom.kind == MATCH_BEGIN || atom.kind == MATCH_END) {
                snprintf(message, messageSize, "an anchor cannot be repeated");
                return false;
            }
            atom.minimum = quantifier == '+' ? 1 : 0;
            atom.repeat = quantifier != '?';
            i++;
        }
        rule->atoms = arenaGrow(arena, rule->atoms, rule->count, &capacity,
                                sizeof *rule->atoms);
        rule->atoms[rule->count++] = atom;
    }
    return true;
}

bool loadIgnoreList(Arena *arena, const char *file, IgnoreList *list,
                    char *message, size_t messageSize)
{
    char *absolute = hostAbsolutePath(file, message, messageSize);
    if (!absolute) {
        return false;
    }
    list->file = arenaStrdup(arena, absolute);
    free(absolute);
    char *contents;
    size_t length;
    if (!readFile(file, &contents, &length)) {
        snprintf(message, messageSize, "cannot read ignore file %s: %s", file,
                 strerror(errno));
        return false;
    }
    if (memchr(contents, '\0', length)) {
        snprintf(message, messageSize, "ignore file %s contains a NUL byte",
                 file);
        free(contents);
        return false;
    }
    size_t capacity = 0;
    bool ok = true;
    size_t line = 0;
    for (char *start = contents; *start && ok;) {
        line++;
        char *end = strchr(start, '\n');
        char *next = end ? end + 1 : start + strlen(start);
        if (end) {
            *end = '\0';
        }
        bool escaped = false;
        for (char *p = start; *p; p++) {
            if (*p == '#' && !escaped) {
                *p = '\0';
                break;
            }
            escaped = *p == '\\' && !escaped;
        }
        while (isspace((unsigned char)*start)) {
            start++;
        }
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        if (*start) {
            list->rules = arenaGrow(arena, list->rules, list->count, &capacity,
                                    sizeof *list->rules);
            IgnoreRule *rule = &list->rules[list->count];
            memset(rule, 0, sizeof *rule);
            char reason[160];
            ok = compileRule(arena, start, rule, reason, sizeof reason);
            if (!ok) {
                snprintf(message, messageSize,
                         "%s:%zu: invalid ignore rule: %s", file, line, reason);
            } else {
                list->count++;
            }
        }
        start = next;
    }
    free(contents);
    return ok;
}

static bool matchRange(const IgnoreRule *rule, const char *text, size_t start,
                       size_t end)
{
    size_t length = strlen(text);
    bool *current = xmalloc((end + 1) * sizeof *current);
    bool *next = xmalloc((end + 1) * sizeof *next);
    memset(current, 0, (end + 1) * sizeof *current);
    current[start] = true;
    for (size_t a = 0; a < rule->count; a++) {
        const MatchAtom *atom = &rule->atoms[a];
        memset(next, 0, (end + 1) * sizeof *next);
        for (size_t i = start; i <= end; i++) {
            if (atom->kind == MATCH_BEGIN || atom->kind == MATCH_END) {
                next[i] = current[i] &&
                          (atom->kind == MATCH_BEGIN ? i == 0 : i == length);
                continue;
            }
            if (atom->minimum == 0 && current[i]) {
                next[i] = true;
            }
            if (i < end && (current[i] || (atom->repeat && next[i])) &&
                (atom->kind == MATCH_ANY ||
                 atom->literal == (unsigned char)text[i])) {
                next[i + 1] = true;
            }
        }
        bool *swap = current;
        current = next;
        next = swap;
    }
    bool matched = current[end];
    free(current);
    free(next);
    return matched;
}

bool ignoresPath(const IgnoreList *list, const char *relative)
{
    if (list->count == 0) {
        return false;
    }
    StrBuf full;
    strBufInit(&full);
    strBufAppendChar(&full, '/');
    strBufAppendStr(&full, relative);
    const char *basename = strrchr(relative, '/');
    basename = basename ? basename + 1 : relative;
    size_t length = full.length;
    bool ignored = false;
    for (size_t r = 0; r < list->count && !ignored; r++) {
        const IgnoreRule *rule = &list->rules[r];
        if (!rule->path) {
            ignored = matchRange(rule, basename, 0, strlen(basename));
            continue;
        }
        for (size_t start = 0; start < length && !ignored; start++) {
            if (start > 0 && full.data[start - 1] != '/') {
                continue;
            }
            for (size_t end = start; end <= length && !ignored; end++) {
                if (end == length || full.data[end] == '/') {
                    ignored = matchRange(rule, full.data, start, end);
                }
            }
        }
    }
    strBufFree(&full);
    return ignored;
}
