/*
   Copyright (c) 2019 Kadalu.IO <https://kadalu.io>
   This file is part of GlusterFS.

   This file is licensed to you under your choice of the GNU Lesser
   General Public License, version 3 or any later version (LGPLv3 or
   later), or the GNU General Public License, version 2 (GPLv2), in all
   cases as published by the Free Software Foundation.
*/

#ifndef _GF_PRJQUOTA_H_
#define _GF_PRJQUOTA_H_

#include <glusterfs/xlator.h>

#define PRJQ_LIMIT_XATTR_KEY "trusted.gf.prjquota.limit"
#define PRJQ_LIMIT_VIRT_XATTR_KEY "prjquota.limit"

enum prjq_mem_types_ {
    prjq_mt_priv_t = gf_common_mt_end + 1,
    prjq_mt_ctx_t,
    prjq_mt_end
};

typedef struct prjq_priv {
    gf_lock_t lock;    /* for list add, delete */
    uint64_t blk_size; /* backend fs blk_size... helps in sending back statfs()
                          response */
    struct list_head qlist; /* list of all the entries, used for dumping */
    char *config_file;      /* file to dump quota config */
                            // int config_fd;
} prjq_priv_t;

typedef struct prjq_ctx {
    struct list_head list; /* Link to the global list */
    uint64_t limit;        /* user setting limit */
    gf_atomic_t used;      /* updated on write, entry creates etc */
    gf_atomic_t ref; /* every inode setting it in the ctx would have a ref */
    gf_lock_t lock;  /* for internal */
    uuid_t gfid;     /* GFID: used for dumping, and repopulate */
    char *path; /* Path of the quota set, used for dumping in config file */
} prjq_ctx_t;

#endif /* PRJQUOTA_H */
