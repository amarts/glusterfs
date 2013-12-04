/*
  (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License aint64_t with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#ifndef _UNIFY_H
#define _UNIFY_H

#include "scheduler.h"
#include "list.h"

#define MAX_DIR_ENTRY_STRING     (32 * 1024)

#define NS(xl)          (((unify_private_t *)xl->private)->namespace)


/* This is used to allocate memory for local structure */
#define INIT_LOCAL(fr, loc)                   \
do {                                          \
  loc = calloc (1, sizeof (unify_local_t));   \
  if (!loc) {                                 \
    STACK_UNWIND (fr, -1, ENOMEM);            \
    return 0;                                 \
  }                                           \
  fr->local = loc;                            \
  loc->op_ret = -1;                           \
  loc->op_errno = ENOENT;                     \
  LOCK_INIT (&fr->mutex);                     \
} while (0)

#define UNIFY_INODE_COUNT 100

struct unify_private {
  /* Update this structure depending on requirement */
  void *scheduler;               /* THIS SHOULD BE THE FIRST VARIABLE, 
				    if xlator is using scheduler */
  struct sched_ops *sched_ops;   /* Scheduler options  */
  xlator_t *namespace;           /* ptr to namespace xlator */
  int32_t child_count;
  int32_t self_heal;
  uint64_t inode_generation;
  pthread_mutex_t mutex;
};
typedef struct unify_private unify_private_t;

struct _unify_local_t {
  int32_t call_count;
  int32_t op_ret;
  int32_t op_errno;
  mode_t mode;
  off_t offset;
  dev_t dev;
  uid_t uid;
  gid_t gid;
  int32_t flags;
  dir_entry_t *ns_entry;   /* Namespace entries */
  dir_entry_t *entry;
  dir_entry_t *last;
  int32_t count;    // dir_entry_t count;
  int32_t ns_count;    // dir_entry_t count for namespace entry;
  fd_t *fd;
  struct stat stbuf;
  struct statvfs statvfs_buf;
  struct timespec tv[2];
  char *path;
  char *name;
  inode_t *inode;
  inode_t *new_inode; /* Only used in case of rename */
  int32_t create_inode;
  int32_t revalidate;

  off_t st_size;
  blkcnt_t st_blocks;
  nlink_t st_nlink;

  struct list_head *list;
  int32_t failed;
};
typedef struct _unify_local_t unify_local_t;

struct unify_inode_list {
  struct list_head list_head;
  xlator_t *xl;
  inode_t *inode;
};
typedef struct unify_inode_list unify_inode_list_t;

int32_t unify_readdir_self_heal (call_frame_t *frame,
				 xlator_t *this,
				 fd_t *fd,
				 unify_local_t *local);

int32_t gf_unify_self_heal (call_frame_t *frame,
			    xlator_t *this,
			    const char *path,
			    inode_t *inode);

#endif /* _UNIFY_H */
