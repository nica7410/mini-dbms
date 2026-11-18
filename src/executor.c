#include "executor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int validate_name(const char *name, DbError *err) {
    size_t n;

    if (name == NULL) {
        db_error_set(err, "name 값이 없습니다");
        return -1;
    }
    n = strlen(name);
    if (n == 0) {
        db_error_set(err, "name은 빈 문자열일 수 없습니다");
        return -1;
    }
    if (n > NAME_MAX_BYTES) {
        db_error_set(err, "name이 %d바이트를 넘습니다 (%u바이트)", NAME_MAX_BYTES, (unsigned)n);
        return -1;
    }
    if (strpbrk(name, "\t\r\n") != NULL) {
        db_error_set(err, "name에 탭이나 줄바꿈을 넣을 수 없습니다");
        return -1;
    }
    return 0;
}

static int validate_age(int age, DbError *err) {
    if (age < AGE_MIN || age > AGE_MAX) {
        db_error_set(err, "age 값 %d이(가) 허용 범위(%d~%d)를 벗어났습니다", age, AGE_MIN, AGE_MAX);
        return -1;
    }
    return 0;
}

static int validate_row(const InsertRow *row, DbError *err) {
    if (validate_name(row->name, err) != 0) {
        return -1;
    }
    return validate_age(row->age, err);
}

/* 모든 행을 검사한다. 두 번째 행부터는 메시지에 행 번호를 붙인다. */
static int validate_insert(const Statement *st, DbError *err) {
    if (st->rows == NULL || st->row_count == 0) {
        db_error_set(err, "INSERT할 행이 없습니다");
        return -1;
    }
    if (st->row_count > INSERT_MAX_ROWS) {
        db_error_set(err, "INSERT 한 문장의 행은 최대 %d개입니다", INSERT_MAX_ROWS);
        return -1;
    }
    for (size_t i = 0; i < st->row_count; i++) {
        DbError row_err = {{0}};

        if (validate_row(&st->rows[i], &row_err) != 0) {
            if (i == 0) {
                db_error_set(err, "%s", row_err.msg);
            } else {
                db_error_set(err, "%u번째 행: %s", (unsigned)(i + 1), row_err.msg);
            }
            return -1;
        }
    }
    return 0;
}

/*
 * 검증을 모두 통과한 뒤에만 기록한다. 따라서 값 오류는 한 행도 기록하지 않는다.
 * 쓰는 도중 저장에 실패하면 앞의 행은 이미 기록되어 남는다(추가 전용, rollback 없음).
 */
static int run_insert(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    AppendRow one;
    AppendRow *rows = &one;
    int64_t first_id = 0;
    size_t stored = 0;
    bool reported = true;
    int rc;

    if (validate_insert(st, err) != 0) {
        return -1;
    }
    /* 한 행이면 스택 변수를 써서 기존 한 행 INSERT에 할당과 실패 경로를 늘리지 않는다. */
    if (st->row_count > 1) {
        rows = malloc(st->row_count * sizeof *rows);
        if (rows == NULL) {
            db_error_set(err, "메모리가 부족합니다");
            return -1;
        }
    }
    for (size_t i = 0; i < st->row_count; i++) {
        rows[i].name = st->rows[i].name;
        rows[i].age = st->rows[i].age;
    }
    /* 문장 하나의 행을 한 번에 쓰고 한 번 flush한다(group commit). */
    rc = table_append_rows(t, rows, st->row_count, &first_id, &stored, err);
    if (rows != &one) {
        free(rows);
    }

    /* 실패했더라도 파일에 기록된 행은 순서대로 알린다(서버가 기록 범위를 안다). */
    for (size_t i = 0; i < stored && reported; i++) {
        reported = sink == NULL || sink->on_insert == NULL ||
                   sink->on_insert(first_id + (int64_t)i, sink->ctx) == 0;
    }
    if (rc != 0) {
        /* 저장 실패가 결과 콜백 실패보다 중요하므로 저장 오류만 보고한다.
         * 경로가 긴 메시지가 잘려도 기록 행 수는 남도록 앞에 붙인다. */
        if (stored > 0 && err != NULL) {
            char msg[sizeof err->msg];
            memcpy(msg, err->msg, sizeof msg);
            db_error_set(err, "앞의 %u행은 기록됨: %s", (unsigned)stored, msg);
        }
        return -1;
    }
    if (!reported) {
        if (st->row_count > 1) {
            db_error_set(err, "앞의 %u행은 기록됨: 결과 출력에 실패했습니다", (unsigned)stored);
        } else {
            db_error_set(err, "결과 출력에 실패했습니다");
        }
        return -1;
    }
    return 0;
}

/*
 * WHERE age는 Storage가 순회하는 레코드 중 조건에 맞는 것만 콜백에 전달한다.
 */
typedef struct {
    int64_t age;
    RowFn next;
    void *next_ctx;
} FilterCtx;

static int filter_age(const Row *row, void *ctx) {
    FilterCtx *f = ctx;

    if (row->age != f->age || f->next == NULL) {
        return 0;
    }
    return f->next(row, f->next_ctx);
}

/* ---------- UPDATE·DELETE ---------- */

static bool where_matches(const Statement *st, const Row *row) {
    switch (st->where) {
    case WHERE_NONE:
        return true;
    case WHERE_ID:
        return row->id == st->where_value;
    case WHERE_AGE:
        return (int64_t)row->age == st->where_value;
    }
    return false;
}

/* table_rewrite 콜백. 일치한 행은 SET에 없는 컬럼을 원래 값으로 둔 채 바꾼다. */
static int edit_update(const Row *row, void *ctx, RowEdit *edit) {
    const Statement *st = ctx;

    if (where_matches(st, row)) {
        edit->action = REWRITE_REPLACE;
        edit->name = st->set_name != NULL ? st->set_name : row->name;
        edit->age = st->has_set_age ? st->set_age : row->age;
    }
    return 0;
}

static int edit_delete(const Row *row, void *ctx, RowEdit *edit) {
    if (where_matches(ctx, row)) {
        edit->action = REWRITE_DELETE;
    }
    return 0;
}

static int validate_update(const Statement *st, DbError *err) {
    if (st->set_name == NULL && !st->has_set_age) {
        db_error_set(err, "UPDATE할 컬럼이 없습니다");
        return -1;
    }
    if (st->set_name != NULL && validate_name(st->set_name, err) != 0) {
        return -1;
    }
    if (st->has_set_age && validate_age(st->set_age, err) != 0) {
        return -1;
    }
    return 0;
}

/*
 * 검증을 통과한 뒤 파일 전체를 다시 쓴다(table_rewrite). 일치한 행이 없으면 파일은 그대로다.
 * 교체 전에 실패하면 아무것도 바뀌지 않는다. 교체 뒤 실패하면 바뀐 행 수를 먼저 알린다.
 */
static int run_modify(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    RewriteFn fn = st->kind == STMT_UPDATE ? edit_update : edit_delete;
    size_t changed = 0;
    bool reported = true;
    int rc;

    if (st->kind == STMT_UPDATE && validate_update(st, err) != 0) {
        return -1;
    }
    /* 콜백은 Statement를 읽기만 한다. */
    rc = table_rewrite(t, fn, (void *)st, &changed, err);
    if (rc == 0 || changed > 0) {
        reported = sink == NULL || sink->on_change == NULL || sink->on_change(changed, sink->ctx) == 0;
    }
    if (rc != 0) {
        if (changed > 0 && err != NULL) {
            char msg[sizeof err->msg];
            memcpy(msg, err->msg, sizeof msg);
            db_error_set(err, "변경 %u행은 적용됨: %s", (unsigned)changed, msg);
        }
        return -1;
    }
    if (!reported) {
        db_error_set(err, "변경 %u행은 적용됨: 결과 출력에 실패했습니다", (unsigned)changed);
        return -1;
    }
    return 0;
}

/* SELECT 실행. r이 NULL이 아니면 t 대신 r의 파일 핸들로 읽는다. */
static int run_select(Table *t, TableReader *r, const Statement *st, const ResultSink *sink,
                      DbError *err) {
    RowFn on_row = sink != NULL ? sink->on_row : NULL;
    void *ctx = sink != NULL ? sink->ctx : NULL;
    FilterCtx f = {st->where_value, on_row, ctx};

    switch (st->where) {
    case WHERE_NONE:
        return r != NULL ? table_reader_scan(r, on_row, ctx, err) : table_scan(t, on_row, ctx, err);
    case WHERE_ID:
        return r != NULL ? table_reader_find_id(r, st->where_value, on_row, ctx, err)
                         : table_find_id(t, st->where_value, on_row, ctx, err);
    case WHERE_AGE:
        return r != NULL ? table_reader_scan(r, filter_age, &f, err) : table_scan(t, filter_age, &f, err);
    }
    db_error_set(err, "알 수 없는 WHERE 조건입니다");
    return -1;
}

int executor_run_read(TableReader *r, const Statement *st, const ResultSink *sink, DbError *err) {
    if (st->kind != STMT_SELECT) {
        db_error_set(err, "읽기 핸들로는 SELECT만 실행할 수 있습니다");
        return -1;
    }
    return run_select(NULL, r, st, sink, err);
}

int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    switch (st->kind) {
    case STMT_INSERT:
        return run_insert(t, st, sink, err);
    case STMT_SELECT:
        return run_select(t, NULL, st, sink, err);
    case STMT_UPDATE:
    case STMT_DELETE:
        return run_modify(t, st, sink, err);
    }
    db_error_set(err, "알 수 없는 문장 종류입니다");
    return -1;
}
