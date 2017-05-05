/*
  Copyright (c) 2010-2011 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/


#ifndef STATEDUMP_H
#define STATEDUMP_H

#include <stdarg.h>
#include "inode.h"

#define GF_DUMP_MAX_BUF_LEN 4096

typedef struct gf_dump_xl_options_ {
        gf_boolean_t    dump_priv;
        gf_boolean_t    dump_inode;
        gf_boolean_t    dump_fd;
        gf_boolean_t    dump_inodectx;
        gf_boolean_t    dump_fdctx;
        gf_boolean_t    dump_history;
} gf_dump_xl_options_t;

typedef struct gf_dump_options_ {
        gf_boolean_t            dump_mem;
        gf_boolean_t            dump_iobuf;
        gf_boolean_t            dump_callpool;
        gf_dump_xl_options_t    xl_options; //options for all xlators
} gf_dump_options_t;

extern gf_dump_options_t dump_options;

static inline
void _gf_proc_dump_build_key (char *key, const char *prefix, char *fmt,...)
{
        char buf[GF_DUMP_MAX_BUF_LEN];
        va_list ap;

        memset(buf, 0, sizeof(buf));
        va_start(ap, fmt);
        vsnprintf(buf, GF_DUMP_MAX_BUF_LEN, fmt, ap);
        va_end(ap);
        snprintf(key, GF_DUMP_MAX_BUF_LEN, "%s.%s", prefix, buf);
}

#define gf_proc_dump_build_key(key, key_prefix, fmt...)                 \
        {                                                               \
                _gf_proc_dump_build_key(key, key_prefix, ##fmt);        \
        }

#define GF_PROC_DUMP_SET_OPTION(opt,val) opt = val

void gf_proc_dump_init();

void gf_proc_dump_fini(void);

void gf_proc_dump_cleanup(void);

void gf_proc_dump_info(int signum);

int gf_proc_dump_add_section(char *key,...);

int gf_proc_dump_write(char *key, char *value,...);

void inode_table_dump(inode_table_t *itable, char *prefix);

void inode_table_dump_to_dict (inode_table_t *itable, char *prefix, dict_t *dict);

void fdtable_dump(fdtable_t *fdtable, char *prefix);

void fdtable_dump_to_dict (fdtable_t *fdtable, char *prefix, dict_t *dict);

void inode_dump(inode_t *inode, char *prefix);

void gf_proc_dump_mem_info_to_dict (dict_t *dict);

void gf_proc_dump_mempool_info_to_dict (glusterfs_ctx_t *ctx, dict_t *dict);

void glusterd_init (int sig);
#endif /* STATEDUMP_H */
