#ifndef MINIDB_STORAGE_H
#define MINIDB_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/*
 * Storage: users.tsv 파일 열기, 순차 읽기, 추가, ID 인덱스 관리.
 * SQL 문법은 알지 않는다.
 * 파일 형식: 헤더 없이 한 줄에 "id<TAB>name<TAB>age<LF>".
 */

typedef struct Table Table; /* 불투명 타입. 내부는 storage.c에만 있다 */

typedef struct {
    int64_t id;
    const char *name; /* 콜백이 실행되는 동안만 유효. 보관하려면 복사한다 */
    int age;
} Row;

/* 0을 반환하면 계속, 0이 아니면 순회를 멈추고 table_scan이 -1을 반환한다. */
typedef int (*RowFn)(const Row *row, void *ctx);

/* 반환 규칙: 0 성공, -1 실패(err에 원인) */

/* data_dir/users.tsv를 연다. 없으면 만든다(data_dir 한 단계 포함).
 * 기존 레코드 전체를 한 번 검증해 최대 ID와 메모리 인덱스를 복원한다. */
int table_open(const char *data_dir, Table **out, DbError *err);

/* 다음 ID(최대 ID + 1)를 부여해 레코드 한 건을 파일 끝에 추가한다. */
int table_append(Table *t, const char *name, int age, int64_t *out_id, DbError *err);

typedef struct {
    const char *name;
    int age;
} AppendRow;

/*
 * rows n개를 연속 ID로 파일 끝에 추가한다. 한 번에 쓰고 한 번 flush한다(group commit).
 * 성공: 0, *stored == n.
 * 실패: -1. *stored는 파일에 끝까지 기록된 앞쪽 행 수(0 이상)이고, 그 행들의 ID는
 * *first_id부터 연속이다. 쓰기가 일부만 되고 실패하면 파일 크기로 이 수를 센다.
 * 쓰기 실패 뒤에는 table_append와 마찬가지로 Table을 더 쓸 수 없다.
 * 쓰기 전에 인덱스 메모리가 부족하면 앞쪽 일부만 쓰고 -1을 반환한다(*stored > 0일 수 있음).
 * 이 경우는 쓰기 실패가 아니므로 Table을 계속 쓸 수 있다.
 */
int table_append_rows(Table *t, const AppendRow *rows, size_t n, int64_t *first_id,
                      size_t *stored, DbError *err);

/* table_rewrite가 행마다 받는 결정. 콜백을 부르기 전에 REWRITE_KEEP으로 채워 둔다. */
typedef enum { REWRITE_KEEP, REWRITE_REPLACE, REWRITE_DELETE } RewriteAction;

typedef struct {
    RewriteAction action;
    const char *name; /* REPLACE: 새 name. 콜백이 돌아온 직후 기록하므로 그때까지만 유효하면 된다 */
    int age;          /* REPLACE: 새 age */
} RowEdit;

/* 0을 반환하면 계속, 0이 아니면 멈추고 아무것도 바꾸지 않는다. */
typedef int (*RewriteFn)(const Row *row, void *ctx, RowEdit *edit);

/*
 * UPDATE·DELETE용. 저장 순서대로 모든 행에 fn을 호출해 남길지(KEEP), 바꿀지(REPLACE),
 * 지울지(DELETE) 정한다. 결과를 임시 파일(users.tsv.tmp)에 모두 쓴 뒤 원래 파일과 바꾸고,
 * 새 파일 기준으로 만든 ID 인덱스로 교체한다. id는 바뀌지 않으며 저장 순서도 그대로다.
 * 비용은 문장마다 파일 전체를 읽고 쓰는 O(행 수)다.
 * 성공: 0. *changed는 REPLACE·DELETE한 행 수. 0이면 파일을 바꾸지 않는다.
 * 실패: -1.
 *   *changed == 0: 파일과 인덱스는 그대로다. 파일을 다시 열지 못한 경우가 아니면 Table을
 *                  계속 쓸 수 있다(table_usable과 같은 규칙).
 *   *changed > 0 : 파일 교체는 끝났지만 다시 열지 못했다. 변경은 적용됐고 Table은 더 쓸 수 없다.
 * 부여한 최대 ID(다음 INSERT의 ID 기준)는 줄지 않는다. 단, 재시작하면 파일에 남은 최대 ID에서
 * 다시 계산하므로 끝쪽 ID를 지운 뒤 재시작하면 그 ID가 다시 쓰일 수 있다.
 * 열려 있는 TableReader의 파일도 새 파일로 다시 연다. 따라서 reader 조회·열기·닫기와 동시에
 * 실행하면 안 된다(table_append_rows와 같은 규칙).
 */
int table_rewrite(Table *t, RewriteFn fn, void *ctx, size_t *changed, DbError *err);

/* 저장 순서대로 레코드를 하나씩 읽어 fn을 호출한다. fn이 NULL이면 검증만 한다. */
int table_scan(Table *t, RowFn fn, void *ctx, DbError *err);

/* ID 인덱스로 레코드를 찾아 fn을 한 번 호출한다. 일치하는 행이 없으면 0을 반환한다.
 * fn이 NULL이면 일치한 레코드를 읽어 검증만 한다. */
int table_find_id(Table *t, int64_t id, RowFn fn, void *ctx, DbError *err);

/* 파일을 닫고 메모리를 해제한다. 실패해도 t는 해제된다. t가 NULL이면 0. */
int table_close(Table *t, DbError *err);

/*
 * TableReader: 같은 Table의 ID 인덱스를 공유하면서 파일 위치만 따로 갖는 읽기 핸들.
 * 여러 스레드가 각자 reader로 동시에 읽을 수 있게 하려는 것이다.
 * - reader는 Table보다 먼저 닫아야 한다.
 * - 한 reader를 여러 스레드가 동시에 쓰면 안 된다. Table도 마찬가지다.
 *   (파일은 FILE 잠금 없이 읽으므로 같은 핸들의 동시 사용은 정의되지 않은 동작이다.)
 * - 인덱스를 공유하므로 reader 조회와 table_append·table_append_rows·table_rewrite가 동시에
 *   실행되면 안 된다. (읽기끼리는 동시에 실행해도 된다. 동기화는 호출자가 맡는다.)
 * - Table은 열린 reader 목록을 갖는다. table_rewrite가 파일을 바꾸면 reader의 파일도 다시 연다.
 *   reader 열기·닫기도 쓰기와 동시에 실행하면 안 된다.
 * 조회 규칙과 반환값은 table_scan / table_find_id와 같다.
 */
typedef struct TableReader TableReader;

int table_reader_open(Table *t, TableReader **out, DbError *err);
int table_reader_scan(TableReader *r, RowFn fn, void *ctx, DbError *err);
int table_reader_find_id(TableReader *r, int64_t id, RowFn fn, void *ctx, DbError *err);
/* 파일을 닫고 메모리를 해제한다. 실패해도 r은 해제된다. r이 NULL이면 0. */
int table_reader_close(TableReader *r, DbError *err);

#endif
