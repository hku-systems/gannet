/*
 * Replication Block filter
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO., LTD.
 * Copyright (c) 2015 Intel Corporation
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * Author:
 *   Wen Congyang <wency@cn.fujitsu.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu-common.h"
#include "block/block_int.h"
#include "block/blockjob.h"
#include "block/nbd.h"

#define DISABLE_HIDDEN
#define DISABLE_BACKUP

#define DEBUG_IO_LATENCY

#ifdef DEBUG_IO_LATENCY
#include <sys/time.h>
#include <stdio.h>

static float timedifference_msec(struct timeval t0, struct timeval t1);

#ifndef _TM_FUNC
#define _TM_FUNC
static float timedifference_msec(struct timeval t0, struct timeval t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000.0f
           + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}
#endif

#endif

typedef struct BDRVReplicationState {
    ReplicationMode mode;
    int replication_state;
    BlockDriverState *active_disk;
    #ifndef DISABLE_HIDDEN
    BlockDriverState *hidden_disk;
    #endif
    BlockDriverState *secondary_disk;
    int error;
} BDRVReplicationState;

enum {
    BLOCK_REPLICATION_NONE,     /* block replication is not started */
    BLOCK_REPLICATION_RUNNING,  /* block replication is running */
    BLOCK_REPLICATION_DONE,     /* block replication is done(failover) */
};

#define COMMIT_CLUSTER_BITS 16
#define COMMIT_CLUSTER_SIZE (1 << COMMIT_CLUSTER_BITS)
#define COMMIT_SECTORS_PER_CLUSTER (COMMIT_CLUSTER_SIZE / BDRV_SECTOR_SIZE)

static void replication_stop(BlockDriverState *bs, bool failover, Error **errp);

#define REPLICATION_MODE        "mode"
static QemuOptsList replication_runtime_opts = {
    .name = "replication",
    .head = QTAILQ_HEAD_INITIALIZER(replication_runtime_opts.head),
    .desc = {
        {
            .name = REPLICATION_MODE,
            .type = QEMU_OPT_STRING,
        },
        { /* end of list */ }
    },
};

static int replication_open(BlockDriverState *bs, QDict *options,
                            int flags, Error **errp)
{
    int ret;
    BDRVReplicationState *s = bs->opaque;;
    Error *local_err = NULL;
    QemuOpts *opts = NULL;
    const char *mode;

    ret = -EINVAL;
    opts = qemu_opts_create(&replication_runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        goto fail;
    }

    mode = qemu_opt_get(opts, REPLICATION_MODE);
    if (!mode) {
        error_setg(&local_err, "Missing the option mode");
        goto fail;
    }

    if (!strcmp(mode, "primary")) {
        s->mode = REPLICATION_MODE_PRIMARY;
    } else if (!strcmp(mode, "secondary")) {
        s->mode = REPLICATION_MODE_SECONDARY;
    } else {
        error_setg(&local_err,
                   "The option mode's value should be primary or secondary");
        goto fail;
    }

    ret = 0;

fail:
    qemu_opts_del(opts);
    /* propagate error */
    if (local_err) {
        error_propagate(errp, local_err);
    }
    return ret;
}

static void replication_close(BlockDriverState *bs)
{
    BDRVReplicationState *s = bs->opaque;

    if (s->replication_state == BLOCK_REPLICATION_RUNNING) {
        replication_stop(bs, false, NULL);
    }
}

static int64_t replication_getlength(BlockDriverState *bs)
{
    return bdrv_getlength(bs->file);
}

static int replication_get_io_status(BDRVReplicationState *s)
{
    switch (s->replication_state) {
    case BLOCK_REPLICATION_NONE:
        return -EIO;
    case BLOCK_REPLICATION_RUNNING:
        return 0;
    case BLOCK_REPLICATION_DONE:
        return s->mode == REPLICATION_MODE_PRIMARY ? -EIO : 1;
    default:
        abort();
    }
}

static int replication_return_value(BDRVReplicationState *s, int ret)
{
    if (s->mode == REPLICATION_MODE_SECONDARY) {
        return ret;
    }

    if (ret < 0) {
        s->error = ret;
        ret = 0;
    }

    return ret;
}

static coroutine_fn int replication_co_readv(BlockDriverState *bs,
                                             int64_t sector_num,
                                             int remaining_sectors,
                                             QEMUIOVector *qiov)
{
    BDRVReplicationState *s = bs->opaque;
    int ret;

    if (s->mode == REPLICATION_MODE_PRIMARY) {
        /* We only use it to forward primary write requests */
        return -EIO;
    }

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    /*
     * After failover, because we don't commit active disk/hidden disk
     * to secondary disk, so we should read from active disk directly.
     */
    ret = bdrv_co_readv(bs->file, sector_num, remaining_sectors, qiov);
    return replication_return_value(s, ret);
}

static coroutine_fn int replication_co_writev(BlockDriverState *bs,
                                              int64_t sector_num,
                                              int remaining_sectors,
                                              QEMUIOVector *qiov)
{
    BDRVReplicationState *s = bs->opaque;
    QEMUIOVector hd_qiov;
    uint64_t bytes_done = 0;
    BlockDriverState *top = bs->file;
    BlockDriverState *base = s->secondary_disk;
    BlockDriverState *target;
    int ret, n;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    #ifdef DEBUG_IO_LATENCY
    FILE *fp = fopen("IO_latency.log", "a+");
    struct timeval start, stop;
    gettimeofday(&start, NULL);
    #endif

    if (ret == 0) {
        ret = bdrv_co_writev(bs->file, sector_num, remaining_sectors, qiov);

        #ifdef DEBUG_IO_LATENCY
        gettimeofday(&stop, NULL);
        fprintf(fp, "Write took %f msec\n", timedifference_msec(start, stop));
        fclose(fp);
        #endif

        return replication_return_value(s, ret);
    }

    /*
     * Only write to active disk if the sectors have
     * already been allocated in active disk/hidden disk.
     */
    qemu_iovec_init(&hd_qiov, qiov->niov);
    while (remaining_sectors > 0) {
        ret = bdrv_is_allocated_above(top, base, sector_num,
                                      remaining_sectors, &n);
        if (ret < 0) {
            return ret;
        }

        qemu_iovec_reset(&hd_qiov);
        qemu_iovec_concat(&hd_qiov, qiov, bytes_done, n * 512);

        target = ret ? top: base;
        ret = bdrv_co_writev(target, sector_num, n, &hd_qiov);
        if (ret < 0) {
            return ret;
        }

        remaining_sectors -= n;
        sector_num += n;
        bytes_done += n * BDRV_SECTOR_SIZE;
    }

    #ifdef DEBUG_IO_LATENCY
    gettimeofday(&stop, NULL);
    fprintf(fp, "Write took %f msec\n", timedifference_msec(start, stop));
    fclose(fp);
    #endif

    return 0;
}

static coroutine_fn int replication_co_discard(BlockDriverState *bs,
                                               int64_t sector_num,
                                               int nb_sectors)
{
    BDRVReplicationState *s = bs->opaque;
    int ret;

    ret = replication_get_io_status(s);
    if (ret < 0) {
        return ret;
    }

    if (ret == 1) {
        /* It is secondary qemu and we are after failover */
        ret = bdrv_co_discard(s->secondary_disk, sector_num, nb_sectors);
        if (ret) {
            return ret;
        }
    }

    ret = bdrv_co_discard(bs->file, sector_num, nb_sectors);
    return replication_return_value(s, ret);
}

static bool replication_recurse_is_first_non_filter(BlockDriverState *bs,
                                                    BlockDriverState *candidate)
{
    return bdrv_recurse_is_first_non_filter(bs->file, candidate);
}

static void secondary_do_checkpoint(BDRVReplicationState *s, Error **errp)
{
    int ret;

    #ifndef DISABLE_BACKUP
    Error *local_err = NULL;

    if (!s->secondary_disk->job) {
        error_setg(errp, "Backup job is cancelled unexpectedly");
        return;
    }

    block_job_do_checkpoint(s->secondary_disk->job, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    #endif

    ret = s->active_disk->drv->bdrv_make_empty(s->active_disk);
    if (ret < 0) {
        error_setg(errp, "Cannot make active disk empty");
        return;
    }

    #ifndef DISABLE_HIDDEN
    ret = s->hidden_disk->drv->bdrv_make_empty(s->hidden_disk);
    if (ret < 0) {
        error_setg(errp, "Cannot make hidden disk empty");
        return;
    }
    #endif
}

#ifndef DISABLE_BACKUP
static void backup_job_completed(void *opaque, int ret)
{
    BDRVReplicationState *s = opaque;

    if (s->replication_state != BLOCK_REPLICATION_DONE) {
        /* The backup job is cancelled unexpectedly */
        s->error = -EIO;
    }

    #ifndef DISABLE_HIDDEN
    bdrv_op_block(s->hidden_disk, BLOCK_OP_TYPE_BACKUP_TARGET,
                  s->active_disk->backing_blocker);
    #endif

    bdrv_op_block(s->secondary_disk, BLOCK_OP_TYPE_BACKUP_SOURCE,
                  #ifndef DISABLE_HIDDEN
                  s->hidden_disk->backing_blocker
                  #else
                  s->active_disk->backing_blocker
                  #endif
    );

    bdrv_put_ref_bh_schedule(s->secondary_disk);
}
#endif

static void replication_start(BlockDriverState *bs, ReplicationMode mode,
                              Error **errp)
{
    BDRVReplicationState *s = bs->opaque;
    int64_t active_length, disk_length;

    #ifndef DISABLE_BACKUP
    AioContext *aio_context;
    Error *local_err = NULL;
    #endif


    if (s->replication_state != BLOCK_REPLICATION_NONE) {
        error_setg(errp, "Block replication is running or done");
        return;
    }

    if (s->mode != mode) {
        error_setg(errp, "Invalid parameter 'mode'");
        return;
    }

    switch (s->mode) {
    case REPLICATION_MODE_PRIMARY:
        break;
    case REPLICATION_MODE_SECONDARY:
        s->active_disk = bs->file;
        if (!bs->file->backing_hd) {
            error_setg(errp, "Active disk doesn't have backing file");
            return;
        }

        #ifndef DISABLE_HIDDEN
        s->hidden_disk = s->active_disk->backing_hd;
        if (!s->hidden_disk->backing_hd) {
            error_setg(errp, "Hidden disk doesn't have backing file");
            return;
        }
        s->secondary_disk = s->hidden_disk->backing_hd;
        #else
        s->secondary_disk = s->active_disk->backing_hd;
        #endif

        if (!s->secondary_disk->blk) {
            error_setg(errp, "The secondary disk doesn't have block backend");
            return;
        }

        /* verify the length */
        active_length = bdrv_getlength(s->active_disk);
        disk_length = bdrv_getlength(s->secondary_disk);

        #ifndef DISABLE_HIDDEN
        int64_t hidden_length = bdrv_getlength(s->hidden_disk);
        #endif

        if (active_length < 0 || disk_length < 0 || active_length != disk_length
            #ifndef DISABLE_HIDDEN
            || hidden_length < 0 || hidden_length != active_length
            #endif
        )
        {
            error_setg(errp, "Disks in chain do not have the same length");
            return;
        }

        if (!s->active_disk->drv->bdrv_make_empty
            #ifndef DISABLE_HIDDEN
            || !s->hidden_disk->drv->bdrv_make_empty
            #endif
          )
        {
            error_setg(errp,
                       "active disk "
                       #ifndef DISABLE_HIDDEN
                       "or hidden disk "
                       #endif
                       "doesn't support make_empty");
            return;
        }

        #ifdef DISABLE_HIDDEN
        assert(strcmp(bdrv_get_device_or_node_name(s->active_disk->backing_hd),
                      "colo1") == 0);
        #endif

        #ifndef DISABLE_BACKUP
        /* start backup job now */
        #ifndef DISABLE_HIDDEN
        bdrv_op_unblock(s->hidden_disk, BLOCK_OP_TYPE_BACKUP_TARGET,
                      s->active_disk->backing_blocker);
        #endif

        bdrv_op_unblock(s->secondary_disk, BLOCK_OP_TYPE_BACKUP_SOURCE,
                      #ifndef DISABLE_HIDDEN
                      s->hidden_disk->backing_blocker
                      #else
                      s->active_disk->backing_blocker
                      #endif
        );

        bdrv_ref(
            #ifndef DISABLE_HIDDEN
            s->hidden_disk
            #else
            s->active_disk
            #endif
        );

        aio_context = bdrv_get_aio_context(bs);
        aio_context_acquire(aio_context);
        bdrv_set_aio_context(s->secondary_disk, aio_context);

        backup_start(s->secondary_disk,
                     #ifndef DISABLE_HIDDEN
                     s->hidden_disk,
                     #else
                     s->active_disk,
                     #endif
                     0, MIRROR_SYNC_MODE_NONE, NULL, BLOCKDEV_ON_ERROR_REPORT,
                     BLOCKDEV_ON_ERROR_REPORT, backup_job_completed,
                     s, &local_err);

        aio_context_release(aio_context);
        if (local_err) {
            error_propagate(errp, local_err);
            #ifndef DISABLE_HIDDEN
            bdrv_op_block(s->hidden_disk, BLOCK_OP_TYPE_BACKUP_TARGET,
                          s->active_disk->backing_blocker);
            #endif

            bdrv_op_block(s->secondary_disk, BLOCK_OP_TYPE_BACKUP_SOURCE,
                          #ifndef DISABLE_HIDDEN
                          s->hidden_disk->backing_blocker
                          #else
                          s->active_disk->backing_blocker
                          #endif
            );

            bdrv_ref(
                #ifndef DISABLE_HIDDEN
                s->hidden_disk
                #else
                s->active_disk
                #endif
            );
            return;
        }
        #endif
        break;
    default:
        abort();
    }

    s->replication_state = BLOCK_REPLICATION_RUNNING;

    if (s->mode == REPLICATION_MODE_SECONDARY) {
        secondary_do_checkpoint(s, errp);
    }

    s->error = 0;
}

static void replication_do_checkpoint(BlockDriverState *bs, Error **errp)
{
    BDRVReplicationState *s = bs->opaque;

    if (s->replication_state != BLOCK_REPLICATION_RUNNING) {
        error_setg(errp, "Block replication is not running");
        return;
    }

    if (s->error) {
        error_setg(errp, "I/O error occurs");
        return;
    }

    if (s->mode == REPLICATION_MODE_SECONDARY) {
        secondary_do_checkpoint(s, errp);
    }
}

static void replication_stop(BlockDriverState *bs, bool failover, Error **errp)
{
    BDRVReplicationState *s = bs->opaque;

    if (s->replication_state != BLOCK_REPLICATION_RUNNING) {
        error_setg(errp, "Block replication is not running");
        return;
    }

    s->replication_state = BLOCK_REPLICATION_DONE;

    switch (s->mode) {
    case REPLICATION_MODE_PRIMARY:
        break;
    case REPLICATION_MODE_SECONDARY:
        if (!failover) {
            /*
             * The guest will be shutdown, and secondary disk is the
             * same as the primary disk. Just make active disk and
             * hidden disk empty.
             */
            secondary_do_checkpoint(s, errp);
            return;
        }

        if (s->secondary_disk->job) {
            block_job_cancel(s->secondary_disk->job);
        }
        break;
    default:
        abort();
    }
}

BlockDriver bdrv_replication = {
    .format_name                = "replication",
    .protocol_name              = "replication",
    .instance_size              = sizeof(BDRVReplicationState),

    .bdrv_open                  = replication_open,
    .bdrv_close                 = replication_close,

    .bdrv_getlength             = replication_getlength,
    .bdrv_co_readv              = replication_co_readv,
    .bdrv_co_writev             = replication_co_writev,
    .bdrv_co_discard            = replication_co_discard,

    .is_filter                  = true,
    .bdrv_recurse_is_first_non_filter = replication_recurse_is_first_non_filter,

    .bdrv_start_replication     = replication_start,
    .bdrv_do_checkpoint         = replication_do_checkpoint,
    .bdrv_stop_replication      = replication_stop,

    .has_variable_length        = true,
};

static void bdrv_replication_init(void)
{
    bdrv_register(&bdrv_replication);
}

block_init(bdrv_replication_init);
