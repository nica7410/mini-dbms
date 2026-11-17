/*
 * 단위 테스트: Parser, Storage, Executor를 함수 단위로 검증한다.
 * 사용법: test_unit <임시 디렉터리>
 */
#ifndef _WIN32
#define _XOPEN_SOURCE 700 /* setrlimit, SIGXFSZ: 쓰기 실패 재현용 */
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "parser.h"
#include "storage.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#define make_dir(path) mkdir((path), 0755)
#endif

static int g_checks;
static int g_failures;
static const char *g_base;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: 실패: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                        \
    } while (0)

/* ---------- 도우미 ---------- */

/* base/name 디렉터리를 비운 상태로 준비하고 경로를 돌려준다. */
static const char *fresh_dir(const char *name) {
    static char dir[512];
    char file[600];

    snprintf(dir, sizeof dir, "%s/%s", g_base, name);
    snprintf(file, sizeof file, "%s/users.tsv", dir);
    remove(file);
    make_dir(dir);
    return dir;
}

static void write_file(const char *dir, const char *data, size_t len) {
    char file[600];
    FILE *fp;

    snprintf(file, sizeof file, "%s/users.tsv", dir);
    fp = fopen(file, "wb");
    if (fp == NULL) {
        fprintf(stderr, "테스트 파일 생성 실패: %s\n", file);
        exit(2);
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

/* 파일 전체를 buf에 읽는다. 없으면 길이 0. */
static size_t read_file(const char *dir, char *buf, size_t cap) {
    char file[600];
    FILE *fp;
    size_t n;

    snprintf(file, sizeof file, "%s/users.tsv", dir);
    fp = fopen(file, "rb");
    if (fp == NULL) {
        buf[0] = '\0';
        return 0;
    }
    n = fread(buf, 1, cap - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return n;
}

typedef struct {
    int count;
    int64_t ids[8];
    char names[8][300];
    int ages[8];
    int stop_after; /* 0이면 멈추지 않음 */
} Collected;

static int collect_row(const Row *row, void *ctx) {
    Collected *c = ctx;

    if (c->count < 8) {
        c->ids[c->count] = row->id;
        snprintf(c->names[c->count], sizeof c->names[0], "%s", row->name);
        c->ages[c->count] = row->age;
    }
    c->count++;
    return (c->stop_after != 0 && c->count >= c->stop_after) ? 1 : 0;
}

/* ---------- Parser ---------- */

static int parse_one(const char *sql, Statement *st, DbError *err) {
    size_t pos = 0;
    return parser_next(sql, &pos, st, err);
}

#define EXPECT_PARSE_ERROR(sql, needle)                                                   \
    do {                                                                                  \
        Statement st_;                                                                    \
        DbError e_ = {{0}};                                                               \
        int r_ = parse_one((sql), &st_, &e_);                                             \
        CHECK(r_ == -1);                                                                  \
        CHECK(strstr(e_.msg, (needle)) != NULL);                                          \
        if (r_ != -1 || strstr(e_.msg, (needle)) == NULL) {                               \
            fprintf(stderr, "    SQL: %s\n    메시지: %s\n    기대: %s\n", (sql), e_.msg, \
                    (needle));                                                            \
        }                                                                                 \
        CHECK(st_.rows == NULL);                                                          \
        if (r_ == 1) {                                                                    \
            statement_free(&st_);                                                         \
        }                                                                                 \
    } while (0)

static void test_parse_insert_basic(void) {
    const char *sql = "INSERT INTO users (name, age) VALUES ('민수', 25);";
    Statement st;
    DbError err = {{0}};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "민수") == 0);
    CHECK(st.rows[0].age == 25);
    CHECK(pos == strlen(sql));
    statement_free(&st);
    CHECK(st.rows == NULL && st.row_count == 0);
    statement_free(&st); /* 두 번 호출해도 안전 */
}

static void test_parse_column_order_and_case(void) {
    Statement st;
    DbError err = {{0}};

    CHECK(parse_one("insert into USERS (Age, NAME) values (30, 'O''Brien');", &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "O'Brien") == 0);
    CHECK(st.rows[0].age == 30);
    statement_free(&st);
}

static void test_parse_string_special_and_multiline(void) {
    Statement st;
    DbError err = {{0}};

    CHECK(parse_one("INSERT\n  INTO users\n(name,\tage)\r\nVALUES\n('a;b ''c''',\n 0)\n;", &st,
                    &err) == 1);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "a;b 'c'") == 0);
    CHECK(st.rows[0].age == 0);
    statement_free(&st);

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('''', 150);", &st, &err) == 1);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "'") == 0);
    CHECK(st.rows[0].age == 150);
    statement_free(&st);
}

static void test_parse_select_and_sequence(void) {
    const char *sql = "SELECT * FROM users; insert into users (name, age) values ('x', 1);\n  \n";
    Statement st;
    DbError err = {{0}};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT);
    CHECK(st.rows == NULL && st.row_count == 0);
    statement_free(&st);

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT);
    statement_free(&st);

    CHECK(parser_next(sql, &pos, &st, &err) == 0);
    CHECK(parser_next(sql, &pos, &st, &err) == 0); /* 끝에서 반복 호출해도 0 */
}

static void test_parse_empty_input(void) {
    Statement st;
    DbError err = {{0}};

    CHECK(parse_one("", &st, &err) == 0);
    CHECK(parse_one(" \n\t\r\n ", &st, &err) == 0);
}

static void test_parse_comment_only(void) {
    Statement st;
    DbError err = {{0}};

    CHECK(parse_one("-- 주석만 있음", &st, &err) == 0);           /* 줄 끝 LF 없이 입력 끝 */
    CHECK(parse_one("-- a\n-- b\n", &st, &err) == 0);
    CHECK(parse_one("/* 블록 */", &st, &err) == 0);
    CHECK(parse_one("/* 여러\n줄 ; 'x' */\n -- 끝\n /**/", &st, &err) == 0);
    CHECK(parse_one("/* /* 중첩 안 함 */", &st, &err) == 0);       /* 첫 닫는 기호에서 끝남 */
}

static void test_parse_comments_around_and_inside(void) {
    const char *sql =
        "-- 앞 주석; SELECT 아님\n"
        "/* 앞 블록; */ INSERT /* 중간 */ INTO users -- 줄 끝 주석\n"
        "(name, /* 컬럼 사이 */ age) VALUES ('a', 1);-- 바로 뒤\n"
        "SELECT*/**/FROM/*x*/users;/* 뒤 */ -- 마지막";
    Statement st;
    DbError err = {{0}};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT && st.row_count == 1 && strcmp(st.rows[0].name, "a") == 0 && st.rows[0].age == 1);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    CHECK(st.kind == STMT_SELECT);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == 0);

    /* 음수는 여전히 정수 토큰, -- 만 주석이다. */
    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('a', -1);", &st, &err) == 1);
    CHECK(st.rows[0].age == -1);
    statement_free(&st);
}

static void test_parse_comment_symbols_in_string(void) {
    Statement st;
    DbError err = {{0}};

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('a--b /* c */ d', 1);", &st, &err) ==
          1);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "a--b /* c */ d") == 0);
    statement_free(&st);

    CHECK(parse_one("INSERT INTO users (name, age) VALUES ('/* 열기만', 2); -- '닫기 */'", &st,
                    &err) == 1);
    CHECK(st.row_count == 1 && strcmp(st.rows[0].name, "/* 열기만") == 0 && st.rows[0].age == 2);
    statement_free(&st);
}

static void test_parse_comment_errors(void) {
    EXPECT_PARSE_ERROR("/* 닫히지 않음", "블록 주석");
    EXPECT_PARSE_ERROR("SELECT * FROM users /* 세미콜론이 주석 안; ", "블록 주석");
    /* 중첩 미지원: 안쪽 닫는 기호에서 주석이 끝나 남은 닫는 기호가 SQL로 읽힌다. */
    EXPECT_PARSE_ERROR("/* a /* b */ */ SELECT * FROM users;", "INSERT 또는 SELECT");
    EXPECT_PARSE_ERROR("SELECT * FROM users -- 세미콜론이 주석 안;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users / 1;", "알 수 없는 문자 '/'");
}

static void expect_where(const char *sql, WhereKind kind, int64_t value) {
    Statement st;
    DbError err = {{0}};
    int r = parse_one(sql, &st, &err);

    CHECK(r == 1);
    CHECK(r == 1 && st.kind == STMT_SELECT && st.where == kind && st.where_value == value);
    if (r != 1 || st.where != kind || st.where_value != value) {
        fprintf(stderr, "    SQL: %s\n    메시지: %s\n", sql, err.msg);
    }
    if (r == 1) {
        statement_free(&st);
    }
}

static void test_parse_where(void) {
    expect_where("SELECT * FROM users;", WHERE_NONE, 0);
    expect_where("SELECT * FROM users WHERE id = 1;", WHERE_ID, 1);
    expect_where("select * from users where ID=42;", WHERE_ID, 42);
    expect_where("SELECT * FROM users WHERE age = 0;", WHERE_AGE, 0);
    expect_where("SELECT * FROM users\nWHERE\n  Age\n  =\n  150\n;", WHERE_AGE, 150);
    expect_where("SELECT * FROM users WHERE /* c */ id -- x\n = /* y */ 7;", WHERE_ID, 7);
    /* 범위 밖 값은 오류가 아니라 일치 없음으로 처리하도록 그대로 담는다. */
    expect_where("SELECT * FROM users WHERE id = -1;", WHERE_ID, -1);
    expect_where("SELECT * FROM users WHERE age = 151;", WHERE_AGE, 151);
    expect_where("SELECT * FROM users WHERE id = 9223372036854775807;", WHERE_ID, INT64_MAX);
    expect_where("SELECT * FROM users WHERE id = -9223372036854775808;", WHERE_ID, INT64_MIN);
}

static void test_parse_where_errors(void) {
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE;", "조건 컬럼 이름");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id =;", "정수 값");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id 1;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE 1 = id;", "조건 컬럼 이름");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE name = 'a';", "id 또는 age");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE email = 1;", "컬럼 'email'");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age > 1;", "'='");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age >= 1;", ">=");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age <> 1;", "<>");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age != 1;", "!=");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age = '1';", "정수 값이 필요");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = NULL;", "정수 값이 필요");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1.5;", "알 수 없는 문자");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 AND age = 2;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 or id = 2;", "AND/OR");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 = 2;", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 9223372036854775808;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = -9223372036854775809;", "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE age = 123456789012345678901234567890123;",
                       "64비트");
    EXPECT_PARSE_ERROR("SELECT * FROM users WHERE id = 1 WHERE age = 2;", "세미콜론");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1) WHERE id = 1;", "세미콜론");
}

static void test_parse_next_statement_error_isolated(void) {
    /* 첫 문장은 성공해야 하고, 두 번째 문장의 잘못된 문자는 두 번째에서만 보고된다. */
    const char *sql = "SELECT * FROM users; SELECT # FROM users;";
    Statement st;
    DbError err = {{0}};
    size_t pos = 0;

    CHECK(parser_next(sql, &pos, &st, &err) == 1);
    statement_free(&st);
    CHECK(parser_next(sql, &pos, &st, &err) == -1);
    CHECK(strstr(err.msg, "알 수 없는 문자") != NULL);
}

static void test_parse_peek_kind(void) {
    StmtKind kind = STMT_SELECT;

    CHECK(parser_peek_kind("INSERT INTO users (name, age) VALUES ('a', 1);", &kind) == 1);
    CHECK(kind == STMT_INSERT);
    CHECK(parser_peek_kind("  -- c\n /* x */ iNsErT broken", &kind) == 1);
    CHECK(kind == STMT_INSERT);
    CHECK(parser_peek_kind("\tselect * FROM users;", &kind) == 1);
    CHECK(kind == STMT_SELECT);
    kind = STMT_INSERT;
    CHECK(parser_peek_kind("SELECT", &kind) == 1);
    CHECK(kind == STMT_SELECT);

    kind = STMT_INSERT;
    CHECK(parser_peek_kind("", &kind) == 0);
    CHECK(parser_peek_kind("   -- only a comment", &kind) == 0);
    CHECK(parser_peek_kind("/* unclosed INSERT", &kind) == 0);
    CHECK(parser_peek_kind("INSERTX INTO users", &kind) == 0);
    CHECK(parser_peek_kind("UPDATE users SET age = 1;", &kind) == 0);
    CHECK(parser_peek_kind("(INSERT", &kind) == 0);
    CHECK(parser_peek_kind("인 INSERT", &kind) == 0);
    CHECK(kind == STMT_INSERT); /* 0이면 *out을 바꾸지 않는다 */
}

static void test_parse_errors(void) {
    EXPECT_PARSE_ERROR("SELECT * FROM users", "세미콜론");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1)", "세미콜론");
    EXPECT_PARSE_ERROR("SELECT * FROM users SELECT * FROM users;", "세미콜론");
    EXPECT_PARSE_ERROR(";", "INSERT 또는 SELECT");
    EXPECT_PARSE_ERROR("UPDATE users SET age = 1;", "INSERT 또는 SELECT");
    EXPECT_PARSE_ERROR("SELECT * FROM orders;", "테이블 'orders'");
    EXPECT_PARSE_ERROR("SELECT name FROM users;", "SELECT *");
    EXPECT_PARSE_ERROR("SELECT * users;", "FROM");
    EXPECT_PARSE_ERROR("INSERT users (name, age) VALUES ('a', 1);", "INTO");
    EXPECT_PARSE_ERROR("INSERT INTO users name, age VALUES ('a', 1);", "'('");
    EXPECT_PARSE_ERROR("INSERT INTO users () VALUES ();", "컬럼 이름");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, email) VALUES ('a', 'b');", "컬럼 'email'");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, NAME) VALUES ('a', 'b');", "중복");
    EXPECT_PARSE_ERROR("INSERT INTO users (name) VALUES ('a');", "age 컬럼");
    EXPECT_PARSE_ERROR("INSERT INTO users (age) VALUES (1);", "name 컬럼");
    EXPECT_PARSE_ERROR("INSERT INTO users (id, name, age) VALUES (1, 'a', 1);", "자동");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) ('a', 1);", "VALUES");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES (1, 1);", "문자열");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 'b');", "정수");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', NULL);", "정수");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a');", "적습니다");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1, 2);", "많습니다");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a' 1);", "',' 또는 ')'");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('abc, 1);", "닫히지");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 99999999999999999999);",
                       "범위");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 2147483648);", "범위");
    EXPECT_PARSE_ERROR("INSERT INTO users (name, age) VALUES ('a', 1.5);", "알 수 없는 문자");
}

/* ---------- Storage ---------- */

static void test_storage_empty_then_append(void) {
    const char *dir = fresh_dir("empty");
    Table *t = NULL;
    DbError err = {{0}};
    Collected c;
    int64_t id = 0;

    memset(&c, 0, sizeof c);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 0);
    CHECK(table_append(t, "kim", 1, &id, &err) == 0);
    CHECK(id == 1);
    CHECK(table_close(t, &err) == 0);
    CHECK(table_close(NULL, &err) == 0);
}

static void test_storage_persist_and_restore_id(void) {
    const char *dir = fresh_dir("persist");
    Table *t = NULL;
    DbError err = {{0}};
    Collected c;
    int64_t id = 0;
    char buf[256];

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "민수", 25, &id, &err) == 0 && id == 1);
    CHECK(table_append(t, "O'Brien", 30, &id, &err) == 0 && id == 2);
    CHECK(table_close(t, &err) == 0);

    /* 다시 열면 같은 레코드가 같은 순서로 보이고, 다음 ID는 3이다. */
    memset(&c, 0, sizeof c);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 2);
    CHECK(c.ids[0] == 1 && strcmp(c.names[0], "민수") == 0 && c.ages[0] == 25);
    CHECK(c.ids[1] == 2 && strcmp(c.names[1], "O'Brien") == 0 && c.ages[1] == 30);
    CHECK(table_append(t, "a;b", 0, &id, &err) == 0 && id == 3);

    /* 같은 핸들로 추가 직후 다시 조회해도 새 레코드가 보인다. */
    memset(&c, 0, sizeof c);
    CHECK(table_scan(t, collect_row, &c, &err) == 0);
    CHECK(c.count == 3 && c.ids[2] == 3 && strcmp(c.names[2], "a;b") == 0);
    CHECK(table_close(t, &err) == 0);

    read_file(dir, buf, sizeof buf);
    CHECK(strcmp(buf, "1\t민수\t25\n2\tO'Brien\t30\n3\ta;b\t0\n") == 0);
}

static void expect_corrupt(const char *data, size_t len, const char *needle) {
    const char *dir = fresh_dir("corrupt");
    Table *t = NULL;
    DbError err = {{0}};

    write_file(dir, data, len);
    CHECK(table_open(dir, &t, &err) == -1);
    CHECK(t == NULL);
    CHECK(strstr(err.msg, needle) != NULL);
    if (strstr(err.msg, needle) == NULL) {
        fprintf(stderr, "    메시지: %s\n    기대: %s\n", err.msg, needle);
    }
    table_close(t, &err);
}

#define CORRUPT(lit, needle) expect_corrupt((lit), sizeof(lit) - 1, (needle))

static void test_storage_corrupt_files(void) {
    char long_line[400];

    CORRUPT("1\tkim\t25", "개행");
    CORRUPT("1\tkim\n", "필드");
    CORRUPT("1\tkim\t25\textra\n", "필드");
    CORRUPT("2\ta\t1\n1\tb\t2\n", "증가");
    CORRUPT("1\ta\t1\n1\tb\t2\n", "증가");
    CORRUPT("0\ta\t1\n", "양의 정수");
    CORRUPT("01\ta\t1\n", "양의 정수");
    CORRUPT("x\ta\t1\n", "양의 정수");
    CORRUPT("-1\ta\t1\n", "양의 정수");
    CORRUPT("99999999999999999999\ta\t1\n", "양의 정수");
    CORRUPT("9223372036854775808\ta\t1\n", "양의 정수");
    CORRUPT("1\t\t1\n", "name");
    CORRUPT("1\ta\r\t1\n", "name");
    CORRUPT("1\ta\t151\n", "age");
    CORRUPT("1\ta\t-1\n", "age");
    CORRUPT("1\ta\t1\r\n", "age");
    CORRUPT("1\ta\t\n", "age");
    CORRUPT("1\ta\t1\n\n", "필드");
    CORRUPT("1\ta\0b\t1\n", "NUL");
    CORRUPT("1\tok\t1\n2\tbad\t200\n", "2번째 줄");

    memset(long_line, 'a', sizeof long_line);
    memcpy(long_line, "1\t", 2);
    expect_corrupt(long_line, sizeof long_line, "너무 깁니다");
}

static void test_storage_max_id(void) {
    const char *dir = fresh_dir("maxid");
    const char data[] = "9223372036854775807\ta\t1\n";
    Table *t = NULL;
    DbError err = {{0}};
    int64_t id = 0;
    Collected c = {0};

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_find_id(t, INT64_MAX, collect_row, &c, &err) == 0);
    CHECK(c.count == 1 && c.ids[0] == INT64_MAX);
    CHECK(table_append(t, "b", 1, &id, &err) == -1);
    CHECK(strstr(err.msg, "ID") != NULL);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_rejects_bad_record(void) {
    const char *dir = fresh_dir("badrec");
    Table *t = NULL;
    DbError err = {{0}};
    char buf[64];
    int64_t id = 0;

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "a\tb", 1, &id, &err) == -1);
    CHECK(table_append(t, "", 1, &id, &err) == -1);
    CHECK(table_append(t, "a", 151, &id, &err) == -1);
    CHECK(table_close(t, &err) == 0);
    CHECK(read_file(dir, buf, sizeof buf) == 0);
}

static void test_storage_callback_stop(void) {
    const char *dir = fresh_dir("stop");
    const char data[] = "1\ta\t1\n2\tb\t2\n3\tc\t3\n";
    Table *t = NULL;
    DbError err = {{0}};
    Collected c;

    write_file(dir, data, sizeof data - 1);
    memset(&c, 0, sizeof c);
    c.stop_after = 2;
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_scan(t, collect_row, &c, &err) == -1);
    CHECK(c.count == 2);
    CHECK(table_close(t, &err) == 0);
}

static void test_storage_open_failure(void) {
    char path[512];
    FILE *fp;
    Table *t = NULL;
    DbError err = {{0}};

    /* 데이터 디렉터리 자리에 일반 파일이 있으면 열 수 없다. */
    snprintf(path, sizeof path, "%s/not_a_dir", g_base);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fclose(fp);
    }
    CHECK(table_open(path, &t, &err) == -1);
    CHECK(t == NULL);
    CHECK(strstr(err.msg, "열 수 없습니다") != NULL);
}

/* ---------- Executor ---------- */

static int count_insert(int64_t id, void *ctx) {
    int64_t *last = ctx;
    *last = id;
    return 0;
}

static void expect_exec_error(Table *t, const char *name, int age, const char *needle) {
    char buf[300];
    InsertRow row;
    Statement st = {STMT_INSERT, &row, 1, WHERE_NONE, 0};
    DbError err = {{0}};

    snprintf(buf, sizeof buf, "%s", name);
    row.name = buf;
    row.age = age;
    CHECK(executor_run(t, &st, NULL, &err) == -1);
    CHECK(strstr(err.msg, needle) != NULL);
    if (strstr(err.msg, needle) == NULL) {
        fprintf(stderr, "    메시지: %s\n    기대: %s\n", err.msg, needle);
    }
}

static void test_executor_validation(void) {
    const char *dir = fresh_dir("exec_valid");
    Table *t = NULL;
    DbError err = {{0}};
    char name255[256];
    char name256[257];
    char buf[64];

    memset(name255, 'a', 255);
    name255[255] = '\0';
    memset(name256, 'a', 256);
    name256[256] = '\0';

    CHECK(table_open(dir, &t, &err) == 0);
    expect_exec_error(t, "", 1, "빈 문자열");
    expect_exec_error(t, name256, 1, "255바이트");
    expect_exec_error(t, "a\tb", 1, "탭");
    expect_exec_error(t, "a\nb", 1, "줄바꿈");
    expect_exec_error(t, "a\rb", 1, "줄바꿈");
    expect_exec_error(t, "a", -1, "범위");
    expect_exec_error(t, "a", 151, "범위");
    CHECK(table_close(t, &err) == 0);
    /* 실패한 INSERT는 하나도 기록되지 않는다. */
    CHECK(read_file(dir, buf, sizeof buf) == 0);

    /* 경계값은 성공한다. */
    {
        InsertRow row = {name255, 0};
        Statement st = {STMT_INSERT, &row, 1, WHERE_NONE, 0};
        int64_t last = 0;
        ResultSink sink = {count_insert, NULL, &last};

        CHECK(table_open(dir, &t, &err) == 0);
        CHECK(executor_run(t, &st, &sink, &err) == 0 && last == 1);
        row.age = 150;
        CHECK(executor_run(t, &st, &sink, &err) == 0 && last == 2);
        CHECK(table_close(t, &err) == 0);
    }
}

static void test_executor_insert_select(void) {
    const char *dir = fresh_dir("exec_flow");
    Table *t = NULL;
    DbError err = {{0}};
    Collected c;
    int64_t last = 0;
    char kim[] = "kim";
    char lee[] = "lee";
    InsertRow row = {kim, 20};
    Statement ins = {STMT_INSERT, &row, 1, WHERE_NONE, 0};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0};
    ResultSink sink;

    memset(&c, 0, sizeof c);
    sink.on_insert = count_insert;
    sink.on_row = collect_row;
    sink.ctx = &last;

    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(executor_run(t, &ins, &sink, &err) == 0 && last == 1);
    row.name = lee;
    row.age = 21;
    CHECK(executor_run(t, &ins, &sink, &err) == 0 && last == 2);

    sink.ctx = &c;
    CHECK(executor_run(t, &sel, &sink, &err) == 0);
    CHECK(c.count == 2);
    CHECK(c.ids[0] == 1 && strcmp(c.names[0], "kim") == 0 && c.ages[0] == 20);
    CHECK(c.ids[1] == 2 && strcmp(c.names[1], "lee") == 0 && c.ages[1] == 21);
    CHECK(executor_run(t, &sel, NULL, &err) == 0); /* sink 없이도 동작 */
    CHECK(table_close(t, &err) == 0);
}

/* WHERE 조건으로 조회한 id 목록을 ids에 담고 개수를 돌려준다. */
static int select_where(Table *t, WhereKind kind, int64_t value, int64_t *ids) {
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_NONE, 0};
    ResultSink sink = {NULL, collect_row, NULL};
    DbError err = {{0}};
    Collected c;
    int i;

    memset(&c, 0, sizeof c);
    sel.where = kind;
    sel.where_value = value;
    sink.ctx = &c;
    CHECK(executor_run(t, &sel, &sink, &err) == 0);
    for (i = 0; i < c.count && i < 8; i++) {
        ids[i] = c.ids[i];
    }
    return c.count;
}

static void test_executor_where(void) {
    const char *dir = fresh_dir("exec_where");
    const char data[] = "1\ta\t25\n2\tb\t30\n3\tc\t25\n5\td\t40\n6\te\t25\n";
    Table *t = NULL;
    DbError err = {{0}};
    int64_t ids[8];

    write_file(dir, data, sizeof data - 1); /* id 4가 빠진 파일(증가 순서만 요구) */
    CHECK(table_open(dir, &t, &err) == 0);

    /* id 조건: 한 건, 없음(빈 id·범위 밖) */
    CHECK(select_where(t, WHERE_ID, 1, ids) == 1 && ids[0] == 1);
    CHECK(select_where(t, WHERE_ID, 6, ids) == 1 && ids[0] == 6);
    CHECK(select_where(t, WHERE_ID, 4, ids) == 0);
    CHECK(select_where(t, WHERE_ID, 0, ids) == 0);
    CHECK(select_where(t, WHERE_ID, -1, ids) == 0);
    CHECK(select_where(t, WHERE_ID, INT64_MAX, ids) == 0);

    /* age 조건: 여러 건은 저장 순서대로, 한 건, 없음 */
    CHECK(select_where(t, WHERE_AGE, 25, ids) == 3 && ids[0] == 1 && ids[1] == 3 && ids[2] == 6);
    CHECK(select_where(t, WHERE_AGE, 40, ids) == 1 && ids[0] == 5);
    CHECK(select_where(t, WHERE_AGE, 0, ids) == 0);
    CHECK(select_where(t, WHERE_AGE, 151, ids) == 0);
    /* int 범위를 넘는 값이 잘려서 25와 같아지지 않아야 한다 */
    CHECK(select_where(t, WHERE_AGE, 25 + ((int64_t)1 << 32), ids) == 0);

    /* 조건 없음은 전체 */
    CHECK(select_where(t, WHERE_NONE, 0, ids) == 5);
    CHECK(table_close(t, &err) == 0);
}

static void test_executor_select_paths(void) {
    /* 뒤에 손상된 행이 생긴 경우, ID 조회는 대상 행만 읽고 age 조회는 끝까지 읽는다. */
    const char *dir = fresh_dir("exec_where_corrupt");
    const char good[] = "1\ta\t25\n";
    const char bad[] = "1\ta\t25\n2\tb\n";
    Table *t = NULL;
    DbError err = {{0}};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_AGE, 25};
    Collected c = {0};
    char path[600];
    FILE *fp;

    write_file(dir, good, sizeof good - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    /* 열린 뒤 다른 경로로 손상된 줄이 덧붙은 상황 */
    snprintf(path, sizeof path, "%s/users.tsv", dir);
    fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fwrite(bad, 1, sizeof bad - 1, fp);
        fclose(fp);
    }
    CHECK(select_where(t, WHERE_ID, 1, c.ids) == 1 && c.ids[0] == 1);
    c.stop_after = 1;
    CHECK(table_find_id(t, 1, collect_row, &c, &err) == -1 && c.count == 1);
    CHECK(strstr(err.msg, "조회 결과 처리") != NULL);
    CHECK(executor_run(t, &sel, NULL, &err) == -1);
    CHECK(strstr(err.msg, "2번째 줄") != NULL);
    CHECK(table_close(t, &err) == 0);
}

static void test_index_splits_and_reopen(void) {
    /* 여러 리프와 내부 노드가 분할될 만큼 삽입한다. */
    enum { ROWS = 6000 };
    const char *dir = fresh_dir("index_splits");
    Table *t = NULL;
    DbError err = {{0}};
    int64_t id;
    int64_t ids[8];

    CHECK(table_open(dir, &t, &err) == 0);
    for (int i = 1; i <= ROWS; i++) {
        CHECK(table_append(t, i % 2 == 0 ? "김민수" : "abc", i % 151, &id, &err) == 0 && id == i);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = ROWS; i > 0; i--) {
            Collected c;
            c.count = 0;
            c.stop_after = 0;
            CHECK(table_find_id(t, i, collect_row, &c, &err) == 0);
            CHECK(c.count == 1 && c.ids[0] == i && c.ages[0] == i % 151 &&
                  strcmp(c.names[0], i % 2 == 0 ? "김민수" : "abc") == 0);
        }
        CHECK(table_close(t, &err) == 0);
        CHECK(table_open(dir, &t, &err) == 0);
    }
    CHECK(table_append(t, "after reopen", 20, &id, &err) == 0 && id == ROWS + 1);
    CHECK(select_where(t, WHERE_ID, ROWS + 1, ids) == 1 && ids[0] == ROWS + 1);
    CHECK(table_close(t, &err) == 0);
}

/* ---------- TableReader ---------- */

static void test_reader_shares_index(void) {
    const char *dir = fresh_dir("reader_shared");
    const char data[] = "1\ta\t25\n2\tb\t30\n";
    Table *t = NULL;
    TableReader *r1 = NULL;
    TableReader *r2 = NULL;
    DbError err = {{0}};
    Collected c;
    Collected all;
    int64_t id = 0;

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_reader_open(t, &r1, &err) == 0);
    CHECK(table_reader_open(t, &r2, &err) == 0);

    /* 두 핸들을 번갈아 써도 서로의 파일 위치에 영향을 주지 않는다. */
    memset(&c, 0, sizeof c);
    CHECK(table_reader_find_id(r1, 2, collect_row, &c, &err) == 0 && c.count == 1 && c.ids[0] == 2);
    CHECK(table_reader_find_id(r2, 1, collect_row, &c, &err) == 0 && c.count == 2 && c.ids[1] == 1);
    CHECK(table_reader_find_id(r1, 3, collect_row, &c, &err) == 0 && c.count == 2); /* 없는 ID */

    /* 끝까지 읽어 EOF 상태가 된 핸들도, 뒤에 추가된 행을 다시 읽는다. */
    memset(&c, 0, sizeof c);
    CHECK(table_reader_scan(r2, collect_row, &c, &err) == 0 && c.count == 2);
    CHECK(table_append(t, "kim", 40, &id, &err) == 0 && id == 3);
    memset(&c, 0, sizeof c);
    CHECK(table_reader_find_id(r1, 3, collect_row, &c, &err) == 0 && c.count == 1 &&
          strcmp(c.names[0], "kim") == 0 && c.ages[0] == 40);
    memset(&c, 0, sizeof c);
    CHECK(table_reader_scan(r2, collect_row, &c, &err) == 0 && c.count == 3 && c.ids[2] == 3);

    /* reader 스캔 결과는 Table 스캔 결과와 같다. */
    memset(&all, 0, sizeof all);
    CHECK(table_scan(t, collect_row, &all, &err) == 0 && all.count == c.count &&
          memcmp(all.ids, c.ids, sizeof c.ids) == 0);

    CHECK(table_reader_close(r1, &err) == 0);
    CHECK(table_reader_close(r2, &err) == 0);
    CHECK(table_reader_close(NULL, &err) == 0);
    CHECK(table_close(t, &err) == 0);
}

static void test_reader_executor(void) {
    const char *dir = fresh_dir("reader_exec");
    const char data[] = "1\ta\t25\n2\tb\t30\n3\tc\t25\n";
    char kim[] = "kim";
    Table *t = NULL;
    TableReader *r = NULL;
    DbError err = {{0}};
    InsertRow row = {kim, 20};
    Statement sel = {STMT_SELECT, NULL, 0, WHERE_AGE, 25};
    Statement ins = {STMT_INSERT, &row, 1, WHERE_NONE, 0};
    ResultSink sink = {NULL, collect_row, NULL};
    Collected c;

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_reader_open(t, &r, &err) == 0);
    sink.ctx = &c;

    memset(&c, 0, sizeof c);
    CHECK(executor_run_read(r, &sel, &sink, &err) == 0 && c.count == 2 && c.ids[0] == 1 && c.ids[1] == 3);
    sel.where = WHERE_ID;
    sel.where_value = 2;
    memset(&c, 0, sizeof c);
    CHECK(executor_run_read(r, &sel, &sink, &err) == 0 && c.count == 1 && c.ids[0] == 2);
    sel.where = WHERE_NONE;
    memset(&c, 0, sizeof c);
    CHECK(executor_run_read(r, &sel, &sink, &err) == 0 && c.count == 3);

    /* 읽기 핸들로는 INSERT를 실행하지 않는다. 파일도 바뀌지 않는다. */
    CHECK(executor_run_read(r, &ins, &sink, &err) == -1);
    CHECK(strstr(err.msg, "SELECT만") != NULL);
    memset(&c, 0, sizeof c);
    CHECK(table_scan(t, collect_row, &c, &err) == 0 && c.count == 3);

    CHECK(table_reader_close(r, &err) == 0);
    CHECK(table_close(t, &err) == 0);
}

#ifndef _WIN32
static void test_reader_after_write_failure(void) {
    /* 파일 크기 제한으로 쓰기를 실패시키면, 같은 Table의 reader도 조회를 거부한다. */
    const char *dir = fresh_dir("reader_write_failed");
    const char data[] = "1\ta\t1\n";
    Table *t = NULL;
    TableReader *r = NULL;
    DbError err = {{0}};
    struct rlimit previous;
    struct rlimit limited;
    void (*old_handler)(int);
    int64_t id = 0;
    int appended;

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_reader_open(t, &r, &err) == 0);
    CHECK(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    old_handler = signal(SIGXFSZ, SIG_IGN);
    limited = previous;
    limited.rlim_cur = sizeof data - 1;
    CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    appended = table_append(t, "b", 2, &id, &err);
    CHECK(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    signal(SIGXFSZ, old_handler);

    CHECK(appended == -1);
    CHECK(table_reader_find_id(r, 1, NULL, NULL, &err) == -1);
    CHECK(strstr(err.msg, "쓰기에 실패") != NULL);
    CHECK(table_reader_scan(r, NULL, NULL, &err) == -1);
    CHECK(table_reader_close(r, &err) == 0);
    (void)table_close(t, &err);
}
#endif

/* ---------- 다중 행 INSERT ---------- */

#define INS_PREFIX "INSERT INTO users (name, age) VALUES "

static void test_parse_multi_row(void) {
    Statement st;
    DbError err = {{0}};

    /* 컬럼 순서 (age, name)이 모든 행에 적용된다. */
    CHECK(parse_one("insert into users (age, name) values (1, 'a'), (2, 'O''Brien') ,\n(3,'c');",
                    &st, &err) == 1);
    CHECK(st.kind == STMT_INSERT && st.row_count == 3);
    if (st.row_count == 3) {
        CHECK(strcmp(st.rows[0].name, "a") == 0 && st.rows[0].age == 1);
        CHECK(strcmp(st.rows[1].name, "O'Brien") == 0 && st.rows[1].age == 2);
        CHECK(strcmp(st.rows[2].name, "c") == 0 && st.rows[2].age == 3);
    }
    statement_free(&st);
    CHECK(st.rows == NULL && st.row_count == 0);
    statement_free(&st); /* 두 번 호출해도 안전 */
}

static void test_parse_multi_row_errors(void) {
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1),;", "2번째 행: 구문 오류: 여는 괄호");
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1), ('b');", "2번째 행: 값이 컬럼보다 적습니다");
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1), ('b', 2, 3);", "2번째 행: 값이 컬럼보다 많습니다");
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1), ('b', 2), (3, 'c');", "3번째 행: name에는");
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1) ('b', 2);", "세미콜론");
    EXPECT_PARSE_ERROR(INS_PREFIX ";", "여는 괄호");
    /* 쉼표 바로 뒤 토큰의 lexer 오류에도 행 번호가 붙는다. */
    EXPECT_PARSE_ERROR(INS_PREFIX "('a', 1), 'abc", "2번째 행: 문자열이 닫히지");
    /* 첫 행의 오류 메시지는 한 행 문장과 같다(행 번호 없음). */
    EXPECT_PARSE_ERROR(INS_PREFIX "('a'), ('b', 2);", "값이 컬럼보다 적습니다");
}

/* 잘린 오류 메시지 끝에 미완성 UTF-8 문자가 남지 않는다. */
static void test_error_utf8_truncation(void) {
    char long_msg[400];
    DbError err;
    size_t len;
    int i;

    long_msg[0] = 'a';
    for (i = 0; i < 100; i++) {
        memcpy(long_msg + 1 + i * 3, "\xEA\xB0\x80", 3); /* '가' */
    }
    long_msg[301] = '\0';
    db_error_set(&err, "%s", long_msg);
    len = strlen(err.msg);
    /* 255바이트 중 'a' 1 + 완성된 '가' 84개(252) = 253. 남은 2바이트는 지운다. */
    CHECK(len == 253);
    CHECK(memcmp(err.msg, long_msg, len) == 0);

    db_error_set(&err, "%s", "짧은 메시지");
    CHECK(strcmp(err.msg, "짧은 메시지") == 0);
}

/* rows개의 ('a', 1) 행으로 된 INSERT 문장. 호출자가 free한다. */
static char *multi_row_sql(int rows) {
    size_t cap = sizeof INS_PREFIX + (size_t)rows * 10 + 2;
    char *sql = malloc(cap);
    size_t n;
    int i;

    if (sql == NULL) {
        fprintf(stderr, "메모리 부족\n");
        exit(2);
    }
    n = (size_t)snprintf(sql, cap, "%s", INS_PREFIX);
    for (i = 0; i < rows; i++) {
        n += (size_t)snprintf(sql + n, cap - n, "%s('a', 1)", i == 0 ? "" : ",");
    }
    snprintf(sql + n, cap - n, ";");
    return sql;
}

static void test_parse_row_limit(void) {
    Statement st;
    DbError err = {{0}};
    char *sql = multi_row_sql(INSERT_MAX_ROWS);

    CHECK(parse_one(sql, &st, &err) == 1 && st.row_count == INSERT_MAX_ROWS);
    statement_free(&st);
    free(sql);

    sql = multi_row_sql(INSERT_MAX_ROWS + 1);
    CHECK(parse_one(sql, &st, &err) == -1);
    CHECK(strstr(err.msg, "최대 1000개") != NULL);
    free(sql);
}

typedef struct {
    int count;
    int64_t ids[8];
} InsertedIds;

static int record_ids(int64_t id, void *ctx) {
    InsertedIds *c = ctx;

    if (c->count < 8) {
        c->ids[c->count] = id;
    }
    c->count++;
    return 0;
}

static void test_executor_multi_row(void) {
    const char *dir = fresh_dir("exec_multi");
    char a[] = "a";
    char b[] = "b";
    char c[] = "c";
    InsertRow rows[3] = {{a, 1}, {b, 2}, {c, 151}};
    Statement st = {STMT_INSERT, rows, 3, WHERE_NONE, 0};
    InsertedIds ids;
    ResultSink sink = {record_ids, NULL, &ids};
    Table *t = NULL;
    DbError err = {{0}};
    Collected got;
    char buf[64];

    CHECK(table_open(dir, &t, &err) == 0);
    /* 마지막 행의 값이 틀리면 앞의 행도 기록하지 않는다. */
    memset(&ids, 0, sizeof ids);
    CHECK(executor_run(t, &st, &sink, &err) == -1);
    CHECK(strstr(err.msg, "3번째 행: age 값 151") != NULL);
    CHECK(ids.count == 0);
    CHECK(read_file(dir, buf, sizeof buf) == 0);

    /* 성공하면 ID가 연속이고 콜백은 행 순서대로 호출된다. */
    rows[2].age = 3;
    CHECK(executor_run(t, &st, &sink, &err) == 0);
    CHECK(ids.count == 3 && ids.ids[0] == 1 && ids.ids[1] == 2 && ids.ids[2] == 3);
    memset(&got, 0, sizeof got);
    CHECK(table_scan(t, collect_row, &got, &err) == 0 && got.count == 3);
    CHECK(strcmp(got.names[0], "a") == 0 && strcmp(got.names[2], "c") == 0 && got.ages[2] == 3);
    CHECK(table_close(t, &err) == 0);
}

#ifndef _WIN32
static void test_executor_multi_row_partial_write(void) {
    /* 파일 크기 제한으로 두 번째 행부터 쓰기를 실패시킨다. 첫 행은 기록된 채 남는다. */
    const char *dir = fresh_dir("exec_multi_partial");
    const char data[] = "1\ta\t1\n";
    const char first_line[] = "2\tb\t2\n";
    char b[] = "b";
    char c[] = "c";
    InsertRow rows[2] = {{b, 2}, {c, 3}};
    Statement st = {STMT_INSERT, rows, 2, WHERE_NONE, 0};
    InsertedIds ids;
    ResultSink sink = {record_ids, NULL, &ids};
    Table *t = NULL;
    DbError err = {{0}};
    struct rlimit previous;
    struct rlimit limited;
    void (*old_handler)(int);
    char buf[64];
    int rc;

    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    memset(&ids, 0, sizeof ids);
    CHECK(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    old_handler = signal(SIGXFSZ, SIG_IGN);
    limited = previous;
    limited.rlim_cur = (sizeof data - 1) + (sizeof first_line - 1);
    CHECK(setrlimit(RLIMIT_FSIZE, &limited) == 0);
    rc = executor_run(t, &st, &sink, &err);
    CHECK(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    signal(SIGXFSZ, old_handler);

    CHECK(rc == -1);
    CHECK(strstr(err.msg, "앞의 1행은 기록됨") != NULL);
    CHECK(ids.count == 1 && ids.ids[0] == 2);
    /* 닫기 전 디스크 상태. 닫을 때 stdio 버퍼에 남은 실패 행이 다시 쓰일 수 있으므로
     * (기존 한 행 INSERT와 같은 '상태 알 수 없음') 닫은 뒤의 내용은 검사하지 않는다. */
    CHECK(read_file(dir, buf, sizeof buf) == (sizeof data - 1) + (sizeof first_line - 1));
    CHECK(strcmp(buf, "1\ta\t1\n2\tb\t2\n") == 0);
    (void)table_close(t, &err);
}

/* 파일 크기 제한을 limit_bytes로 두고 rows를 묶음으로 쓴다. SIGXFSZ는 무시한다. */
static int append_rows_limited(Table *t, const AppendRow *rows, size_t n, size_t limit_bytes,
                               int64_t *first_id, size_t *stored, DbError *err) {
    struct rlimit previous;
    struct rlimit limited;
    void (*old_handler)(int);
    int rc;

    if (getrlimit(RLIMIT_FSIZE, &previous) != 0) {
        return -2;
    }
    old_handler = signal(SIGXFSZ, SIG_IGN);
    limited = previous;
    limited.rlim_cur = limit_bytes;
    if (setrlimit(RLIMIT_FSIZE, &limited) != 0) {
        signal(SIGXFSZ, old_handler);
        return -2;
    }
    rc = table_append_rows(t, rows, n, first_id, stored, err);
    CHECK(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    signal(SIGXFSZ, old_handler);
    return rc;
}

static void test_append_rows_torn_write(void) {
    /* 묶음 쓰기가 줄 중간에서 끊기면 끝까지 기록된 줄만 센다. */
    const char data[] = "1\ta\t1\n"; /* 6바이트. 이후 행도 모두 6바이트 */
    const AppendRow rows[3] = {{"b", 2}, {"c", 3}, {"d", 4}};
    struct {
        const char *name;
        size_t limit;    /* 기존 6바이트 뒤로 허용할 바이트 */
        size_t stored;
    } cases[] = {
        {"torn_mid_second", 6 + 3, 1},  /* b 완전, c 절반 */
        {"torn_first", 3, 0},           /* b부터 절반 */
        {"torn_on_boundary", 12, 2},    /* b, c 완전, d 0바이트 */
    };

    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const char *dir = fresh_dir(cases[k].name);
        Table *t = NULL;
        DbError err = {{0}};
        int64_t first_id = -1;
        size_t stored = 99;
        int rc;

        write_file(dir, data, sizeof data - 1);
        CHECK(table_open(dir, &t, &err) == 0);
        rc = append_rows_limited(t, rows, 3, (sizeof data - 1) + cases[k].limit,
                                 &first_id, &stored, &err);
        CHECK(rc == -1);
        CHECK(stored == cases[k].stored);
        CHECK(stored == 0 ? first_id == 0 : first_id == 2);
        CHECK(strstr(err.msg, "데이터 파일 쓰기 실패") != NULL);
        /* k의 근거: 실패 직후(닫기 전) 파일은 제한 크기까지 기록되어 있다. */
        {
            char buf[64];
            CHECK(read_file(dir, buf, sizeof buf) == (sizeof data - 1) + cases[k].limit);
        }
        /* 쓰기 실패 뒤에는 Table을 쓸 수 없다. */
        CHECK(table_append(t, "e", 5, NULL, &err) == -1);
        (void)table_close(t, &err);
    }
}

static void test_append_rows_torn_large_write(void) {
    /* stdio 버퍼(4KB)보다 큰 묶음: fwrite가 직접 쓰다가 짧게 반환하는 경로. */
    enum { ROWS = 40, CUT_AFTER = 17 };
    static char name[NAME_MAX_BYTES + 1];
    static char buf[ROWS * 300 + 64];
    const char *dir = fresh_dir("torn_large");
    const char data[] = "1\ta\t1\n";
    AppendRow rows[ROWS];
    Table *t = NULL;
    DbError err = {{0}};
    int64_t first_id = -1;
    size_t stored = 99;
    size_t limit = sizeof data - 1;
    int rc;

    memset(name, 'x', NAME_MAX_BYTES);
    name[NAME_MAX_BYTES] = '\0';
    for (int i = 0; i < ROWS; i++) {
        rows[i].name = name;
        rows[i].age = 100 + i;
        /* 앞의 CUT_AFTER행은 끝까지, 그다음 행은 절반만 허용한다. */
        if (i < CUT_AFTER) {
            limit += (size_t)snprintf(NULL, 0, "%d\t%s\t%d\n", 2 + i, name, 100 + i);
        } else if (i == CUT_AFTER) {
            limit += 100;
        }
    }
    write_file(dir, data, sizeof data - 1);
    CHECK(table_open(dir, &t, &err) == 0);
    rc = append_rows_limited(t, rows, ROWS, limit, &first_id, &stored, &err);
    CHECK(rc == -1);
    CHECK(stored == CUT_AFTER && first_id == 2);
    CHECK(read_file(dir, buf, sizeof buf) == limit);
    (void)table_close(t, &err);
}
#endif

/* 묶음 쓰기에서 계산한 오프셋으로 모든 ID를 찾을 수 있다. 한 행 추가와 섞어도 이어진다. */
static void test_append_rows_offsets(void) {
    enum { BATCH = 1000 };
    static AppendRow rows[BATCH];
    const char *dir = fresh_dir("append_rows");
    const char *names[3] = {"김민수", "abc", "이름이 조금 더 긴 사용자"};
    Table *t = NULL;
    DbError err = {{0}};
    int64_t first_id = 0;
    int64_t id = 0;
    size_t stored = 0;

    for (int i = 0; i < BATCH; i++) {
        rows[i].name = names[i % 3];
        rows[i].age = i % 151;
    }
    CHECK(table_open(dir, &t, &err) == 0);
    CHECK(table_append(t, "first", 1, &id, &err) == 0 && id == 1);
    CHECK(table_append_rows(t, rows, BATCH, &first_id, &stored, &err) == 0);
    CHECK(first_id == 2 && stored == BATCH);
    CHECK(table_append(t, "last", 2, &id, &err) == 0 && id == BATCH + 2);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < BATCH; i++) {
            Collected c;
            c.count = 0;
            c.stop_after = 0;
            CHECK(table_find_id(t, 2 + i, collect_row, &c, &err) == 0);
            CHECK(c.count == 1 && c.ids[0] == 2 + i && c.ages[0] == i % 151 &&
                  strcmp(c.names[0], names[i % 3]) == 0);
        }
        {
            Collected c;
            c.count = 0;
            c.stop_after = 0;
            CHECK(table_find_id(t, BATCH + 2, collect_row, &c, &err) == 0);
            CHECK(c.count == 1 && strcmp(c.names[0], "last") == 0);
        }
        /* 다시 열면 인덱스를 파일에서 새로 만든다. */
        CHECK(table_close(t, &err) == 0);
        CHECK(table_open(dir, &t, &err) == 0);
    }
    CHECK(table_append(t, "after reopen", 3, &id, &err) == 0 && id == BATCH + 3);

    /* 저장할 수 없는 행이 하나라도 있으면 아무것도 쓰지 않는다. */
    rows[BATCH - 1].age = 151;
    stored = 99;
    CHECK(table_append_rows(t, rows, BATCH, &first_id, &stored, &err) == -1);
    CHECK(stored == 0 && strstr(err.msg, "저장할 수 없는 레코드") != NULL);
    CHECK(table_append(t, "still usable", 4, &id, &err) == 0 && id == BATCH + 4);
    CHECK(table_close(t, &err) == 0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "사용법: test_unit <임시 디렉터리>\n");
        return 2;
    }
    g_base = argv[1];
    make_dir(g_base);

    test_parse_insert_basic();
    test_parse_column_order_and_case();
    test_parse_string_special_and_multiline();
    test_parse_select_and_sequence();
    test_parse_empty_input();
    test_parse_comment_only();
    test_parse_comments_around_and_inside();
    test_parse_comment_symbols_in_string();
    test_parse_comment_errors();
    test_parse_where();
    test_parse_where_errors();
    test_parse_next_statement_error_isolated();
    test_parse_errors();
    test_parse_peek_kind();

    test_storage_empty_then_append();
    test_storage_persist_and_restore_id();
    test_storage_corrupt_files();
    test_storage_max_id();
    test_storage_rejects_bad_record();
    test_storage_callback_stop();
    test_storage_open_failure();

    test_executor_validation();
    test_executor_insert_select();
    test_executor_where();
    test_executor_select_paths();
    test_index_splits_and_reopen();

    test_reader_shares_index();
    test_reader_executor();
#ifndef _WIN32
    test_reader_after_write_failure();
#endif

    test_parse_multi_row();
    test_parse_multi_row_errors();
    test_parse_row_limit();
    test_error_utf8_truncation();
    test_executor_multi_row();
#ifndef _WIN32
    test_executor_multi_row_partial_write();
    test_append_rows_torn_write();
    test_append_rows_torn_large_write();
#endif
    test_append_rows_offsets();

    printf("단위 테스트: %d개 검사 중 %d개 실패\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
