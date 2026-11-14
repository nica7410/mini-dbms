#include "bptree.h"

#include <stdlib.h>
#include <string.h>

#define FANOUT 64

struct BPTreeNode {
    bool leaf;
    int count;
    int64_t keys[FANOUT];
    union {
        int64_t offsets[FANOUT];
        BPTreeNode *children[FANOUT];
    } entries;
    BPTreeNode *next_leaf;
};

static bool is_full(const BPTreeNode *node) {
    return node->count == (node->leaf ? FANOUT : FANOUT - 1);
}

static BPTreeNode *new_node(bool leaf) {
    BPTreeNode *node = malloc(sizeof *node);
    if (node != NULL) {
        node->leaf = leaf;
        node->count = 0;
        node->next_leaf = NULL;
    }
    return node;
}

/* 부모에 자리가 있는 상태에서 맨 오른쪽 자식을 분할한다. */
static bool split_rightmost(BPTreeNode *parent) {
    int slot = parent->count;
    BPTreeNode *left = parent->entries.children[slot];
    int middle = left->count / 2;
    BPTreeNode *right = new_node(left->leaf);
    int64_t separator = left->keys[middle];
    int first;

    if (right == NULL) {
        return false;
    }

    /* 리프에서는 경계 키도 레코드와 함께 오른쪽 리프에 남는다.
     * 내부 노드에서는 경계 키가 부모로 올라간다. */
    first = left->leaf ? middle : middle + 1;
    right->count = left->count - first;
    memcpy(right->keys, left->keys + first, (size_t)right->count * sizeof right->keys[0]);
    if (left->leaf) {
        memcpy(right->entries.offsets, left->entries.offsets + first,
               (size_t)right->count * sizeof right->entries.offsets[0]);
        right->next_leaf = left->next_leaf;
        left->next_leaf = right;
    } else {
        memcpy(right->entries.children, left->entries.children + first,
               (size_t)(right->count + 1) * sizeof right->entries.children[0]);
    }
    left->count = middle;
    parent->keys[slot] = separator;
    parent->entries.children[slot + 1] = right;
    parent->count++;
    return true;
}

BPTreeNode *bptree_reserve_append(BPTree *tree) {
    BPTreeNode *node = tree->root;

    if (node == NULL) {
        node = new_node(true);
        if (node == NULL) {
            return NULL;
        }
        tree->root = node;
    } else if (is_full(node)) {
        BPTreeNode *root = new_node(false);
        if (root == NULL) {
            return NULL;
        }
        root->entries.children[0] = node;
        if (!split_rightmost(root)) {
            free(root);
            return NULL;
        }
        tree->root = node = root;
    }

    /* 증가하는 키는 항상 가장 오른쪽 리프로 들어간다.
     * 내려가기 전에 가득 찬 자식을 분할해 한 자리 이상을 확보한다. */
    while (!node->leaf) {
        if (is_full(node->entries.children[node->count]) &&
            !split_rightmost(node)) {
            return NULL;
        }
        node = node->entries.children[node->count];
    }
    return node;
}

void bptree_append(BPTreeNode *leaf, int64_t key, int64_t offset) {
    leaf->keys[leaf->count] = key;
    leaf->entries.offsets[leaf->count] = offset;
    leaf->count++;
}

/* key보다 큰 첫 키의 인덱스. 내부 노드에서 경계 키와 같으면 오른쪽으로 간다. */
static int upper_bound(const BPTreeNode *node, int64_t key) {
    int lo = 0;
    int hi = node->count;

    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (node->keys[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool bptree_find(const BPTree *tree, int64_t key, int64_t *offset) {
    const BPTreeNode *node = tree->root;
    int slot;

    if (node == NULL) {
        return false;
    }
    while (!node->leaf) {
        node = node->entries.children[upper_bound(node, key)];
    }
    slot = upper_bound(node, key) - 1;
    if (slot < 0 || node->keys[slot] != key) {
        return false;
    }
    *offset = node->entries.offsets[slot];
    return true;
}

static void free_node(BPTreeNode *node) {
    if (!node->leaf) {
        for (int i = 0; i <= node->count; i++) {
            free_node(node->entries.children[i]);
        }
    }
    free(node);
}

void bptree_clear(BPTree *tree) {
    if (tree->root != NULL) {
        free_node(tree->root);
        tree->root = NULL;
    }
}
