/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2009-2015 Red Hat Inc
 *
 * Authors:
 *  Juan Quintela <quintela@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config-host.h"
#include "qemu-common.h"
#include "hw/boards.h"
#include "hw/hw.h"
#include "hw/qdev.h"
#include "net/net.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qemu/timer.h"
#include "audio/audio.h"
#include "migration/migration.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/queue.h"
#include "sysemu/cpus.h"
#include "exec/memory.h"
#include "qmp-commands.h"
#include "trace.h"
#include "qemu/iov.h"
#include "block/snapshot.h"
#include "block/qapi.h"
#include "migration/colo.h"

#ifndef ETH_P_RARP
#define ETH_P_RARP 0x8035
#endif
#define ARP_HTYPE_ETH 0x0001
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST_REV 0x3

static bool skip_section_footers;

static int announce_self_create(uint8_t *buf,
                                uint8_t *mac_addr)
{
    /* Ethernet header. */
    memset(buf, 0xff, 6);         /* destination MAC addr */
    memcpy(buf + 6, mac_addr, 6); /* source MAC addr */
    *(uint16_t *)(buf + 12) = htons(ETH_P_RARP); /* ethertype */

    /* RARP header. */
    *(uint16_t *)(buf + 14) = htons(ARP_HTYPE_ETH); /* hardware addr space */
    *(uint16_t *)(buf + 16) = htons(ARP_PTYPE_IP); /* protocol addr space */
    *(buf + 18) = 6; /* hardware addr length (ethernet) */
    *(buf + 19) = 4; /* protocol addr length (IPv4) */
    *(uint16_t *)(buf + 20) = htons(ARP_OP_REQUEST_REV); /* opcode */
    memcpy(buf + 22, mac_addr, 6); /* source hw addr */
    memset(buf + 28, 0x00, 4);     /* source protocol addr */
    memcpy(buf + 32, mac_addr, 6); /* target hw addr */
    memset(buf + 38, 0x00, 4);     /* target protocol addr */

    /* Padding to get up to 60 bytes (ethernet min packet size, minus FCS). */
    memset(buf + 42, 0x00, 18);

    return 60; /* len (FCS will be added by hardware) */
}

static void qemu_announce_self_iter(NICState *nic, void *opaque)
{
    uint8_t buf[60];
    int len;

    trace_qemu_announce_self_iter(qemu_ether_ntoa(&nic->conf->macaddr));
    len = announce_self_create(buf, nic->conf->macaddr.a);

    qemu_send_packet_raw(qemu_get_queue(nic), buf, len);
}


static void qemu_announce_self_once(void *opaque)
{
    static int count = SELF_ANNOUNCE_ROUNDS;
    QEMUTimer *timer = *(QEMUTimer **)opaque;

    qemu_foreach_nic(qemu_announce_self_iter, NULL);

    if (--count) {
        /* delay 50ms, 150ms, 250ms, ... */
        timer_mod(timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                  self_announce_delay(count));
    } else {
            timer_del(timer);
            timer_free(timer);
    }
}

void qemu_announce_self(void)
{
    static QEMUTimer *timer;
    timer = timer_new_ms(QEMU_CLOCK_REALTIME, qemu_announce_self_once, &timer);
    qemu_announce_self_once(&timer);
}

/***********************************************************/
/* savevm/loadvm support */

static ssize_t block_writev_buffer(void *opaque, struct iovec *iov, int iovcnt,
                                   int64_t pos)
{
    int ret;
    QEMUIOVector qiov;

    qemu_iovec_init_external(&qiov, iov, iovcnt);
    ret = bdrv_writev_vmstate(opaque, &qiov, pos);
    if (ret < 0) {
        return ret;
    }

    return qiov.size;
}

static int block_put_buffer(void *opaque, const uint8_t *buf,
                           int64_t pos, int size)
{
    bdrv_save_vmstate(opaque, buf, pos, size);
    return size;
}

static int block_get_buffer(void *opaque, uint8_t *buf, int64_t pos, int size)
{
    return bdrv_load_vmstate(opaque, buf, pos, size);
}

static int bdrv_fclose(void *opaque)
{
    return bdrv_flush(opaque);
}

static const QEMUFileOps bdrv_read_ops = {
    .get_buffer = block_get_buffer,
    .close =      bdrv_fclose
};

static const QEMUFileOps bdrv_write_ops = {
    .put_buffer     = block_put_buffer,
    .writev_buffer  = block_writev_buffer,
    .close          = bdrv_fclose
};

static QEMUFile *qemu_fopen_bdrv(BlockDriverState *bs, int is_writable)
{
    if (is_writable) {
        return qemu_fopen_ops(bs, &bdrv_write_ops);
    }
    return qemu_fopen_ops(bs, &bdrv_read_ops);
}


/* QEMUFile timer support.
 * Not in qemu-file.c to not add qemu-timer.c as dependency to qemu-file.c
 */

void timer_put(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = timer_expire_time_ns(ts);
    qemu_put_be64(f, expire_time);
}

void timer_get(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = qemu_get_be64(f);
    if (expire_time != -1) {
        timer_mod_ns(ts, expire_time);
    } else {
        timer_del(ts);
    }
}


/* VMState timer support.
 * Not in vmstate.c to not add qemu-timer.c as dependency to vmstate.c
 */

static int get_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_get(f, v);
    return 0;
}

static void put_timer(QEMUFile *f, void *pv, size_t size)
{
    QEMUTimer *v = pv;
    timer_put(f, v);
}

const VMStateInfo vmstate_info_timer = {
    .name = "timer",
    .get  = get_timer,
    .put  = put_timer,
};


typedef struct CompatEntry {
    char idstr[256];
    int instance_id;
} CompatEntry;

typedef struct SaveStateEntry {
    QTAILQ_ENTRY(SaveStateEntry) entry;
    char idstr[256];
    int instance_id;
    int alias_id;
    int version_id;
    int section_id;
    SaveVMHandlers *ops;
    const VMStateDescription *vmsd;
    void *opaque;
    CompatEntry *compat;
    int is_ram;
} SaveStateEntry;

typedef struct SaveState {
    QTAILQ_HEAD(, SaveStateEntry) handlers;
    int global_section_id;
    bool skip_configuration;
    uint32_t len;
    const char *name;
} SaveState;

static SaveState savevm_state = {
    .handlers = QTAILQ_HEAD_INITIALIZER(savevm_state.handlers),
    .global_section_id = 0,
    .skip_configuration = false,
};

void savevm_skip_configuration(void)
{
    savevm_state.skip_configuration = true;
}


static void configuration_pre_save(void *opaque)
{
    SaveState *state = opaque;
    const char *current_name = MACHINE_GET_CLASS(current_machine)->name;

    state->len = strlen(current_name);
    state->name = current_name;
}

static int configuration_post_load(void *opaque, int version_id)
{
    SaveState *state = opaque;
    const char *current_name = MACHINE_GET_CLASS(current_machine)->name;

    if (strncmp(state->name, current_name, state->len) != 0) {
        error_report("Machine type received is '%s' and local is '%s'",
                     state->name, current_name);
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_configuration = {
    .name = "configuration",
    .version_id = 1,
    .post_load = configuration_post_load,
    .pre_save = configuration_pre_save,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(len, SaveState),
        VMSTATE_VBUFFER_ALLOC_UINT32(name, SaveState, 0, NULL, 0, len),
        VMSTATE_END_OF_LIST()
    },
};

static void dump_vmstate_vmsd(FILE *out_file,
                              const VMStateDescription *vmsd, int indent,
                              bool is_subsection);

static void dump_vmstate_vmsf(FILE *out_file, const VMStateField *field,
                              int indent)
{
    fprintf(out_file, "%*s{\n", indent, "");
    indent += 2;
    fprintf(out_file, "%*s\"field\": \"%s\",\n", indent, "", field->name);
    fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
            field->version_id);
    fprintf(out_file, "%*s\"field_exists\": %s,\n", indent, "",
            field->field_exists ? "true" : "false");
    fprintf(out_file, "%*s\"size\": %zu", indent, "", field->size);
    if (field->vmsd != NULL) {
        fprintf(out_file, ",\n");
        dump_vmstate_vmsd(out_file, field->vmsd, indent, false);
    }
    fprintf(out_file, "\n%*s}", indent - 2, "");
}

static void dump_vmstate_vmss(FILE *out_file,
                              const VMStateDescription **subsection,
                              int indent)
{
    if (*subsection != NULL) {
        dump_vmstate_vmsd(out_file, *subsection, indent, true);
    }
}

static void dump_vmstate_vmsd(FILE *out_file,
                              const VMStateDescription *vmsd, int indent,
                              bool is_subsection)
{
    if (is_subsection) {
        fprintf(out_file, "%*s{\n", indent, "");
    } else {
        fprintf(out_file, "%*s\"%s\": {\n", indent, "", "Description");
    }
    indent += 2;
    fprintf(out_file, "%*s\"name\": \"%s\",\n", indent, "", vmsd->name);
    fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
            vmsd->version_id);
    fprintf(out_file, "%*s\"minimum_version_id\": %d", indent, "",
            vmsd->minimum_version_id);
    if (vmsd->fields != NULL) {
        const VMStateField *field = vmsd->fields;
        bool first;

        fprintf(out_file, ",\n%*s\"Fields\": [\n", indent, "");
        first = true;
        while (field->name != NULL) {
            if (field->flags & VMS_MUST_EXIST) {
                /* Ignore VMSTATE_VALIDATE bits; these don't get migrated */
                field++;
                continue;
            }
            if (!first) {
                fprintf(out_file, ",\n");
            }
            dump_vmstate_vmsf(out_file, field, indent + 2);
            field++;
            first = false;
        }
        fprintf(out_file, "\n%*s]", indent, "");
    }
    if (vmsd->subsections != NULL) {
        const VMStateDescription **subsection = vmsd->subsections;
        bool first;

        fprintf(out_file, ",\n%*s\"Subsections\": [\n", indent, "");
        first = true;
        while (*subsection != NULL) {
            if (!first) {
                fprintf(out_file, ",\n");
            }
            dump_vmstate_vmss(out_file, subsection, indent + 2);
            subsection++;
            first = false;
        }
        fprintf(out_file, "\n%*s]", indent, "");
    }
    fprintf(out_file, "\n%*s}", indent - 2, "");
}

static void dump_machine_type(FILE *out_file)
{
    MachineClass *mc;

    mc = MACHINE_GET_CLASS(current_machine);

    fprintf(out_file, "  \"vmschkmachine\": {\n");
    fprintf(out_file, "    \"Name\": \"%s\"\n", mc->name);
    fprintf(out_file, "  },\n");
}

void dump_vmstate_json_to_file(FILE *out_file)
{
    GSList *list, *elt;
    bool first;

    fprintf(out_file, "{\n");
    dump_machine_type(out_file);

    first = true;
    list = object_class_get_list(TYPE_DEVICE, true);
    for (elt = list; elt; elt = elt->next) {
        DeviceClass *dc = OBJECT_CLASS_CHECK(DeviceClass, elt->data,
                                             TYPE_DEVICE);
        const char *name;
        int indent = 2;

        if (!dc->vmsd) {
            continue;
        }

        if (!first) {
            fprintf(out_file, ",\n");
        }
        name = object_class_get_name(OBJECT_CLASS(dc));
        fprintf(out_file, "%*s\"%s\": {\n", indent, "", name);
        indent += 2;
        fprintf(out_file, "%*s\"Name\": \"%s\",\n", indent, "", name);
        fprintf(out_file, "%*s\"version_id\": %d,\n", indent, "",
                dc->vmsd->version_id);
        fprintf(out_file, "%*s\"minimum_version_id\": %d,\n", indent, "",
                dc->vmsd->minimum_version_id);

        dump_vmstate_vmsd(out_file, dc->vmsd, indent, false);

        fprintf(out_file, "\n%*s}", indent - 2, "");
        first = false;
    }
    fprintf(out_file, "\n}\n");
    fclose(out_file);
}

static int calculate_new_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (strcmp(idstr, se->idstr) == 0
            && instance_id <= se->instance_id) {
            instance_id = se->instance_id + 1;
        }
    }
    return instance_id;
}

static int calculate_compat_instance_id(const char *idstr)
{
    SaveStateEntry *se;
    int instance_id = 0;

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->compat) {
            continue;
        }

        if (strcmp(idstr, se->compat->idstr) == 0
            && instance_id <= se->compat->instance_id) {
            instance_id = se->compat->instance_id + 1;
        }
    }
    return instance_id;
}

/* TODO: Individual devices generally have very little idea about the rest
   of the system, so instance_id should be removed/replaced.
   Meanwhile pass -1 as instance_id if you do not already have a clearly
   distinguishing id for all instances of your device class. */
int register_savevm_live(DeviceState *dev,
                         const char *idstr,
                         int instance_id,
                         int version_id,
                         SaveVMHandlers *ops,
                         void *opaque)
{
    SaveStateEntry *se;

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = version_id;
    se->section_id = savevm_state.global_section_id++;
    se->ops = ops;
    se->opaque = opaque;
    se->vmsd = NULL;
    /* if this is a live_savem then set is_ram */
    if (ops->save_live_setup != NULL) {
        se->is_ram = 1;
    }

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), idstr);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(idstr) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), idstr);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_state.handlers, se, entry);
    return 0;
}

int register_savevm(DeviceState *dev,
                    const char *idstr,
                    int instance_id,
                    int version_id,
                    SaveStateHandler *save_state,
                    LoadStateHandler *load_state,
                    void *opaque)
{
    SaveVMHandlers *ops = g_malloc0(sizeof(SaveVMHandlers));
    ops->save_state = save_state;
    ops->load_state = load_state;
    return register_savevm_live(dev, idstr, instance_id, version_id,
                                ops, opaque);
}

void unregister_savevm(DeviceState *dev, const char *idstr, void *opaque)
{
    SaveStateEntry *se, *new_se;
    char id[256] = "";

    if (dev) {
        char *path = qdev_get_dev_path(dev);
        if (path) {
            pstrcpy(id, sizeof(id), path);
            pstrcat(id, sizeof(id), "/");
            g_free(path);
        }
    }
    pstrcat(id, sizeof(id), idstr);

    QTAILQ_FOREACH_SAFE(se, &savevm_state.handlers, entry, new_se) {
        if (strcmp(se->idstr, id) == 0 && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_state.handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se->ops);
            g_free(se);
        }
    }
}

int vmstate_register_with_alias_id(DeviceState *dev, int instance_id,
                                   const VMStateDescription *vmsd,
                                   void *opaque, int alias_id,
                                   int required_for_version)
{
    SaveStateEntry *se;

    /* If this triggers, alias support can be dropped for the vmsd. */
    assert(alias_id == -1 || required_for_version >= vmsd->minimum_version_id);

    se = g_malloc0(sizeof(SaveStateEntry));
    se->version_id = vmsd->version_id;
    se->section_id = savevm_state.global_section_id++;
    se->opaque = opaque;
    se->vmsd = vmsd;
    se->alias_id = alias_id;

    if (dev) {
        char *id = qdev_get_dev_path(dev);
        if (id) {
            pstrcpy(se->idstr, sizeof(se->idstr), id);
            pstrcat(se->idstr, sizeof(se->idstr), "/");
            g_free(id);

            se->compat = g_malloc0(sizeof(CompatEntry));
            pstrcpy(se->compat->idstr, sizeof(se->compat->idstr), vmsd->name);
            se->compat->instance_id = instance_id == -1 ?
                         calculate_compat_instance_id(vmsd->name) : instance_id;
            instance_id = -1;
        }
    }
    pstrcat(se->idstr, sizeof(se->idstr), vmsd->name);

    if (instance_id == -1) {
        se->instance_id = calculate_new_instance_id(se->idstr);
    } else {
        se->instance_id = instance_id;
    }
    assert(!se->compat || se->instance_id == 0);
    /* add at the end of list */
    QTAILQ_INSERT_TAIL(&savevm_state.handlers, se, entry);
    return 0;
}

void vmstate_unregister(DeviceState *dev, const VMStateDescription *vmsd,
                        void *opaque)
{
    SaveStateEntry *se, *new_se;

    QTAILQ_FOREACH_SAFE(se, &savevm_state.handlers, entry, new_se) {
        if (se->vmsd == vmsd && se->opaque == opaque) {
            QTAILQ_REMOVE(&savevm_state.handlers, se, entry);
            if (se->compat) {
                g_free(se->compat);
            }
            g_free(se);
        }
    }
}

static int vmstate_load(QEMUFile *f, SaveStateEntry *se, int version_id)
{
    trace_vmstate_load(se->idstr, se->vmsd ? se->vmsd->name : "(old)");
    if (!se->vmsd) {         /* Old style */
        return se->ops->load_state(f, se->opaque, version_id);
    }
    return vmstate_load_state(f, se->vmsd, se->opaque, version_id);
}

static void vmstate_save_old_style(QEMUFile *f, SaveStateEntry *se, QJSON *vmdesc)
{
    int64_t old_offset, size;

    old_offset = qemu_ftell_fast(f);
    se->ops->save_state(f, se->opaque);
    size = qemu_ftell_fast(f) - old_offset;

    if (vmdesc) {
        json_prop_int(vmdesc, "size", size);
        json_start_array(vmdesc, "fields");
        json_start_object(vmdesc, NULL);
        json_prop_str(vmdesc, "name", "data");
        json_prop_int(vmdesc, "size", size);
        json_prop_str(vmdesc, "type", "buffer");
        json_end_object(vmdesc);
        json_end_array(vmdesc);
    }
}

static void vmstate_save(QEMUFile *f, SaveStateEntry *se, QJSON *vmdesc)
{
    trace_vmstate_save(se->idstr, se->vmsd ? se->vmsd->name : "(old)");
    if (!se->vmsd) {
        vmstate_save_old_style(f, se, vmdesc);
        return;
    }
    vmstate_save_state(f, se->vmsd, se->opaque, vmdesc);
}

void savevm_skip_section_footers(void)
{
    skip_section_footers = true;
}

/*
 * Write the header for device section (QEMU_VM_SECTION START/END/PART/FULL)
 */
static void save_section_header(QEMUFile *f, SaveStateEntry *se,
                                uint8_t section_type)
{
    qemu_put_byte(f, section_type);
    qemu_put_be32(f, se->section_id);

    if (section_type == QEMU_VM_SECTION_FULL ||
        section_type == QEMU_VM_SECTION_START) {
        /* ID string */
        size_t len = strlen(se->idstr);
        qemu_put_byte(f, len);
        qemu_put_buffer(f, (uint8_t *)se->idstr, len);

        qemu_put_be32(f, se->instance_id);
        qemu_put_be32(f, se->version_id);
    }
}

/*
 * Write a footer onto device sections that catches cases misformatted device
 * sections.
 */
static void save_section_footer(QEMUFile *f, SaveStateEntry *se)
{
    if (!skip_section_footers) {
        qemu_put_byte(f, QEMU_VM_SECTION_FOOTER);
        qemu_put_be32(f, se->section_id);
    }
}

bool qemu_savevm_state_blocked(Error **errp)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (se->vmsd && se->vmsd->unmigratable) {
            error_setg(errp, "State blocked by non-migratable device '%s'",
                       se->idstr);
            return true;
        }
    }
    return false;
}

void qemu_savevm_state_header(QEMUFile *f)
{
    trace_savevm_state_header();
    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);
}

void qemu_savevm_state_begin(QEMUFile *f,
                             const MigrationParams *params)
{
    SaveStateEntry *se;
    int ret;

    trace_savevm_state_begin();
    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->ops || !se->ops->set_params) {
            continue;
        }
        se->ops->set_params(params, se->opaque);
    }

    if (!savevm_state.skip_configuration) {
        qemu_put_byte(f, QEMU_VM_CONFIGURATION);
        vmstate_save_state(f, &vmstate_configuration, &savevm_state, 0);
    }

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->ops || !se->ops->save_live_setup) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        save_section_header(f, se, QEMU_VM_SECTION_START);

        ret = se->ops->save_live_setup(f, se->opaque);
        save_section_footer(f, se);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
            break;
        }
    }
}

/*
 * this function has three return values:
 *   negative: there was one error, and we have -errno.
 *   0 : We haven't finished, caller have to go again
 *   1 : We have finished, we can go to complete phase
 */
int qemu_savevm_state_iterate(QEMUFile *f)
{
    SaveStateEntry *se;
    int ret = 1;

    trace_savevm_state_iterate();
    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->ops || !se->ops->save_live_iterate) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        if (qemu_file_rate_limit(f)) {
            return 0;
        }
        trace_savevm_section_start(se->idstr, se->section_id);

        save_section_header(f, se, QEMU_VM_SECTION_PART);

        ret = se->ops->save_live_iterate(f, se->opaque);
        trace_savevm_section_end(se->idstr, se->section_id, ret);
        save_section_footer(f, se);

        if (ret < 0) {
            qemu_file_set_error(f, ret);
        }
        if (ret <= 0) {
            /* Do not proceed to the next vmstate before this one reported
               completion of the current stage. This serializes the migration
               and reduces the probability that a faster changing state is
               synchronized over and over again. */
            break;
        }
    }
    return ret;
}

static bool should_send_vmdesc(void)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    return !machine->suppress_vmdesc;
}

void qemu_savevm_state_complete(QEMUFile *f)
{
    QJSON *vmdesc;
    int vmdesc_len;
    SaveStateEntry *se;
    int ret;

    trace_savevm_state_complete();

    cpu_synchronize_all_states();

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->ops || !se->ops->save_live_complete) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        trace_savevm_section_start(se->idstr, se->section_id);

        save_section_header(f, se, QEMU_VM_SECTION_END);

        ret = se->ops->save_live_complete(f, se->opaque);
        trace_savevm_section_end(se->idstr, se->section_id, ret);
        save_section_footer(f, se);
        if (ret < 0) {
            qemu_file_set_error(f, ret);
            return;
        }
    }

    vmdesc = qjson_new();
    json_prop_int(vmdesc, "page_size", TARGET_PAGE_SIZE);
    json_start_array(vmdesc, "devices");
    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {

        if ((!se->ops || !se->ops->save_state) && !se->vmsd) {
            continue;
        }
        if (se->vmsd && !vmstate_save_needed(se->vmsd, se->opaque)) {
            trace_savevm_section_skip(se->idstr, se->section_id);
            continue;
        }

        trace_savevm_section_start(se->idstr, se->section_id);

        json_start_object(vmdesc, NULL);
        json_prop_str(vmdesc, "name", se->idstr);
        json_prop_int(vmdesc, "instance_id", se->instance_id);

        save_section_header(f, se, QEMU_VM_SECTION_FULL);

        vmstate_save(f, se, vmdesc);

        json_end_object(vmdesc);
        trace_savevm_section_end(se->idstr, se->section_id, 0);
        save_section_footer(f, se);
    }

    qemu_put_byte(f, QEMU_VM_EOF);

    json_end_array(vmdesc);
    qjson_finish(vmdesc);
    vmdesc_len = strlen(qjson_get_str(vmdesc));

    if (should_send_vmdesc()) {
        qemu_put_byte(f, QEMU_VM_VMDESCRIPTION);
        qemu_put_be32(f, vmdesc_len);
        qemu_put_buffer(f, (uint8_t *)qjson_get_str(vmdesc), vmdesc_len);
    }
    object_unref(OBJECT(vmdesc));

    qemu_fflush(f);
}

uint64_t qemu_savevm_state_pending(QEMUFile *f, uint64_t max_size)
{
    SaveStateEntry *se;
    uint64_t ret = 0;

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!se->ops || !se->ops->save_live_pending) {
            continue;
        }
        if (se->ops && se->ops->is_active) {
            if (!se->ops->is_active(se->opaque)) {
                continue;
            }
        }
        ret += se->ops->save_live_pending(f, se->opaque, max_size);
    }
    return ret;
}

void qemu_savevm_state_cancel(void)
{
    SaveStateEntry *se;

    trace_savevm_state_cancel();
    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (se->ops && se->ops->cancel) {
            se->ops->cancel(se->opaque);
        }
    }
}

static int qemu_savevm_state(QEMUFile *f, Error **errp)
{
    int ret;
    MigrationParams params = {
        .blk = 0,
        .shared = 0
    };

    if (qemu_savevm_state_blocked(errp)) {
        return -EINVAL;
    }

    qemu_mutex_unlock_iothread();
    qemu_savevm_state_header(f);
    qemu_savevm_state_begin(f, &params);
    qemu_mutex_lock_iothread();

    while (qemu_file_get_error(f) == 0) {
        if (qemu_savevm_state_iterate(f) > 0) {
            break;
        }
    }

    ret = qemu_file_get_error(f);
    if (ret == 0) {
        qemu_savevm_state_complete(f);
        ret = qemu_file_get_error(f);
    }
    if (ret != 0) {
        qemu_savevm_state_cancel();
        error_setg_errno(errp, -ret, "Error while writing VM state");
    }
    return ret;
}

static int qemu_save_device_state(QEMUFile *f)
{
    SaveStateEntry *se;

    qemu_put_be32(f, QEMU_VM_FILE_MAGIC);
    qemu_put_be32(f, QEMU_VM_FILE_VERSION);

    cpu_synchronize_all_states();

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (se->is_ram) {
            continue;
        }
        if ((!se->ops || !se->ops->save_state) && !se->vmsd) {
            continue;
        }
        if (se->vmsd && !vmstate_save_needed(se->vmsd, se->opaque)) {
            continue;
        }

        save_section_header(f, se, QEMU_VM_SECTION_FULL);

        vmstate_save(f, se, NULL);

        save_section_footer(f, se);
    }

    qemu_put_byte(f, QEMU_VM_EOF);

    return qemu_file_get_error(f);
}

static SaveStateEntry *find_se(const char *idstr, int instance_id)
{
    SaveStateEntry *se;

    QTAILQ_FOREACH(se, &savevm_state.handlers, entry) {
        if (!strcmp(se->idstr, idstr) &&
            (instance_id == se->instance_id ||
             instance_id == se->alias_id))
            return se;
        /* Migrating from an older version? */
        if (strstr(se->idstr, idstr) && se->compat) {
            if (!strcmp(se->compat->idstr, idstr) &&
                (instance_id == se->compat->instance_id ||
                 instance_id == se->alias_id))
                return se;
        }
    }
    return NULL;
}

struct LoadStateEntry {
    QLIST_ENTRY(LoadStateEntry) entry;
    SaveStateEntry *se;
    int section_id;
    int version_id;
};

/*
 * Read a footer off the wire and check that it matches the expected section
 *
 * Returns: true if the footer was good
 *          false if there is a problem (and calls error_report to say why)
 */
static bool check_section_footer(QEMUFile *f, LoadStateEntry *le)
{
    uint8_t read_mark;
    uint32_t read_section_id;

    if (skip_section_footers) {
        /* No footer to check */
        return true;
    }

    read_mark = qemu_get_byte(f);

    if (read_mark != QEMU_VM_SECTION_FOOTER) {
        error_report("Missing section footer for %s", le->se->idstr);
        return false;
    }

    read_section_id = qemu_get_be32(f);
    if (read_section_id != le->section_id) {
        error_report("Mismatched section id in footer for %s -"
                     " read 0x%x expected 0x%x",
                     le->se->idstr, read_section_id, le->section_id);
        return false;
    }

    /* All good */
    return true;
}

void loadvm_free_handlers(MigrationIncomingState *mis)
{
    LoadStateEntry *le, *new_le;

    QLIST_FOREACH_SAFE(le, &mis->loadvm_handlers, entry, new_le) {
        QLIST_REMOVE(le, entry);
        g_free(le);
    }
}

int qemu_loadvm_state(QEMUFile *f)
{
    MigrationIncomingState *mis = migration_incoming_get_current();
    Error *local_err = NULL;
    uint8_t section_type;
    unsigned int v;
    int ret;
    int file_error_after_eof = -1;

    if (qemu_savevm_state_blocked(&local_err)) {
        error_report_err(local_err);
        return -EINVAL;
    }

    v = qemu_get_be32(f);
    if (v != QEMU_VM_FILE_MAGIC) {
        error_report("Not a migration stream");
        return -EINVAL;
    }

    v = qemu_get_be32(f);
    if (v == QEMU_VM_FILE_VERSION_COMPAT) {
        error_report("SaveVM v2 format is obsolete and don't work anymore");
        return -ENOTSUP;
    }
    if (v != QEMU_VM_FILE_VERSION) {
        error_report("Unsupported migration stream version");
        return -ENOTSUP;
    }

    if (!savevm_state.skip_configuration) {
        if (qemu_get_byte(f) != QEMU_VM_CONFIGURATION) {
            error_report("Configuration section missing");
            return -EINVAL;
        }
        ret = vmstate_load_state(f, &vmstate_configuration, &savevm_state, 0);

        if (ret) {
            return ret;
        }
    }

    while ((section_type = qemu_get_byte(f)) != QEMU_VM_EOF) {
        uint32_t instance_id, version_id, section_id;
        SaveStateEntry *se;
        LoadStateEntry *le;
        char idstr[256];

        trace_qemu_loadvm_state_section(section_type);
        switch (section_type) {
        case QEMU_VM_SECTION_START:
        case QEMU_VM_SECTION_FULL:
            /* Read section start */
            section_id = qemu_get_be32(f);
            if (!qemu_get_counted_string(f, idstr)) {
                error_report("Unable to read ID string for section %u",
                            section_id);
                return -EINVAL;
            }
            instance_id = qemu_get_be32(f);
            version_id = qemu_get_be32(f);

            trace_qemu_loadvm_state_section_startfull(section_id, idstr,
                                                      instance_id, version_id);
            /* Find savevm section */
            se = find_se(idstr, instance_id);
            if (se == NULL) {
                error_report("Unknown savevm section or instance '%s' %d",
                             idstr, instance_id);
                ret = -EINVAL;
                goto out;
            }

            /* Validate version */
            if (version_id > se->version_id) {
                error_report("savevm: unsupported version %d for '%s' v%d",
                             version_id, idstr, se->version_id);
                ret = -EINVAL;
                goto out;
            }

            /* Add entry */
            le = g_malloc0(sizeof(*le));

            le->se = se;
            le->section_id = section_id;
            le->version_id = version_id;
            QLIST_INSERT_HEAD(&mis->loadvm_handlers, le, entry);

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                error_report("error while loading state for instance 0x%x of"
                             " device '%s'", instance_id, idstr);
                goto out;
            }
            if (!check_section_footer(f, le)) {
                ret = -EINVAL;
                goto out;
            }
            break;
        case QEMU_VM_SECTION_PART:
        case QEMU_VM_SECTION_END:
            section_id = qemu_get_be32(f);

            trace_qemu_loadvm_state_section_partend(section_id);
            QLIST_FOREACH(le, &mis->loadvm_handlers, entry) {
                if (le->section_id == section_id) {
                    break;
                }
            }
            if (le == NULL) {
                error_report("Unknown savevm section %d", section_id);
                ret = -EINVAL;
                goto out;
            }

            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                error_report("error while loading state section id %d(%s)",
                             section_id, le->se->idstr);
                goto out;
            }
            if (!check_section_footer(f, le)) {
                ret = -EINVAL;
                goto out;
            }
            break;
        default:
            error_report("Unknown savevm section type %d", section_type);
            ret = -EINVAL;
            goto out;
        }
    }

    file_error_after_eof = qemu_file_get_error(f);

    /*
     * Try to read in the VMDESC section as well, so that dumping tools that
     * intercept our migration stream have the chance to see it.
     */

    /* We've got to be careful; if we don't read the data and just shut the fd
     * then the sender can error if we close while it's still sending.
     * We also mustn't read data that isn't there; some transports (RDMA)
     * will stall waiting for that data when the source has already closed.
     */
    if (should_send_vmdesc()) {
        uint8_t *buf;
        uint32_t size;
        section_type = qemu_get_byte(f);

        if (section_type != QEMU_VM_VMDESCRIPTION) {
            error_report("Expected vmdescription section, but got %d",
                         section_type);
            /*
             * It doesn't seem worth failing at this point since
             * we apparently have an otherwise valid VM state
             */
        } else {
            buf = g_malloc(0x1000);
            size = qemu_get_be32(f);

            while (size > 0) {
                uint32_t read_chunk = MIN(size, 0x1000);
                qemu_get_buffer(f, buf, read_chunk);
                size -= read_chunk;
            }
            g_free(buf);
        }
    }

    cpu_synchronize_all_post_init();

    ret = 0;

out:
    if (ret == 0) {
        /* We may not have a VMDESC section, so ignore relative errors */
        ret = file_error_after_eof;
    }

    return ret;
}

static BlockDriverState *find_vmstate_bs(void)
{
    BlockDriverState *bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs)) {
            return bs;
        }
    }
    return NULL;
}

/*
 * Deletes snapshots of a given name in all opened images.
 */
static int del_existing_snapshots(Monitor *mon, const char *name)
{
    BlockDriverState *bs;
    QEMUSnapshotInfo sn1, *snapshot = &sn1;
    Error *err = NULL;

    bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs) &&
            bdrv_snapshot_find(bs, snapshot, name) >= 0) {
            bdrv_snapshot_delete_by_id_or_name(bs, name, &err);
            if (err) {
                monitor_printf(mon,
                               "Error while deleting snapshot on device '%s':"
                               " %s\n",
                               bdrv_get_device_name(bs),
                               error_get_pretty(err));
                error_free(err);
                return -1;
            }
        }
    }

    return 0;
}

void hmp_savevm(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo sn1, *sn = &sn1, old_sn1, *old_sn = &old_sn1;
    int ret;
    QEMUFile *f;
    int saved_vm_running;
    uint64_t vm_state_size;
    qemu_timeval tv;
    struct tm tm;
    const char *name = qdict_get_try_str(qdict, "name");
    Error *local_err = NULL;

    /* Verify if there is a device that doesn't support snapshots and is writable */
    bs = NULL;
    while ((bs = bdrv_next(bs))) {

        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
            continue;
        }

        if (!bdrv_can_snapshot(bs)) {
            monitor_printf(mon, "Device '%s' is writable but does not support snapshots.\n",
                               bdrv_get_device_name(bs));
            return;
        }
    }

    bs = find_vmstate_bs();
    if (!bs) {
        monitor_printf(mon, "No block device can accept snapshots\n");
        return;
    }

    saved_vm_running = runstate_is_running();

    ret = global_state_store();
    if (ret) {
        monitor_printf(mon, "Error saving global state\n");
        return;
    }
    vm_stop(RUN_STATE_SAVE_VM);

    memset(sn, 0, sizeof(*sn));

    /* fill auxiliary fields */
    qemu_gettimeofday(&tv);
    sn->date_sec = tv.tv_sec;
    sn->date_nsec = tv.tv_usec * 1000;
    sn->vm_clock_nsec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (name) {
        ret = bdrv_snapshot_find(bs, old_sn, name);
        if (ret >= 0) {
            pstrcpy(sn->name, sizeof(sn->name), old_sn->name);
            pstrcpy(sn->id_str, sizeof(sn->id_str), old_sn->id_str);
        } else {
            pstrcpy(sn->name, sizeof(sn->name), name);
        }
    } else {
        /* cast below needed for OpenBSD where tv_sec is still 'long' */
        localtime_r((const time_t *)&tv.tv_sec, &tm);
        strftime(sn->name, sizeof(sn->name), "vm-%Y%m%d%H%M%S", &tm);
    }

    /* Delete old snapshots of the same name */
    if (name && del_existing_snapshots(mon, name) < 0) {
        goto the_end;
    }

    /* save the VM state */
    f = qemu_fopen_bdrv(bs, 1);
    if (!f) {
        monitor_printf(mon, "Could not open VM state file\n");
        goto the_end;
    }
    ret = qemu_savevm_state(f, &local_err);
    vm_state_size = qemu_ftell(f);
    qemu_fclose(f);
    if (ret < 0) {
        monitor_printf(mon, "%s\n", error_get_pretty(local_err));
        error_free(local_err);
        goto the_end;
    }

    /* create the snapshots */

    bs1 = NULL;
    while ((bs1 = bdrv_next(bs1))) {
        if (bdrv_can_snapshot(bs1)) {
            /* Write VM state size only to the image that contains the state */
            sn->vm_state_size = (bs == bs1 ? vm_state_size : 0);
            ret = bdrv_snapshot_create(bs1, sn);
            if (ret < 0) {
                monitor_printf(mon, "Error while creating snapshot on '%s'\n",
                               bdrv_get_device_name(bs1));
            }
        }
    }

 the_end:
    if (saved_vm_running) {
        vm_start();
    }
}

void qmp_xen_save_devices_state(const char *filename, Error **errp)
{
    QEMUFile *f;
    int saved_vm_running;
    int ret;

    saved_vm_running = runstate_is_running();
    vm_stop(RUN_STATE_SAVE_VM);

    f = qemu_fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        goto the_end;
    }
    ret = qemu_save_device_state(f);
    qemu_fclose(f);
    if (ret < 0) {
        error_setg(errp, QERR_IO_ERROR);
    }

 the_end:
    if (saved_vm_running) {
        vm_start();
    }
}

int load_vmstate(const char *name)
{
    BlockDriverState *bs, *bs_vm_state;
    QEMUSnapshotInfo sn;
    QEMUFile *f;
    int ret;

    bs_vm_state = find_vmstate_bs();
    if (!bs_vm_state) {
        error_report("No block device supports snapshots");
        return -ENOTSUP;
    }

    /* Don't even try to load empty VM states */
    ret = bdrv_snapshot_find(bs_vm_state, &sn, name);
    if (ret < 0) {
        return ret;
    } else if (sn.vm_state_size == 0) {
        error_report("This is a disk-only snapshot. Revert to it offline "
            "using qemu-img.");
        return -EINVAL;
    }

    /* Verify if there is any device that doesn't support snapshots and is
    writable and check if the requested snapshot is available too. */
    bs = NULL;
    while ((bs = bdrv_next(bs))) {

        if (!bdrv_is_inserted(bs) || bdrv_is_read_only(bs)) {
            continue;
        }

        if (!bdrv_can_snapshot(bs)) {
            error_report("Device '%s' is writable but does not support snapshots.",
                               bdrv_get_device_name(bs));
            return -ENOTSUP;
        }

        ret = bdrv_snapshot_find(bs, &sn, name);
        if (ret < 0) {
            error_report("Device '%s' does not have the requested snapshot '%s'",
                           bdrv_get_device_name(bs), name);
            return ret;
        }
    }

    /* Flush all IO requests so they don't interfere with the new state.  */
    bdrv_drain_all();

    bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs)) {
            ret = bdrv_snapshot_goto(bs, name);
            if (ret < 0) {
                error_report("Error %d while activating snapshot '%s' on '%s'",
                             ret, name, bdrv_get_device_name(bs));
                return ret;
            }
        }
    }

    /* restore the VM state */
    f = qemu_fopen_bdrv(bs_vm_state, 0);
    if (!f) {
        error_report("Could not open VM state file");
        return -EINVAL;
    }

    qemu_system_reset(VMRESET_SILENT);
    migration_incoming_state_new(f);
    ret = qemu_loadvm_state(f);

    qemu_fclose(f);
    migration_incoming_state_destroy();
    if (ret < 0) {
        error_report("Error %d while loading VM state", ret);
        return ret;
    }

    return 0;
}

void hmp_delvm(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs;
    Error *err;
    const char *name = qdict_get_str(qdict, "name");

    if (!find_vmstate_bs()) {
        monitor_printf(mon, "No block device supports snapshots\n");
        return;
    }

    bs = NULL;
    while ((bs = bdrv_next(bs))) {
        if (bdrv_can_snapshot(bs)) {
            err = NULL;
            bdrv_snapshot_delete_by_id_or_name(bs, name, &err);
            if (err) {
                monitor_printf(mon,
                               "Error while deleting snapshot on device '%s':"
                               " %s\n",
                               bdrv_get_device_name(bs),
                               error_get_pretty(err));
                error_free(err);
            }
        }
    }
}

void hmp_info_snapshots(Monitor *mon, const QDict *qdict)
{
    BlockDriverState *bs, *bs1;
    QEMUSnapshotInfo *sn_tab, *sn, s, *sn_info = &s;
    int nb_sns, i, ret, available;
    int total;
    int *available_snapshots;

    bs = find_vmstate_bs();
    if (!bs) {
        monitor_printf(mon, "No available block device supports snapshots\n");
        return;
    }

    nb_sns = bdrv_snapshot_list(bs, &sn_tab);
    if (nb_sns < 0) {
        monitor_printf(mon, "bdrv_snapshot_list: error %d\n", nb_sns);
        return;
    }

    if (nb_sns == 0) {
        monitor_printf(mon, "There is no snapshot available.\n");
        return;
    }

    available_snapshots = g_malloc0(sizeof(int) * nb_sns);
    total = 0;
    for (i = 0; i < nb_sns; i++) {
        sn = &sn_tab[i];
        available = 1;
        bs1 = NULL;

        while ((bs1 = bdrv_next(bs1))) {
            if (bdrv_can_snapshot(bs1) && bs1 != bs) {
                ret = bdrv_snapshot_find(bs1, sn_info, sn->id_str);
                if (ret < 0) {
                    available = 0;
                    break;
                }
            }
        }

        if (available) {
            available_snapshots[total] = i;
            total++;
        }
    }

    if (total > 0) {
        bdrv_snapshot_dump((fprintf_function)monitor_printf, mon, NULL);
        monitor_printf(mon, "\n");
        for (i = 0; i < total; i++) {
            sn = &sn_tab[available_snapshots[i]];
            bdrv_snapshot_dump((fprintf_function)monitor_printf, mon, sn);
            monitor_printf(mon, "\n");
        }
    } else {
        monitor_printf(mon, "There is no suitable snapshot available\n");
    }

    g_free(sn_tab);
    g_free(available_snapshots);

}

void vmstate_register_ram(MemoryRegion *mr, DeviceState *dev)
{
    qemu_ram_set_idstr(memory_region_get_ram_addr(mr) & TARGET_PAGE_MASK,
                       memory_region_name(mr), dev);
}

void vmstate_unregister_ram(MemoryRegion *mr, DeviceState *dev)
{
    qemu_ram_unset_idstr(memory_region_get_ram_addr(mr) & TARGET_PAGE_MASK);
}

void vmstate_register_ram_global(MemoryRegion *mr)
{
    vmstate_register_ram(mr, NULL);
}
