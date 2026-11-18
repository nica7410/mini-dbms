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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) _mkdir(path)
#define file_seek _fseeki64
#define file_tell _ftelli64
#define getc_nolock _getc_nolock
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define make_dir(path) mkdir((path), 0755)
#define file_seek fseeko
#define file_tell ftello
#define getc_nolock getc_unlocked
#endif

#define TABLE_FILE "users.tsv"
#define TEMP_SUFFIX ".tmp" /* table_rewrite가 새 내용을 먼저 쓰는 파일: users.tsv.tmp */
#define ID_MAX_DIGITS 19  /* INT64_MAX = 9223372036854775807 */
#define AGE_MAX_DIGITS 3
/* 가장 긴 레코드: 19 + 1 + 255 + 1 + 3 = 279바이트. 넉넉히 잡고 넘으면 손상으로 본다. */
#define LINE_CAP 320
/* 쓰기 버퍼에서 한 레코드 줄이 차지할 수 있는 최대 바이트(LF 포함): 280 */
#define RECORD_MAX_BYTES (ID_MAX_DIGITS + 1 + NAME_MAX_BYTES + 1 + AGE_MAX_DIGITS + 1)

struct Table {
    FILE *fp;      /* "a+b": 읽기는 아무 위치에서, 쓰기는 항상 파일 끝 */
    char *path;
    int64_t last_id; /* 파일에 있는 최대 ID. 빈 테이블이면 0 */
    BPTree index;
    bool write_failed;
    TableReader *readers; /* 열린 reader 목록. table_rewrite가 파일을 바꾼 뒤 다시 연다 */
};

/* 같은 Table의 인덱스를 공유하고, 파일 위치만 따로 갖는 읽기 핸들. */
struct TableReader {
    Table *table;
    FILE *fp; /* "rb": 읽기 전에 항상 위치를 다시 잡는다. 다시 열기에 실패하면 NULL */
    TableReader *next;
};

/* ---------- 레코드 읽기 ---------- */

/* 1: 한 줄 읽음(LF 제외), 0: 파일 끝, -1: 형식 손상(*why 설정), -2: 읽기 오류
 * 문자마다 FILE 잠금을 걸지 않는 getc_nolock을 쓴다. 멀티스레드 프로세스에서 getc는
 * 문자마다 잠금을 걸어 스캔이 약 4.7배 느려진다. 하나의 FILE을 여러 스레드가 동시에
 * 쓰지 않는다는 전제가 필요하다(storage.h의 TableReader 설명 참고). */
static int read_line(FILE *fp, char *buf, size_t cap, size_t *out_len, const char **why) {
    size_t n = 0;
    int c;

    while ((c = getc_nolock(fp)) != EOF) {
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

/* 인덱스로 위치를 찾아 fp에서 한 행을 읽는다. 없으면 0.
 * 인덱스를 읽기 전에 반드시 table_usable을 확인한다. 쓰기 실패 뒤의 인덱스에는 파일에 없는
 * 행을 가리키는 키가 남을 수 있다(table_append_rows 설명). 인덱스를 읽는 새 경로도 같은
 * 규칙을 따라야 한다. */
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
    r->next = t->readers;
    t->readers = r;
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
    TableReader **link;

    if (r == NULL) {
        return 0;
    }
    for (link = &r->table->readers; *link != NULL; link = &(*link)->next) {
        if (*link == r) {
            *link = r->next;
            break;
        }
    }
    rc = r->fp != NULL ? fclose(r->fp) : 0;
    if (rc != 0) {
        db_error_set(err, "데이터 파일 닫기 실패: %s (%s)", r->table->path, strerror(errno));
    }
    free(r);
    return rc == 0 ? 0 : -1;
}

int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err) {
    AppendRow row;
    int64_t id = 0;
    size_t stored = 0;

    row.name = name;
    row.age = age;
    if (table_append_rows(t, &row, 1, &id, &stored, err) != 0) {
        return -1;
    }
    if (out_id != NULL) {
        *out_id = id;
    }
    return 0;
}

/* 열린 파일의 현재 크기. 쓰기 실패 뒤 실제로 기록된 바이트를 확인하는 데 쓴다. */
static int file_size(FILE *fp, int64_t *out) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(fp), &st) != 0) {
        return -1;
    }
#else
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) {
        return -1;
    }
#endif
    *out = (int64_t)st.st_size;
    return 0;
}

/* 파일 형식을 깨는 값은 Executor 검증과 별개로 여기서도 막는다. */
static bool storable(const AppendRow *row) {
    size_t name_len;

    if (row->name == NULL) {
        return false;
    }
    name_len = strlen(row->name);
    return name_len > 0 && name_len <= NAME_MAX_BYTES &&
           strpbrk(row->name, "\t\r\n") == NULL && row->age >= AGE_MIN && row->age <= AGE_MAX;
}

/*
 * 순서:
 *   1. 모든 행을 메모리 버퍼 하나에 레코드 줄로 만든다. ends[i]는 i번째 행의 끝 위치다.
 *   2. 행마다 인덱스 키를 넣는다. 오프셋은 시작 위치 + 버퍼 안 위치로 계산한다.
 *      출력 스트림에서 fseek하면 버퍼가 비워지므로 행마다 위치를 다시 묻지 않는다.
 *   3. 키를 넣은 행까지 한 번에 fwrite하고 한 번 fflush한다.
 * 키를 쓰기보다 먼저 넣는다. B+ 트리는 한 번에 한 자리만 확보할 수 있어서, 쓴 뒤에 넣다가
 * 메모리가 부족하면 파일에는 있는데 인덱스에 없는 행이 생기기 때문이다. 먼저 넣어도 안전한
 * 이유: 호출자가 쓰기 동안 reader를 막고(storage.h), 쓰기에 실패하면 write_failed로 Table
 * 전체를 쓸 수 없게 되어 파일에 없는 행을 가리키는 키를 아무도 읽지 않는다.
 * 키를 넣다가 메모리가 부족하면 이미 키를 넣은 앞쪽 행만 쓰고 실패를 반환한다.
 */
int table_append_rows(Table *t, const AppendRow *rows, size_t n, int64_t *first_id,
                      size_t *stored, DbError *err) {
    char local_buf[RECORD_MAX_BYTES + 1];
    size_t local_end;
    char *buf = local_buf;
    size_t *ends = &local_end;
    size_t cap = sizeof local_buf;
    size_t len = 0;
    size_t indexed = 0;
    size_t done = 0;
    int64_t start = 0;
    int rc = -1;

    if (first_id != NULL) {
        *first_id = 0;
    }
    if (stored != NULL) {
        *stored = 0;
    }
    if (!table_usable(t, err)) {
        return -1;
    }
    if (rows == NULL || n == 0) {
        db_error_set(err, "추가할 레코드가 없습니다");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (!storable(&rows[i])) {
            db_error_set(err, "저장할 수 없는 레코드입니다");
            return -1;
        }
    }
    if ((uint64_t)(INT64_MAX - t->last_id) < (uint64_t)n) {
        db_error_set(err, "더 이상 부여할 ID가 없습니다");
        return -1;
    }
    /* 한 행이면 스택 버퍼를 써서 기존 한 행 INSERT에 할당을 늘리지 않는다. */
    if (n > 1) {
        if (n > (SIZE_MAX - 1) / RECORD_MAX_BYTES) {
            db_error_set(err, "한 번에 추가할 레코드가 너무 많습니다");
            return -1;
        }
        cap = n * RECORD_MAX_BYTES + 1;
        buf = malloc(cap);
        ends = malloc(n * sizeof *ends);
        if (buf == NULL || ends == NULL) {
            db_error_set(err, "메모리가 부족합니다");
            goto out;
        }
    }
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(buf + len, cap - len, "%" PRId64 "\t%s\t%d\n",
                         t->last_id + 1 + (int64_t)i, rows[i].name, rows[i].age);
        if (w < 0 || (size_t)w >= cap - len) {
            db_error_set(err, "레코드를 만들지 못했습니다");
            goto out;
        }
        len += (size_t)w;
        ends[i] = len;
    }

    if (file_seek(t->fp, 0, SEEK_END) != 0 || (start = file_tell(t->fp)) < 0) {
        db_error_set(err, "데이터 파일 쓰기 준비 실패: %s (%s)", t->path, strerror(errno));
        goto out;
    }
    for (indexed = 0; indexed < n; indexed++) {
        BPTreeNode *leaf = reserve_index_entry(t, err);
        if (leaf == NULL) {
            break; /* err에 원인. 앞쪽 indexed행만 쓴다 */
        }
        bptree_append(leaf, t->last_id + 1 + (int64_t)indexed,
                      start + (int64_t)(indexed == 0 ? 0 : ends[indexed - 1]));
    }
    if (indexed == 0) {
        goto out;
    }
    if (fwrite(buf, 1, ends[indexed - 1], t->fp) != ends[indexed - 1] || fflush(t->fp) != 0) {
        int saved_errno = errno;
        int64_t size;

        t->write_failed = true;
        /* 끝까지 기록된 줄만 센다. 크기를 모르면 0행으로 보고한다(많게 보고하지 않는다). */
        if (file_size(t->fp, &size) == 0 && size > start) {
            while (done < indexed && (int64_t)ends[done] <= size - start) {
                done++;
            }
        }
        db_error_set(err, "데이터 파일 쓰기 실패: %s (%s)", t->path, strerror(saved_errno));
        goto out;
    }
    done = indexed;
    rc = done == n ? 0 : -1;

out:
    if (done > 0) {
        if (first_id != NULL) {
            *first_id = t->last_id + 1;
        }
        t->last_id += (int64_t)done;
    }
    if (stored != NULL) {
        *stored = done;
    }
    if (buf != local_buf) {
        free(buf);
    }
    if (ends != &local_end) {
        free(ends);
    }
    return rc;
}

/* ---------- UPDATE·DELETE: 임시 파일에 다시 쓰고 교체 ---------- */

typedef enum {
    REWRITE_OK,
    REWRITE_CALLBACK_FAILED,
    REWRITE_UNSTORABLE,
    REWRITE_NO_MEMORY,
    REWRITE_WRITE_FAILED
} RewriteFailure;

typedef struct {
    RewriteFn fn;
    void *ctx;
    FILE *out;
    BPTree index; /* 새 파일 기준 인덱스. 교체에 성공해야 Table로 옮긴다 */
    int64_t offset;
    size_t changed;
    RewriteFailure failure;
    int saved_errno;
} RewriteState;

/* scan_rows 콜백: 행 하나의 결정을 받아 임시 파일에 쓰고 새 인덱스에 키를 넣는다. */
static int rewrite_row(const Row *row, void *opaque) {
    RewriteState *s = opaque;
    RowEdit edit = {REWRITE_KEEP, NULL, 0};
    AppendRow out;
    BPTreeNode *leaf;
    int w;

    if (s->fn(row, s->ctx, &edit) != 0) {
        s->failure = REWRITE_CALLBACK_FAILED;
        return -1;
    }
    switch (edit.action) {
    case REWRITE_DELETE:
        s->changed++;
        return 0;
    case REWRITE_REPLACE:
        out.name = edit.name;
        out.age = edit.age;
        if (!storable(&out)) {
            s->failure = REWRITE_UNSTORABLE;
            return -1;
        }
        s->changed++;
        break;
    case REWRITE_KEEP:
        out.name = row->name;
        out.age = row->age;
        break;
    default:
        s->failure = REWRITE_CALLBACK_FAILED;
        return -1;
    }
    leaf = bptree_reserve_append(&s->index);
    if (leaf == NULL) {
        s->failure = REWRITE_NO_MEMORY;
        return -1;
    }
    /* 기존 줄은 정규형이므로 KEEP 행은 원래 줄과 같은 바이트로 다시 쓰인다. */
    w = fprintf(s->out, "%" PRId64 "\t%s\t%d\n", row->id, out.name, out.age);
    if (w < 0) {
        s->failure = REWRITE_WRITE_FAILED;
        s->saved_errno = errno;
        return -1;
    }
    bptree_append(leaf, row->id, s->offset);
    s->offset += w;
    return 0;
}

/* from을 to로 바꾼다. 이미 있는 to는 대체한다. */
static int replace_file(const char *from, const char *to) {
#ifdef _WIN32
    /* Windows의 rename은 대상이 있으면 실패한다. MoveFileExA는 errno를 남기지 않으므로
     * 다른 프로세스가 파일을 연 경우 같은 대표 원인으로 보고한다. */
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return 0;
    }
    errno = EACCES;
    return -1;
#else
    return rename(from, to);
#endif
}

/* fflush한 파일의 내용을 디스크까지 내린다. */
static int sync_file(FILE *fp) {
#ifdef _WIN32
    return _commit(_fileno(fp)); /* FlushFileBuffers */
#else
    return fsync(fileno(fp));
#endif
}

/* path가 든 폴더를 동기화해 이름 바꾸기를 디스크에 남긴다.
 * Windows는 MoveFileExA(MOVEFILE_WRITE_THROUGH)가 이동을 디스크에 기록한 뒤 돌아오므로 할 일이 없다.
 * 폴더를 열 권한이 없거나(EACCES) 폴더 fsync를 지원하지 않는 파일 시스템(EINVAL·EBADF)이면
 * 할 수 있는 것이 없으므로 성공으로 본다(PostgreSQL과 같은 규칙). */
static int sync_parent_dir(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    const char *slash = strrchr(path, '/');
    size_t len = slash == NULL ? 0 : (slash == path ? 1 : (size_t)(slash - path));
    char *dir = malloc(len + 2);
    int fd;
    int rc;
    int saved_errno;

    if (dir == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (len == 0) {
        strcpy(dir, ".");
    } else {
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    saved_errno = errno;
    free(dir);
    if (fd < 0) {
        errno = saved_errno;
        return saved_errno == EACCES ? 0 : -1;
    }
    rc = fsync(fd);
    saved_errno = errno;
    (void)close(fd);
    if (rc != 0 && (saved_errno == EINVAL || saved_errno == EBADF)) {
        rc = 0;
    }
    errno = saved_errno;
    return rc;
#endif
}

/* Table과 모든 reader의 파일을 path로 다시 연다. 하나라도 실패하면 Table을 쓸 수 없게 한다. */
static bool reopen_files(Table *t) {
    bool ok = true;

    t->fp = fopen(t->path, "a+b");
    if (t->fp == NULL) {
        ok = false;
    }
    for (TableReader *r = t->readers; r != NULL; r = r->next) {
        r->fp = fopen(t->path, "rb");
        if (r->fp == NULL) {
            ok = false;
        }
    }
    if (!ok) {
        t->write_failed = true;
    }
    return ok;
}

/* 열린 파일을 모두 닫는다. Windows는 열린 파일을 대체할 수 없고, POSIX에서도 reader가
 * 옛 파일을 계속 읽지 않도록 다시 열어야 한다. 쓰기는 매번 flush했으므로 닫기 오류는 무시한다. */
static void close_files(Table *t) {
    if (t->fp != NULL) {
        (void)fclose(t->fp);
        t->fp = NULL;
    }
    for (TableReader *r = t->readers; r != NULL; r = r->next) {
        if (r->fp != NULL) {
            (void)fclose(r->fp);
            r->fp = NULL;
        }
    }
}

/*
 * 순서:
 *   1. 기존 파일을 처음부터 검증하며 읽고, 행마다 fn의 결정대로 임시 파일에 쓴다.
 *      새 인덱스도 같은 순서로 만든다(ID는 그대로라 계속 증가한다).
 *   2. 바뀐 행이 없으면 임시 파일을 지우고 끝낸다. 파일은 그대로다.
 *   3. 임시 파일을 flush·fsync·close한 뒤 열린 파일을 모두 닫고 원래 파일을 대체한다.
 *   4. 인덱스를 새 것으로 바꾸고 새 파일을 다시 연 뒤, 폴더를 fsync해 대체를 디스크에 남긴다.
 * 3의 대체 전까지 실패하면 원래 파일과 인덱스는 그대로다. 대체는 내용이 디스크에 기록된 임시 파일로만
 * 하므로, 전원 장애가 나도 파일은 대체 전 내용이나 대체 후 내용 중 하나다(빈 파일·일부만 남은 파일이
 * 되지 않는다). 폴더 fsync가 실패하면 변경은 적용됐지만 장애 뒤 남을지 알 수 없으므로 Table을 막는다.
 * INSERT의 추가 쓰기는 지금처럼 fsync하지 않는다.
 */
int table_rewrite(Table *t, RewriteFn fn, void *ctx, size_t *changed, DbError *err) {
    RewriteState s;
    char *tmp_path = NULL;
    size_t tmp_len;
    bool replaced = false;
    int rc = -1;

    if (changed != NULL) {
        *changed = 0;
    }
    if (!table_usable(t, err)) {
        return -1;
    }
    if (fn == NULL) {
        db_error_set(err, "행 처리 함수가 없습니다");
        return -1;
    }
    memset(&s, 0, sizeof s);
    s.fn = fn;
    s.ctx = ctx;

    tmp_len = strlen(t->path) + sizeof TEMP_SUFFIX;
    tmp_path = malloc(tmp_len);
    if (tmp_path == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    snprintf(tmp_path, tmp_len, "%s%s", t->path, TEMP_SUFFIX);
    s.out = fopen(tmp_path, "wb");
    if (s.out == NULL) {
        db_error_set(err, "임시 파일을 만들 수 없습니다: %s (%s)", tmp_path, strerror(errno));
        free(tmp_path);
        return -1;
    }

    if (scan_rows(t, t->fp, rewrite_row, &s, false, err) != 0) {
        switch (s.failure) {
        case REWRITE_OK: /* 기존 파일 손상·읽기 오류: scan_rows가 남긴 메시지를 쓴다 */
            break;
        case REWRITE_CALLBACK_FAILED:
            db_error_set(err, "행 처리에 실패했습니다");
            break;
        case REWRITE_UNSTORABLE:
            db_error_set(err, "저장할 수 없는 레코드입니다");
            break;
        case REWRITE_NO_MEMORY:
            db_error_set(err, "ID 인덱스를 위한 메모리가 부족합니다");
            break;
        case REWRITE_WRITE_FAILED:
            db_error_set(err, "임시 파일 쓰기 실패: %s (%s)", tmp_path, strerror(s.saved_errno));
            break;
        }
        goto out;
    }
    if (s.changed == 0) {
        rc = 0;
        goto out;
    }
    if (fflush(s.out) != 0 || ferror(s.out)) {
        db_error_set(err, "임시 파일 쓰기 실패: %s (%s)", tmp_path, strerror(errno));
        goto out;
    }
    if (sync_file(s.out) != 0) {
        db_error_set(err, "임시 파일을 디스크에 기록하지 못했습니다: %s (%s)", tmp_path, strerror(errno));
        goto out;
    }
    if (fclose(s.out) != 0) {
        s.out = NULL;
        db_error_set(err, "임시 파일 닫기 실패: %s (%s)", tmp_path, strerror(errno));
        goto out;
    }
    s.out = NULL;

    close_files(t);
    if (replace_file(tmp_path, t->path) != 0) {
        int saved_errno = errno;
        /* 원래 파일은 그대로다. 다시 열지 못하면 reopen_files가 Table을 막는다. */
        if (reopen_files(t)) {
            db_error_set(err, "데이터 파일 교체 실패: %s (%s)", t->path, strerror(saved_errno));
        } else {
            db_error_set(err, "데이터 파일 교체 실패 뒤 다시 열지 못했습니다: %s", t->path);
        }
        goto out;
    }
    replaced = true;
    bptree_clear(&t->index);
    t->index = s.index;
    s.index.root = NULL;
    if (changed != NULL) {
        *changed = s.changed;
    }
    if (!reopen_files(t)) {
        db_error_set(err, "변경한 데이터 파일을 다시 열지 못했습니다: %s (%s)", t->path, strerror(errno));
        goto out;
    }
    if (sync_parent_dir(t->path) != 0) {
        t->write_failed = true;
        db_error_set(err, "데이터 파일 교체를 디스크에 기록하지 못했습니다: %s (%s)", t->path, strerror(errno));
        goto out;
    }
    rc = 0;

out:
    if (s.out != NULL) {
        (void)fclose(s.out);
    }
    if (!replaced) {
        (void)remove(tmp_path);
    }
    bptree_clear(&s.index);
    free(tmp_path);
    return rc;
}

int table_close(Table *t, DbError *err) {
    int rc;

    if (t == NULL) {
        return 0;
    }
    rc = t->fp != NULL ? fclose(t->fp) : 0;
    if (rc != 0) {
        db_error_set(err, "데이터 파일 닫기 실패: %s (%s)", t->path, strerror(errno));
    }
    bptree_clear(&t->index);
    free(t->path);
    free(t);
    return rc == 0 ? 0 : -1;
}
