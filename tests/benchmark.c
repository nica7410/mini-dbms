#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
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

static int measure_select(Table *table, const char *sql, int repeats, double *us, DbError *err) {
    int count = 0;
    ResultSink sink = {NULL, count_target, &count};
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

    if (argc != 2) {
        fprintf(stderr, "Usage: benchmark <empty temporary data directory>\n");
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
