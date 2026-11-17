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

/*
 * 첫 키워드만 읽어 문장 종류를 추측한다. 공백·주석·대소문자 규칙은 parser_next와 같다.
 *  1: 첫 토큰이 INSERT 또는 SELECT. *out에 종류를 담는다
 *  0: 그 밖의 경우(빈 입력, 주석만 있음, 다른 단어, 알 수 없는 문자, 닫히지 않은 주석)
 * 나머지 문장은 검사하지 않는다. 1을 돌려줘도 parser_next는 실패할 수 있다.
 * Phase 3 서버가 요청을 읽기·쓰기 큐로 나누는 데만 쓴다.
 */
int parser_peek_kind(const char *sql, StmtKind *out);

#endif
