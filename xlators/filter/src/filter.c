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


#include "glusterfs.h"
#include "filter.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"

/*
  This filter currently only makes its child read-only.
  In the future it'll be extended to handle other types of filtering
  (filtering certain file types, for example)
*/

/* Calls which return at this level */

static int32_t
filter_mknod (call_frame_t *frame,
	      xlator_t *this,
	      const char *path,
	      mode_t mode,
	      dev_t dev)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}

static int32_t 
filter_mkdir (call_frame_t *frame,
	      xlator_t *this,
	      const char *path,
	      mode_t mode)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t
filter_unlink (call_frame_t *frame,
	       xlator_t *this,
	       const char *path)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}


static int32_t 
filter_rmdir (call_frame_t *frame,
	      xlator_t *this,
	      const char *path)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}

static int32_t
filter_symlink (call_frame_t *frame,
		xlator_t *this,
		const char *oldpath,
		const char *newpath)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}

static int32_t
filter_rename (call_frame_t *frame,
	       xlator_t *this,
	       const char *oldpath,
	       const char *newpath)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}

static int32_t
filter_link (call_frame_t *frame,
	     xlator_t *this,
	     const char *oldpath,
	     const char *newpath)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t 
filter_chmod (call_frame_t *frame,
	      xlator_t *this,
	      const char *path,
	      mode_t mode)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t 
filter_chown (call_frame_t *frame,
	      xlator_t *this,
	      const char *path,
	      uid_t uid,
	      gid_t gid)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t 
filter_truncate (call_frame_t *frame,
		 xlator_t *this,
		 const char *path,
		 off_t offset)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t 
filter_utimes (call_frame_t *frame,
	       xlator_t *this,
	       const char *path,
	       struct timespec *tvp)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t 
filter_writev (call_frame_t *frame,
	       xlator_t *this,
	       dict_t *ctx,
	       struct iovec *vector,
	       int32_t count,
	       off_t offset)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}


static int32_t 
filter_flush (call_frame_t *frame,
	      xlator_t *this,
	      dict_t *file_ctx)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}

int32_t 
filter_fsync (call_frame_t *frame,
	      xlator_t *this,
	      dict_t *file_ctx,
	      int32_t datasync)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}

static int32_t 
filter_setxattr (call_frame_t *frame,
		 xlator_t *this,
		 const char *path,
		 const char *name,
		 const char *value,
		 size_t size,
		 int32_t flags)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}
		     
static int32_t 
filter_removexattr (call_frame_t *frame,
		    xlator_t *this,
		    const char *path,
		    const char *name)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}


static int32_t 
filter_fsyncdir (call_frame_t *frame,
		 xlator_t *this,
		 dict_t *file_ctx,
		 int32_t datasync)
{
  STACK_UNWIND (frame, -1, EROFS);
  return 0;
}

static int32_t 
filter_ftruncate (call_frame_t *frame,
		  xlator_t *this,
		  dict_t *file_ctx,
		  off_t offset)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t
filter_open_cbk (call_frame_t *frame,
		 call_frame_t *prev_frame,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 dict_t *file_ctx,
		 struct stat *buf)
{
  STACK_UNWIND (frame, op_ret, op_errno, file_ctx, buf);
  return 0;
}

static int32_t 
filter_open (call_frame_t *frame,
	     xlator_t *this,
	     const char *path,
	     int32_t flags,
	     mode_t mode)
{
  if ((flags & O_WRONLY) || (flags & O_RDWR)) {
    struct stat buf = {0, };
    STACK_UNWIND (frame, -1, EROFS, &buf);
    return 0;
  }
  
  STACK_WIND (frame,
	      filter_open_cbk,
	      this->first_child,
	      this->first_child->fops->open,
	      path,
	      flags,
	      mode);

  return 0;
}

static int32_t 
filter_create (call_frame_t *frame,
	       xlator_t *this,
	       const char *path,
	       mode_t mode)
{
  struct stat buf = {0, };
  STACK_UNWIND (frame, -1, EROFS, &buf);
  return 0;
}


static int32_t
filter_access_cbk (call_frame_t *frame,
		   call_frame_t *prev_frame,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
filter_access (call_frame_t *frame,
	       xlator_t *this,
	       const char *path,
	       mode_t mode)
{
  if (mode & W_OK) {
    STACK_UNWIND (frame, -1, EROFS);
    return 0;
  }
    
  STACK_WIND (frame,
	      filter_access_cbk,
	      this->first_child,
	      this->first_child->fops->access,
	      path,
	      mode);
  return 0;
}

int32_t 
init (xlator_t *this)
{

  if (!this->first_child || this->first_child->next_sibling) {
    gf_log ("filter",
	    GF_LOG_ERROR,
	    "FATAL: xlator (%s) not configured with exactly one child",
	    this->name);
    return -1;
  }
    
  return 0;
}

void
fini (xlator_t *xl)
{

  return;
}


struct xlator_fops fops = {
  .mknod       = filter_mknod,
  .mkdir       = filter_mkdir,
  .unlink      = filter_unlink,
  .rmdir       = filter_rmdir,
  .symlink     = filter_symlink,
  .rename      = filter_rename,
  .link        = filter_link,
  .chmod       = filter_chmod,
  .chown       = filter_chown,
  .truncate    = filter_truncate,
  .utimes      = filter_utimes,
  .open        = filter_open,
  .create      = filter_create,
  .writev      = filter_writev,
  .flush       = filter_flush,
  .fsync       = filter_fsync,
  .setxattr    = filter_setxattr,
  .removexattr = filter_removexattr,
  .fsyncdir    = filter_fsyncdir,
  .access      = filter_access,
  .ftruncate   = filter_ftruncate,
};

struct xlator_mops mops = {

};
