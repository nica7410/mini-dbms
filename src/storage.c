#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include "storage.h"

#include "bptree.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#define file_seek _fseeki64
#define file_tell _ftelli64
#else
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) mkdir((path), 0755)
#define file_seek fseeko
#define file_tell ftello
#endif

#define TABLE_FILE "users.tsv"
#define ID_MAX_DIGITS 19  /* INT64_MAX = 9223372036854775807 */
#define AGE_MAX_DIGITS 3
/* 가장 긴 레코드: 19 + 1 + 255 + 1 + 3 = 279바이트. 넉넉히 잡고 넘으면 손상으로 본다. */
#define LINE_CAP 320

struct Table {
    FILE *fp;      /* "a+b": 읽기는 아무 위치에서, 쓰기는 항상 파일 끝 */
    char *path;
    int64_t last_id; /* 파일에 있는 최대 ID. 빈 테이블이면 0 */
    BPTree index;
    bool write_failed;
};

/* 같은 Table의 인덱스를 공유하고, 파일 위치만 따로 갖는 읽기 핸들. */
struct TableReader {
    Table *table;
    FILE *fp; /* "rb": 읽기 전에 항상 위치를 다시 잡는다 */
};

/* ---------- 레코드 읽기 ---------- */

/* 1: 한 줄 읽음(LF 제외), 0: 파일 끝, -1: 형식 손상(*why 설정), -2: 읽기 오류 */
static int read_line(FILE *fp, char *buf, size_t cap, size_t *out_len, const char **why) {
    size_t n = 0;
    int c;

    while ((c = getc(fp)) != EOF) {
        if (c == '\n') {
            buf[n] = '\0';
            *out_len = n;
            return 1;
        }
        if (c == '\0') {
            *why = "NUL 바이트가 있습니다";
            return -1;
        }
        if (n + 1 >= cap) {
            *why = "줄이 너무 깁니다";
            return -1;
        }
        buf[n++] = (char)c;
    }
    if (ferror(fp)) {
        return -2;
    }
    if (n > 0) {
        *why = "마지막 줄이 개행(LF)으로 끝나지 않습니다";
        return -1;
    }
    return 0;
}

/* s가 0 이상의 정규형 10진수(앞자리 0 없음)이면 *out에 담고 0. */
static int parse_digits(const char *s, int max_digits, int64_t *out) {
    int64_t v = 0;
    int n = 0;

    if (s[0] == '\0' || (s[0] == '0' && s[1] != '\0')) {
        return -1;
    }
    for (; s[n] != '\0'; n++) {
        int d;
        if (s[n] < '0' || s[n] > '9' || n >= max_digits) {
            return -1;
        }
        d = s[n] - '0';
        if (v > (INT64_MAX - d) / 10) {
            return -1;
        }
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

/* line을 제자리에서 잘라 Row로 만든다. row->name은 line 안을 가리킨다. */
static int parse_record(char *line, size_t len, int64_t prev_id, Row *row, const char **why) {
    char *tab1 = memchr(line, '\t', len);
    char *tab2;
    char *name;
    size_t name_len;
    int64_t v;

    if (tab1 == NULL ||
        (tab2 = memchr(tab1 + 1, '\t', len - (size_t)(tab1 + 1 - line))) == NULL ||
        memchr(tab2 + 1, '\t', len - (size_t)(tab2 + 1 - line)) != NULL) {
        *why = "필드가 3개(id, name, age)가 아닙니다";
        return -1;
    }
    *tab1 = '\0';
    *tab2 = '\0';
    name = tab1 + 1;
    name_len = (size_t)(tab2 - name);

    if (parse_digits(line, ID_MAX_DIGITS, &v) != 0 || v <= 0) {
        *why = "id가 양의 정수가 아닙니다";
        return -1;
    }
    if (v <= prev_id) {
        *why = "id가 증가 순서가 아닙니다";
        return -1;
    }
    row->id = v;

    if (name_len == 0 || name_len > NAME_MAX_BYTES || memchr(name, '\r', name_len) != NULL) {
        *why = "name이 비었거나 255바이트를 넘거나 CR을 포함합니다";
        return -1;
    }
    row->name = name;

    if (parse_digits(tab2 + 1, AGE_MAX_DIGITS, &v) != 0 || v < AGE_MIN || v > AGE_MAX) {
        *why = "age가 0~150 정수가 아닙니다";
        return -1;
    }
    row->age = (int)v;
    return 0;
}

/* read_line과 같은 반환 규칙. len은 파일에서 읽은 LF 이전의 바이트 수다. */
static int read_row(FILE *fp, char *line, size_t *len, int64_t prev_id, Row *row,
                    const char **why) {
    int r = read_line(fp, line, LINE_CAP, len, why);
    if (r == 1 && parse_record(line, *len, prev_id, row, why) != 0) {
        return -1;
    }
    return r;
}

static BPTreeNode *reserve_index_entry(Table *t, DbError *err) {
    BPTreeNode *leaf = bptree_reserve_append(&t->index);
    if (leaf == NULL) {
        db_error_set(err, "ID 인덱스를 위한 메모리가 부족합니다");
    }
    return leaf;
}

/* fp는 t->fp 또는 TableReader의 핸들이다. 위치를 처음으로 옮겨 버퍼와 EOF 상태를 비운다. */
static int scan_rows(Table *t, FILE *fp, RowFn fn, void *ctx, bool build_index, DbError *err) {
    char line[LINE_CAP];
    int64_t lineno = 0;
    int64_t offset = 0;
    int64_t prev_id = 0;

    if (file_seek(fp, 0, SEEK_SET) != 0) {
        db_error_set(err, "데이터 파일 읽기 준비 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    for (;;) {
        Row row;
        size_t len;
        const char *why = NULL;
        int r = read_row(fp, line, &len, prev_id, &row, &why);

        if (r == 0) {
            break;
        }
        lineno++;
        if (r == -2) {
            db_error_set(err, "데이터 파일 읽기 실패: %s", t->path);
            return -1;
        }
        if (r == -1) {
            db_error_set(err, "데이터 파일 손상: %s %" PRId64 "번째 줄: %s", t->path, lineno, why);
            return -1;
        }
        if (build_index) {
            BPTreeNode *leaf = reserve_index_entry(t, err);
            if (leaf == NULL) {
                return -1;
            }
            bptree_append(leaf, row.id, offset);
            offset += (int64_t)len + 1; /* 바이너리 모드에서 LF는 1바이트 */
        }
        prev_id = row.id;
        if (fn != NULL && fn(&row, ctx) != 0) {
            db_error_set(err, "조회 결과 처리에 실패했습니다");
            return -1;
        }
    }
    if (build_index) {
        t->last_id = prev_id;
    }
    return 0;
}

static bool table_usable(Table *t, DbError *err) {
    if (t->write_failed) {
        db_error_set(err, "데이터 파일 쓰기에 실패한 테이블입니다. 다시 열어 확인하세요: %s", t->path);
        return false;
    }
    return true;
}

/* 인덱스로 위치를 찾아 fp에서 한 행을 읽는다. 없으면 0. */
static int find_id_rows(Table *t, FILE *fp, int64_t id, RowFn fn, void *ctx, DbError *err) {
    int64_t offset;
    char line[LINE_CAP];
    size_t len;
    Row row;
    const char *why = NULL;
    int r;

    if (!table_usable(t, err)) {
        return -1;
    }
    if (!bptree_find(&t->index, id, &offset)) {
        return 0;
    }
    if (file_seek(fp, offset, SEEK_SET) != 0) {
        db_error_set(err, "데이터 파일 읽기 준비 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    r = read_row(fp, line, &len, 0, &row, &why);
    if (r == -2) {
        db_error_set(err, "데이터 파일 읽기 실패: %s", t->path);
        return -1;
    }
    if (r != 1 || row.id != id) {
        db_error_set(err, "데이터 파일 손상: %s %" PRId64 " 바이트 위치: %s",
                     t->path, offset, why != NULL ? why : "ID 인덱스와 일치하는 행이 없습니다");
        return -1;
    }
    if (fn != NULL && fn(&row, ctx) != 0) {
        db_error_set(err, "조회 결과 처리에 실패했습니다");
        return -1;
    }
    return 0;
}

/* ---------- 공개 함수 ---------- */

int table_open(const char *data_dir, Table **out, DbError *err) {
    Table *t;
    size_t path_len;
    int saved_errno;

    *out = NULL;
    t = calloc(1, sizeof *t);
    if (t == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    path_len = strlen(data_dir) + 1 + strlen(TABLE_FILE) + 1;
    t->path = malloc(path_len);
    if (t->path == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        goto fail;
    }
    snprintf(t->path, path_len, "%s/%s", data_dir, TABLE_FILE);

    t->fp = fopen(t->path, "a+b");
    if (t->fp == NULL) {
        saved_errno = errno;
        if (saved_errno == ENOENT && make_dir(data_dir) == 0) {
            t->fp = fopen(t->path, "a+b");
            saved_errno = errno;
        }
        if (t->fp == NULL) {
            db_error_set(err, "데이터 파일을 열 수 없습니다: %s (%s)", t->path, strerror(saved_errno));
            goto fail;
        }
    }

    /* 시작할 때 한 번만 전체를 검증하며 인덱스와 최대 ID를 복원한다. */
    if (scan_rows(t, t->fp, NULL, NULL, true, err) != 0) {
        goto fail;
    }
    *out = t;
    return 0;

fail:
    if (t->fp != NULL) {
        fclose(t->fp);
    }
    bptree_clear(&t->index);
    free(t->path);
    free(t);
    return -1;
}

int table_scan(Table *t, RowFn fn, void *ctx, DbError *err) {
    if (!table_usable(t, err)) {
        return -1;
    }
    return scan_rows(t, t->fp, fn, ctx, false, err);
}

int table_find_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err) {
    return find_id_rows(t, t->fp, id, fn, ctx, err);
}

int table_reader_open(Table *t, TableReader **out, DbError *err) {
    TableReader *r;

    *out = NULL;
    r = calloc(1, sizeof *r);
    if (r == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    r->table = t;
    r->fp = fopen(t->path, "rb");
    if (r->fp == NULL) {
        db_error_set(err, "데이터 파일을 읽기용으로 열 수 없습니다: %s (%s)", t->path, strerror(errno));
        free(r);
        return -1;
    }
    *out = r;
    return 0;
}

int table_reader_scan(TableReader *r, RowFn fn, void *ctx, DbError *err) {
    if (!table_usable(r->table, err)) {
        return -1;
    }
    return scan_rows(r->table, r->fp, fn, ctx, false, err);
}

int table_reader_find_id(TableReader *r, int64_t id, RowFn fn, void *ctx, DbError *err) {
    return find_id_rows(r->table, r->fp, id, fn, ctx, err);
}

int table_reader_close(TableReader *r, DbError *err) {
    int rc;

    if (r == NULL) {
        return 0;
    }
    rc = fclose(r->fp);
    if (rc != 0) {
        db_error_set(err, "데이터 파일 닫기 실패: %s (%s)", r->table->path, strerror(errno));
    }
    free(r);
    return rc == 0 ? 0 : -1;
}

int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err) {
    size_t name_len = strlen(name);
    int64_t id;
    int64_t offset;
    BPTreeNode *leaf;

    if (!table_usable(t, err)) {
        return -1;
    }
    /* 파일 형식을 깨는 값은 Executor 검증과 별개로 여기서도 막는다. */
    if (name_len == 0 || name_len > NAME_MAX_BYTES || strpbrk(name, "\t\r\n") != NULL ||
        age < AGE_MIN || age > AGE_MAX) {
        db_error_set(err, "저장할 수 없는 레코드입니다");
        return -1;
    }
    if (t->last_id == INT64_MAX) {
        db_error_set(err, "더 이상 부여할 ID가 없습니다");
        return -1;
    }
    id = t->last_id + 1;

    if (file_seek(t->fp, 0, SEEK_END) != 0 || (offset = file_tell(t->fp)) < 0) {
        db_error_set(err, "데이터 파일 쓰기 준비 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    /* 파일을 쓰기 전에 노드 공간을 확보하고, 쓰기가 끝난 뒤 키를 공개한다. */
    leaf = reserve_index_entry(t, err);
    if (leaf == NULL) {
        return -1;
    }
    if (fprintf(t->fp, "%" PRId64 "\t%s\t%d\n", id, name, age) < 0 ||
        fflush(t->fp) != 0) {
        t->write_failed = true;
        db_error_set(err, "데이터 파일 쓰기 실패: %s (%s)", t->path, strerror(errno));
        return -1;
    }
    bptree_append(leaf, id, offset);
    t->last_id = id;
    if (out_id != NULL) {
        *out_id = id;
    }
    return 0;
}

int table_close(Table *t, DbError *err) {
    int rc;

    if (t == NULL) {
        return 0;
    }
    rc = fclose(t->fp);
    if (rc != 0) {
        db_error_set(err, "데이터 파일 닫기 실패: %s (%s)", t->path, strerror(errno));
    }
    bptree_clear(&t->index);
    free(t->path);
    free(t);
    return rc == 0 ? 0 : -1;
}
