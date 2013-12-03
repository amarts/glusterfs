/*
  (C) 2006 Gluster core team <http://www.gluster.org/>
  
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

#ifndef _XLATOR_H
#define _XLATOR_H

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "glusterfs.h"
#include "layout.h"
#include "common-utils.h"

struct xlator;
struct _layout_t;

struct file_context {
  struct file_context *next;
  struct xlator *volume;
  int8_t path[PATH_MAX];
  void *context;
};

#define FILL_MY_CTX(tmp, ctx, xl)  do {\
  tmp = ctx->next;\
  while (tmp != NULL && tmp->volume != xl) \
    tmp = tmp->next; \
} while (0)

/* just detach node from link list and free the context */
#define RM_MY_CTX(ctx, tmp) do {        \
  while (ctx && ctx->next != tmp)       \
    ctx = ctx->next;                    \
  if (ctx && ctx->next == tmp)          \
    ctx->next = tmp->next;              \
} while (0)

/* required for bulk_getattr call */
struct bulk_stat {
  struct stat *stbuf;
  int8_t *pathname;
  struct bulk_stat *next;
};

struct xlator_stats {
  uint64_t nr_files;   /* Number of files open via this xlator */
  uint64_t free_disk; /* Mega bytes */
  uint64_t disk_usage; /* Mega bytes */
  uint64_t disk_speed; /* MHz or Mbps */
  uint64_t nr_clients; /* Number of client nodes (filled by glusterfsd) */
  uint64_t write_usage;
  uint64_t read_usage;
  /* add more stats here */
};

struct xlator_mgmt_ops {
  int32_t (*stats) (struct xlator *this, struct xlator_stats *stats);
  int32_t (*fsck) (struct xlator *this);
  int32_t (*lock) (struct xlator *this, const int8_t *name);
  int32_t (*unlock) (struct xlator *this, const int8_t *name);
  int32_t (*listlocks) (struct xlator *this);
  int32_t (*nslookup) (struct xlator *this, const int8_t *name,
		   dict_t *ns);
  int32_t (*nsupdate) (struct xlator *this, const int8_t *name,
		   dict_t *ns);
};

struct xlator_fops {
  int32_t (*open) (struct xlator *this, const int8_t *path, int32_t flags,
	       mode_t mode, struct file_context *ctx);
  int32_t (*getattr) (struct xlator *this, const int8_t *path, 
		  struct stat *stbuf);
  int32_t (*readlink) (struct xlator *this, const int8_t *path, 
		   int8_t *dest, size_t size);
  int32_t (*mknod) (struct xlator *this, const int8_t *path, 
		mode_t mode, dev_t dev, uid_t uid, gid_t gid);
  int32_t (*mkdir) (struct xlator *this, const int8_t *path,
		mode_t mode, uid_t uid, gid_t gid);
  int32_t (*unlink) (struct xlator *this, const int8_t *path);
  int32_t (*rmdir) (struct xlator *this, const int8_t *path);
  int32_t (*symlink) (struct xlator *this, const int8_t *oldpath, 
		  const int8_t *newpath, uid_t uid, gid_t gid);
  int32_t (*rename) (struct xlator *this, const int8_t *oldpath,
		 const int8_t *newpath, uid_t uid, gid_t gid);
  int32_t (*link) (struct xlator *this, const int8_t *oldpath,
	       const int8_t *newpath, uid_t uid, gid_t gid);
  int32_t (*chmod) (struct xlator *this, const int8_t *path, mode_t mode);
  int32_t (*chown) (struct xlator *this, const int8_t *path, uid_t uid, gid_t gid);
  int32_t (*truncate) (struct xlator *this, const int8_t *path, off_t offset);
  int32_t (*utime) (struct xlator *this, const int8_t *path, struct utimbuf *buf);
  int32_t (*read) (struct xlator *this, const int8_t *path, int8_t *buf, size_t size,
	       off_t offset, struct file_context *ctx);
  int32_t (*write) (struct xlator *this, const int8_t *path, const int8_t *buf, size_t size,
	       off_t offset, struct file_context *ctx);
  int32_t (*statfs) (struct xlator *this, const int8_t *path, struct statvfs *buf);
  int32_t (*flush) (struct xlator *this, const int8_t *path, 
		struct file_context *ctx);
  int32_t (*release) (struct xlator *this, const int8_t *path, 
		  struct file_context *ctx);
  int32_t (*fsync) (struct xlator *this, const int8_t *path, int32_t flags,
		struct file_context *ctx);
  int32_t (*setxattr) (struct xlator *this, const int8_t *path, const int8_t *name,
		   const int8_t *value, size_t size, int32_t flags);
  int32_t (*getxattr) (struct xlator *this, const int8_t *path, const int8_t *name,
		   int8_t *value, size_t size);
  int32_t (*listxattr) (struct xlator *this, const int8_t *path, int8_t *list, size_t size);
  int32_t (*removexattr) (struct xlator *this, const int8_t *path, const int8_t *name);
  int32_t (*opendir) (struct xlator *this, const int8_t *path, 
		  struct file_context *ctx);
  int8_t *(*readdir) (struct xlator *this, const int8_t *path, off_t offset);
  int32_t (*releasedir) (struct xlator *this, const int8_t *path,
		     struct file_context *ctx);
  int32_t (*fsyncdir) (struct xlator *this, const int8_t *path, int32_t flags, 
		   struct file_context *ctx);
  int32_t (*access) (struct xlator *this, const int8_t *path, mode_t mode);
  int32_t (*ftruncate) (struct xlator *this, const int8_t *path, off_t offset,
		    struct  file_context *ctx);
  int32_t (*fgetattr) (struct xlator *this, const int8_t *path, struct stat *buf,
		 struct file_context *ctx);
  int32_t (*bulk_getattr) (struct xlator *this, const int8_t *path, struct bulk_stat *bstbuf);
};

struct xlator {
  int8_t *name;
  struct xlator *next; /* for maintainence */
  struct xlator *parent;
  struct xlator *first_child;
  struct xlator *next_sibling;

  struct xlator_fops *fops;
  struct xlator_mgmt_ops *mgmt_ops;

  void (*fini) (struct xlator *this);
  int32_t (*init) (struct xlator *this);
  struct _layout_t * (*getlayout) (struct xlator *this, 
				   struct _layout_t *layout);
  struct _layout_t * (*setlayout) (struct xlator *this, 
				   struct _layout_t *layout);

  dict_t *options;
  void *private;
};


void xlator_set_type (struct xlator *xl, const int8_t *type);
in_addr_t resolve_ip (const int8_t *hostname);

struct xlator * file_to_xlator_tree (FILE *fp);

void xlator_foreach (struct xlator *this,
		     void (*fn) (struct xlator *each));

#define GF_STAT_PRINT_FMT_STR "%"PRIx64",%"PRIx64",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx64",%"PRIx64",%"PRIx32",%"PRIx64",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32",%"PRIx32"\n"

#define GF_STAT_SCAN_FMT_STR "%"SCNx64",%"SCNx64",%"SCNx32",%"SCNx32",%"SCNx32",%"SCNx32",%"SCNx64",%"SCNx64",%"SCNx32",%"SCNx64",%"SCNx32",%"SCNx32",%"SCNx32",%"SCNx32",%"SCNx32",%"SCNx32"\n"

#define GF_STATFS_PRINT_FMT_STR "%"PRIx32",%"PRIx32",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx32",%"PRIx32",%"PRIx32"\n"

#define GF_STATFS_SCAN_FMT_STR "%"SCNx32",%"SCNx32",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx32",%"SCNx32",%"SCNx32"\n"

#define GF_MGMT_STATS_PRINT_FMT_STR "%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64"\n"

#define GF_MGMT_STATS_SCAN_FMT_STR "%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64",%"SCNx64"\n"

#endif
