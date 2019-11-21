/*
 * Copyright (c) 2019 Kadalu Software. <http://kadalu.io>
 * This file is part of GlusterFS.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 3 or any later version (LGPLv3 or
 * later), or the GNU General Public License, version 2 (GPLv2), in all
 * cases as published by the Free Software Foundation.
 *
 * xlators/features/dsmds:
 *    This translator separates fops into meta-data and data classification,
 *    and sends the operations to relevant subvolume
 */

#ifndef __DSMDS_H__
#define __DSMDS_H__

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include <glusterfs/xlator.h>
#include <glusterfs/call-stub.h>

typedef struct {
    xlator_t *mds;
    xlator_t *ds;
} dsmds_private_t;

typedef struct {
    loc_t orig_loc;
    loc_t ds_loc;
    char ds_path[GF_UUID_BUF_SIZE + 10];

    struct iatt ds_stbuf;
    struct iatt mds_stbuf;

    struct statvfs mds_statvfs;
    struct statvfs ds_statvfs;

  dict_t *xdata_req;
  dict_t *xdata_rsp;

    inode_t *inode;
    int callcnt;
    int op_ret;
    int op_errno;
} dsmds_local_t;

#define DSMDS_STACK_UNWIND(fop, frame, params...)     \
  do {									\
        dsmds_local_t *__local = NULL;                                           \
        xlator_t *__xl = NULL;                                                 \
        if (frame) {                                                           \
            __xl = frame->this;                                                \
            __local = frame->local;                                            \
            frame->local = NULL;                                               \
        }                                                                      \
        STACK_UNWIND_STRICT(fop, frame, params);                               \
        dsmds_local_wipe(__xl, __local);                                         \
    } while (0)

#endif /* __DSMDS_H__ */
