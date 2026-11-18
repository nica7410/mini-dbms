#ifndef MINIDB_EXECUTOR_H
#define MINIDB_EXECUTOR_H

#include <stddef.h>
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
    /* UPDATE·DELETE: 바꾸거나 지운 행 수를 한 번 알린다. 0이 아니면 실패로 처리.
     * 기존 위치 초기화({on_insert, on_row, ctx})를 깨지 않도록 끝에 둔다. */
    int (*on_change)(size_t affected, void *ctx);
} ResultSink;

/*
 * 0 성공, -1 실패(err에 원인). sink나 그 안의 콜백은 NULL이어도 된다.
 * UPDATE·DELETE는 table_rewrite로 파일 전체를 다시 쓴다. 값 검증이나 쓰기 준비 중 실패하면
 * 아무것도 바뀌지 않는다. 파일 교체 뒤 다시 열기에 실패하면 변경은 적용된 채 -1을 반환하고,
 * on_change로 적용된 행 수를 먼저 알린 뒤 메시지 앞에 "변경 N행은 적용됨:"을 붙인다.
 */
int executor_run(Table *t, const Statement *st, const ResultSink *sink, DbError *err);

/* SELECT만 실행한다. 결과는 executor_run과 같고, 파일은 r의 핸들로 읽는다.
 * 동시성 조건은 storage.h의 TableReader 설명을 따른다. */
int executor_run_read(TableReader *r, const Statement *st, const ResultSink *sink, DbError *err);

#endif
