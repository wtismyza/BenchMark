/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
// Copyright (c) 2019-2020 VMware, Inc. All rights reserved.
// Copyright (c) 2016-2019 Carbon Black, Inc. All rights reserved.

#pragma once

#include "process-context.h"

//-------------------------------------------------
// Linux utility functions for locking
//
void cb_spinlock_init(uint64_t *sp, ProcessContext *context);
void cb_spinlock_destroy(uint64_t *sp, ProcessContext *context);
void cb_write_unlock(uint64_t *sp, ProcessContext *context);
void cb_write_lock(uint64_t *sp, ProcessContext *context);
void cb_read_unlock(uint64_t *sp, ProcessContext *context);
void cb_read_lock(uint64_t *sp, ProcessContext *context);
