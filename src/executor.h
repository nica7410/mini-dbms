#ifndef MINIDB_EXECUTOR_H
#define MINIDB_EXECUTOR_H

#include <stdint.h>

#include "common.h"
#include "parser.h"
#include "storage.h"

/*
 * Executor: Statement의 값을 검증하고 Storage에 저장·조회를 요청한다.
 * stdout에 직접 쓰지 않고, 결과는 ResultSink 콜백으로 넘긴다.
 */

typedef struct {
    int (*on_insert)(int64_t id, void *ctx); /* 0이 아니면 실패로 처리 */
    RowFn on_row;
    void *ctx;
} ResultSink;

/* 0 성공, -1 실패(err에 원인). sink나 그 안의 콜백은 NULL이어도 된다. */
int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err);

/* SELECT만 실행한다. 결과는 executor_run과 같고, 파일은 r의 핸들로 읽는다.
 * 동시성 조건은 storage.h의 TableReader 설명을 따른다. */
int executor_run_read(TableReader *r, const Statement *st, const ResultSink *sink, DbError *err);

#endif
