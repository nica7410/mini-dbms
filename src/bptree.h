#ifndef MINIDB_BPTREE_H
#define MINIDB_BPTREE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BPTreeNode BPTreeNode;

typedef struct {
    BPTreeNode *root; /* 빈 트리는 {0}으로 초기화한다. */
} BPTree;

/* 다음 키를 넣을 리프를 준비한다. 할당 실패 시 NULL을 반환하며 기존 키는 유지된다. */
BPTreeNode *bptree_reserve_append(BPTree *tree);

/* 방금 준비한 리프에 현재 최대 키보다 큰 키를 추가한다. 메모리 할당은 없다. */
void bptree_append(BPTreeNode *leaf, int64_t key, int64_t offset);

/* 키가 있으면 true를 반환하고 레코드의 바이트 위치를 기록한다. */
bool bptree_find(const BPTree *tree, int64_t key, int64_t *offset);

void bptree_clear(BPTree *tree);

#endif
