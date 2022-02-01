/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
// Copyright (c) 2019-2020 VMware, Inc. All rights reserved.
// Copyright (c) 2016-2019 Carbon Black, Inc. All rights reserved.

#pragma once

#include <linux/hash.h>
#include <linux/list.h>

#include "version.h"
#include "mem-cache.h"

#define  ACTION_CONTINUE   0
#define  ACTION_STOP       1
#define  ACTION_DELETE     4

#define  HASHTBL_DISABLE_REF_COUNT  -1


// hash-table-generic provides interfaces for hash tables. It supports arbitary
// key length. In order to use this hash table, you need to create a struct that
// contains a struct hlist_node called 'link'. Then you can add one, or more
// fields as the key. Last, add fields as value. The order does matter here,
// because the implementation will use the address of link plus the offset to
// get the key. So you need to make sure 'link' is before the key, and the key
// is before the value. Also be careful of struct alignment here. Memset to 0 is
// recommended after creating a key. See hash_table_test for usage.
//
typedef void (*hashtbl_delete_cb)(void *datap, ProcessContext *context);

typedef struct hashbtl_bkt {
    uint64_t lock;
    struct hlist_head head;
} HashTableBkt;

typedef struct hashtbl {
    HashTableBkt *tablePtr;
    struct list_head   genTables;
    uint64_t   numberOfBuckets;
    uint32_t   secret;
    atomic64_t tableInstance;
    atomic64_t tableShutdown;  // shutting down = 1 running = 0
    int key_len;
    int value_len;
    CB_MEM_CACHE hash_cache;
    int key_offset;
    int node_offset;
    int refcount_offset;
    size_t base_size;
    hashtbl_delete_cb delete_callback;
} HashTbl;

typedef struct hash_table_node {
    struct hlist_node link;
    u32 hash;
} HashTableNode;

void hashtbl_generic_init(ProcessContext *context);
void hashtbl_generic_destoy(ProcessContext *context);

typedef int (*hashtbl_for_each_generic_cb)(HashTbl *tblp, HashTableNode *datap, void *priv, ProcessContext *context);

HashTbl *hashtbl_init_generic(ProcessContext *context,
                              uint64_t numberOfBuckets, uint64_t datasize,
                              uint64_t sizehint, const char *hashtble_name, int key_len,
                              int key_offset, int node_offset, int refcount_offset,
                              hashtbl_delete_cb delete_callback);
void *hashtbl_alloc_generic(HashTbl *tblp, ProcessContext *context);
int hashtbl_add_generic(HashTbl *tblp, void *datap, ProcessContext *context);

// Like hashtbl_add_generic but returns -EEXIST on a duplicate entry.
// Caller responsible for freeing on failure to add entry.
int hashtbl_add_generic_safe(HashTbl *hashTblp, void *datap, ProcessContext *context);

// Finds and removes data for key from hash table. Caller must put or free return.
void *hashtbl_del_by_key_generic(HashTbl *tblp, void *key, ProcessContext *context);

// Removes datap from hash table but does not free it
// Free with hashtbl_put_generic (for ref counted hashtbl) or hashtbl_free_generic
void hashtbl_del_generic(HashTbl *tblp, void *datap, ProcessContext *context);

void *hashtbl_get_generic(HashTbl *tblp, void *key, ProcessContext *context);

// Decrements reference count and frees datap if reference count is 0
// Only for reference counted hash tables
void hashtbl_put_generic(HashTbl *tblp, void *datap, ProcessContext *context);

void hashtbl_free_generic(HashTbl *tblp, void *datap, ProcessContext *context);
void hashtbl_shutdown_generic(HashTbl *tblp, ProcessContext *context);
void hashtbl_clear_generic(HashTbl *tblp, ProcessContext *context);
void hashtbl_write_for_each_generic(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, ProcessContext *context);
void hashtbl_read_for_each_generic(HashTbl *hashTblp, hashtbl_for_each_generic_cb callback, void *priv, ProcessContext *context);
int hashtbl_show_proc_cache(struct seq_file *m, void *v);
size_t hashtbl_get_memory(ProcessContext *context);
void debug_on(void);
void debug_off(void);
