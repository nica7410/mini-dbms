#ifndef MINIDB_PARSER_H
#define MINIDB_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

/* Parser: SQL 텍스트 → Statement. 파일 저장은 하지 않는다. */

typedef enum { STMT_INSERT, STMT_SELECT } StmtKind;

/* SELECT의 WHERE 조건: 없음, id = 값, age = 값 */
typedef enum { WHERE_NONE, WHERE_ID, WHERE_AGE } WhereKind;

/* INSERT 한 문장에 넣을 수 있는 최대 행 수. 묶음 전체가 쓰기 잠금 안에서 실행되므로
 * 한 문장이 잠금을 너무 오래 잡지 않도록 제한한다. */
#define INSERT_MAX_ROWS 1000

typedef struct {
    char *name; /* 따옴표를 풀어낸 문자열. Statement가 소유(malloc) */
    int age;
} InsertRow;

typedef struct {
    StmtKind kind;
    InsertRow *rows;     /* INSERT: row_count개. Statement가 소유(malloc) */
    size_t row_count;    /* INSERT: 1 이상. 파싱 도중 실패하면 name까지 채운 행 수 */
    WhereKind where;     /* SELECT */
    int64_t where_value; /* SELECT: where가 WHERE_NONE이 아닐 때 비교할 값 */
} Statement;

/*
 * sql[*pos]부터 문장 하나를 파싱한다.
 *  1: out에 문장 하나를 채움. *pos는 세미콜론 바로 다음으로 이동
 *  0: 남은 입력이 공백뿐 (입력 끝)
 * -1: 오류. err에 원인. out은 해제된 상태라 statement_free가 필요 없다
 * 성공 시 호출자는 사용 후 statement_free(out)를 호출한다.
 */
int parser_next(const char *sql, size_t *pos, Statement *out, DbError *err);

/* 여러 번 호출해도 안전하다. */
void statement_free(Statement *st);

#endif
