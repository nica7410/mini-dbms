#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "executor.h"

enum {
    ROW_COUNT = 1000000,
    TARGET_ID = ROW_COUNT / 2,
    ID_REPEATS = 100000,
    AGE_REPEATS = 10
};

static double seconds(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static int run_sql(Table *table, const char *sql, DbError *err) {
    size_t pos = 0;
    Statement st;
    int rc;

    if (parser_next(sql, &pos, &st, err) != 1) {
        return -1;
    }
    rc = executor_run(table, &st, NULL, err);
    statement_free(&st);
    return rc;
}

static int count_target(const Row *row, void *ctx) {
    int *count = ctx;

    if (row->id != TARGET_ID || row->age != 150 || strcmp(row->name, "target") != 0) {
        return -1;
    }
    (*count)++;
    return 0;
}

static int reject_existing_row(const Row *row, void *ctx) {
    (void)row;
    (void)ctx;
    return -1;
}

static int count_any(const Row *row, void *ctx) {
    (void)row;
    (*(int *)ctx)++;
    return 0;
}

enum { MULTI_ROWS = 1000 }; /* 다중 행 모드: 1,000행 문장을 ROW_COUNT / MULTI_ROWS번 */

/*
 * 다중 행 INSERT 모드. 같은 1,000행 문장을 1,000번 실행해 100만 행을 넣는다.
 * 매 문장을 다시 파싱하므로 파싱 비용도 포함한다.
 */
static int run_multi_insert(const char *dir, DbError *err) {
    static const char prefix[] = "INSERT INTO users (name, age) VALUES ";
    static const char row[] = "('other', 25)";
    size_t cap = sizeof prefix + (size_t)MULTI_ROWS * sizeof row + 2;
    char *sql = malloc(cap);
    Table *table = NULL;
    size_t n;
    double start;
    double elapsed;
    int found = 0;

    if (sql == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    n = (size_t)snprintf(sql, cap, "%s", prefix);
    for (int i = 0; i < MULTI_ROWS; i++) {
        n += (size_t)snprintf(sql + n, cap - n, "%s%s", i == 0 ? "" : ",", row);
    }
    snprintf(sql + n, cap - n, ";");

    if (table_open(dir, &table, err) != 0 ||
        table_scan(table, reject_existing_row, NULL, err) != 0) {
        free(sql);
        table_close(table, NULL);
        return -1;
    }
    start = seconds();
    for (int i = 0; i < ROW_COUNT / MULTI_ROWS; i++) {
        if (run_sql(table, sql, err) != 0) {
            free(sql);
            table_close(table, NULL);
            return -1;
        }
    }
    elapsed = seconds() - start;
    free(sql);
    /* 마지막 행이 인덱스로 찾아져야 측정이 유효하다. */
    if (table_find_id(table, ROW_COUNT, count_any, &found, err) != 0) {
        table_close(table, NULL);
        return -1;
    }
    if (found != 1) {
        db_error_set(err, "다중 행 INSERT 결과 건수가 예상과 다릅니다");
        table_close(table, NULL);
        return -1;
    }
    if (table_close(table, err) != 0) {
        return -1;
    }
    printf("SQL multi-row INSERT: %d statements x %d rows in %.3f s\n",
           ROW_COUNT / MULTI_ROWS, MULTI_ROWS, elapsed);
    return 0;
}

/* ---------- modify 모드: UPDATE·DELETE 한 문장의 시간 (계획 11) ---------- */

enum { MODIFY_RUNS = 3 };

typedef struct {
    size_t affected; /* UPDATE·DELETE의 변경 행 수 또는 SELECT 행 수 */
} ModifyCount;

static int modify_count_row(const Row *row, void *ctx) {
    (void)row;
    ((ModifyCount *)ctx)->affected++;
    return 0;
}

static int modify_record_change(size_t affected, void *ctx) {
    ((ModifyCount *)ctx)->affected = affected;
    return 0;
}

/* sql 한 문장을 실행해 걸린 시간(ms)을 *ms에 담는다. 결과 행 수가 expected와 다르면 실패. */
static int time_statement(Table *table, const char *sql, size_t expected, double *ms, DbError *err) {
    ModifyCount count = {0};
    ResultSink sink = {NULL, modify_count_row, &count, modify_record_change};
    size_t pos = 0;
    Statement st;
    double start;
    int rc;

    if (parser_next(sql, &pos, &st, err) != 1) {
        return -1;
    }
    start = seconds();
    rc = executor_run(table, &st, &sink, err);
    *ms = (seconds() - start) * 1e3;
    statement_free(&st);
    if (rc != 0) {
        return -1;
    }
    if (count.affected != expected) {
        db_error_set(err, "%s: 결과 %lu행, 기대 %lu행", sql, (unsigned long)count.affected,
                     (unsigned long)expected);
        return -1;
    }
    return 0;
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static void print_runs(const char *label, long rows, double *ms) {
    qsort(ms, MODIFY_RUNS, sizeof *ms, compare_double);
    printf("%-18s rows=%-8ld min %9.3f ms  median %9.3f ms  max %9.3f ms\n", label, rows, ms[0],
           ms[MODIFY_RUNS / 2], ms[MODIFY_RUNS - 1]);
}

/*
 * 빈 디렉터리에 rows행을 채운 뒤 UPDATE·DELETE 4종과 비교용 전체 스캔 SELECT를 MODIFY_RUNS번씩 잰다.
 * 채우는 시간은 재지 않는다. rows는 MULTI_ROWS의 배수여야 한다.
 */
static int run_modify(const char *dir, long rows, DbError *err) {
    static const char prefix[] = "INSERT INTO users (name, age) VALUES ";
    static const char row[] = "('other', 25)";
    size_t cap = sizeof prefix + (size_t)MULTI_ROWS * sizeof row + 2;
    char *sql = malloc(cap);
    char stmt[128];
    Table *table = NULL;
    double ms[MODIFY_RUNS];
    long mid = rows / 2;
    size_t n;

    if (sql == NULL) {
        db_error_set(err, "메모리가 부족합니다");
        return -1;
    }
    n = (size_t)snprintf(sql, cap, "%s", prefix);
    for (int i = 0; i < MULTI_ROWS; i++) {
        n += (size_t)snprintf(sql + n, cap - n, "%s%s", i == 0 ? "" : ",", row);
    }
    snprintf(sql + n, cap - n, ";");
    if (table_open(dir, &table, err) != 0 || table_scan(table, reject_existing_row, NULL, err) != 0) {
        goto fail;
    }
    for (long i = 0; i < rows / MULTI_ROWS; i++) {
        if (run_sql(table, sql, err) != 0) {
            goto fail;
        }
    }

    /* 한 행 변경: 매번 같은 행 */
    snprintf(stmt, sizeof stmt, "UPDATE users SET age = 1 WHERE id = %ld;", mid);
    for (int i = 0; i < MODIFY_RUNS; i++) {
        if (time_statement(table, stmt, 1, &ms[i], err) != 0) goto fail;
    }
    print_runs("update_id_hit", rows, ms);
    /* 일치 0행이지만 전체를 읽는 경우 */
    for (int i = 0; i < MODIFY_RUNS; i++) {
        if (time_statement(table, "UPDATE users SET age = 2 WHERE age = 99;", 0, &ms[i], err) != 0) goto fail;
    }
    print_runs("update_age_miss", rows, ms);
    /* 인덱스에서 끝나는 경우 */
    snprintf(stmt, sizeof stmt, "DELETE FROM users WHERE id = %ld;", rows + 100);
    for (int i = 0; i < MODIFY_RUNS; i++) {
        if (time_statement(table, stmt, 0, &ms[i], err) != 0) goto fail;
    }
    print_runs("delete_id_miss", rows, ms);
    /* 한 행 삭제: 매번 다른 행 */
    for (int i = 0; i < MODIFY_RUNS; i++) {
        snprintf(stmt, sizeof stmt, "DELETE FROM users WHERE id = %ld;", mid + 1 + i);
        if (time_statement(table, stmt, 1, &ms[i], err) != 0) goto fail;
    }
    print_runs("delete_id_hit", rows, ms);
    /* 비교 기준: 전체 스캔 SELECT 한 번 */
    for (int i = 0; i < MODIFY_RUNS; i++) {
        if (time_statement(table, "SELECT * FROM users WHERE age = 99;", 0, &ms[i], err) != 0) goto fail;
    }
    print_runs("select_scan", rows, ms);

    free(sql);
    return table_close(table, err);

fail:
    free(sql);
    table_close(table, NULL);
    return -1;
}

static int measure_select(Table *table, const char *sql, int repeats, double *us, DbError *err) {
    int count = 0;
    ResultSink sink = {NULL, count_target, &count, NULL};
    size_t pos = 0;
    Statement st;
    double start;

    if (parser_next(sql, &pos, &st, err) != 1) {
        return -1;
    }
    /* 각 쿼리 경로를 한 번 실행한 다음, 같은 열린 테이블에서 반복 측정한다. */
    if (executor_run(table, &st, &sink, err) != 0) {
        statement_free(&st);
        return -1;
    }
    start = seconds();
    for (int i = 0; i < repeats; i++) {
        if (executor_run(table, &st, &sink, err) != 0) {
            statement_free(&st);
            return -1;
        }
    }
    *us = (seconds() - start) * 1e6 / repeats;
    statement_free(&st);
    if (count != repeats + 1) {
        db_error_set(err, "SELECT 결과 건수가 예상과 다릅니다");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Table *table = NULL;
    DbError err = {{0}};
    char id_sql[80];
    double start;
    double insert_time;
    double open_time;
    double id_us;
    double age_us;

    if (argc == 3 && strcmp(argv[2], "multi") == 0) {
        if (run_multi_insert(argv[1], &err) != 0) {
            fprintf(stderr, "%s\n", err.msg);
            return 1;
        }
        return 0;
    }
    if (argc == 4 && strcmp(argv[2], "modify") == 0) {
        char *end = NULL;
        long rows = strtol(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || rows < MULTI_ROWS || rows % MULTI_ROWS != 0) {
            fprintf(stderr, "modify: rows must be a positive multiple of %d\n", MULTI_ROWS);
            return 2;
        }
        if (run_modify(argv[1], rows, &err) != 0) {
            fprintf(stderr, "%s\n", err.msg);
            return 1;
        }
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: benchmark <empty temporary data directory> [multi | modify <rows>]\n");
        return 2;
    }
    if (table_open(argv[1], &table, &err) != 0) {
        goto fail;
    }
    /* make bench는 매번 새 디렉터리를 넘긴다. 직접 실행했을 때도 기존 데이터는 보호한다. */
    if (table_scan(table, reject_existing_row, NULL, &err) != 0) {
        db_error_set(&err, "벤치마크에는 빈 데이터 디렉터리가 필요합니다");
        goto fail;
    }
    start = seconds();
    for (int i = 1; i <= ROW_COUNT; i++) {
        const char *sql = i == TARGET_ID
            ? "INSERT INTO users (name, age) VALUES ('target', 150);"
            : "INSERT INTO users (name, age) VALUES ('other', 25);";
        if (run_sql(table, sql, &err) != 0) {
            goto fail;
        }
    }
    insert_time = seconds() - start;
    if (table_close(table, &err) != 0) {
        table = NULL;
        goto fail;
    }
    start = seconds();
    if (table_open(argv[1], &table, &err) != 0) {
        goto fail;
    }
    open_time = seconds() - start;
    snprintf(id_sql, sizeof id_sql, "SELECT * FROM users WHERE id = %d;", TARGET_ID);
    if (measure_select(table, id_sql, ID_REPEATS, &id_us, &err) != 0 ||
        measure_select(table, "SELECT * FROM users WHERE age = 150;", AGE_REPEATS, &age_us, &err) != 0) {
        goto fail;
    }
    if (table_close(table, &err) != 0) {
        table = NULL;
        goto fail;
    }

    printf("SQL INSERT: %d rows in %.3f s\n", ROW_COUNT, insert_time);
    printf("Reopen (validate TSV and rebuild index): %.3f s\n", open_time);
    printf("WHERE id = %d (B+ tree, %d runs): %.3f us/query\n", TARGET_ID, ID_REPEATS, id_us);
    printf("WHERE age = 150 (linear scan, %d runs): %.3f us/query\n", AGE_REPEATS, age_us);
    printf("Linear scan / index: %.1fx\n", age_us / id_us);
    return 0;

fail:
    fprintf(stderr, "%s\n", err.msg);
    table_close(table, NULL);
    return 1;
}
