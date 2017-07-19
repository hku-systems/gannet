/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifndef QEMU_COLO_H
#define QEMU_COLO_H

#include "qemu-common.h"
#include "migration/migration.h"
#include "block/coroutine.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"

bool colo_supported(void);
void colo_info_mig_init(void);

void colo_init_checkpointer(MigrationState *s);
bool migration_in_colo_state(void);

/* loadvm */
extern Coroutine *migration_incoming_co;
bool migration_incoming_enable_colo(void);
void migration_incoming_exit_colo(void);
void *colo_process_incoming_checkpoints(void *opaque);
bool migration_incoming_in_colo_state(void);

int get_colo_mode(void);

/* ram cache */
int create_and_init_ram_cache(void);
void colo_flush_ram_cache(void);
void release_ram_cache(void);

/* failover */
void colo_do_failover(MigrationState *s);

#endif
