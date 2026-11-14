#include "executor.h"

#include <string.h>

static int validate_insert(const Statement *st, DbError *err) {
    size_t n;

    if (st->name == NULL) {
        db_error_set(err, "name 값이 없습니다");
        return -1;
    }
    n = strlen(st->name);
    if (n == 0) {
        db_error_set(err, "name은 빈 문자열일 수 없습니다");
        return -1;
    }
    if (n > NAME_MAX_BYTES) {
        db_error_set(err, "name이 %d바이트를 넘습니다 (%u바이트)", NAME_MAX_BYTES, (unsigned)n);
        return -1;
    }
    if (strpbrk(st->name, "\t\r\n") != NULL) {
        db_error_set(err, "name에 탭이나 줄바꿈을 넣을 수 없습니다");
        return -1;
    }
    if (st->age < AGE_MIN || st->age > AGE_MAX) {
        db_error_set(err, "age 값 %d이(가) 허용 범위(%d~%d)를 벗어났습니다",
                     st->age, AGE_MIN, AGE_MAX);
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

int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err) {
    switch (st->kind) {
    case STMT_INSERT: {
        int64_t id;
        /* 검증을 모두 통과한 뒤에만 기록한다. */
        if (validate_insert(st, err) != 0 ||
            table_append(t, st->name, st->age, &id, err) != 0) {
            return -1;
        }
        if (sink != NULL && sink->on_insert != NULL && sink->on_insert(id, sink->ctx) != 0) {
            db_error_set(err, "결과 출력에 실패했습니다");
            return -1;
        }
        return 0;
    }
    case STMT_SELECT: {
        RowFn on_row = sink != NULL ? sink->on_row : NULL;
        void *ctx = sink != NULL ? sink->ctx : NULL;
        FilterCtx f = {st->where_value, on_row, ctx};

        switch (st->where) {
        case WHERE_NONE: return table_scan(t, on_row, ctx, err);
        case WHERE_ID: return table_find_id(t, st->where_value, on_row, ctx, err);
        case WHERE_AGE: return table_scan(t, filter_age, &f, err);
        }
        db_error_set(err, "알 수 없는 WHERE 조건입니다");
        return -1;
    }
    }
    db_error_set(err, "알 수 없는 문장 종류입니다");
    return -1;
}
