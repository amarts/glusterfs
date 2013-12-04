/*
   Copyright (c) 2006, 2007 Z RESEARCH, Inc. <http://www.zresearch.com>
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

/**
 * xlators/debug/trace :
 *    This translator logs all the arguments to the fops/mops and also 
 *    their _cbk functions, which later passes the call to next layer. 
 *    Very helpful translator for debugging.
 */

#include <time.h>
#include <errno.h>
#include "glusterfs.h"
#include "xlator.h"

#define ERR_EINVAL_NORETURN(cond)                \
do                                               \
  {						 \
    if ((cond))					 \
      {						 \
	gf_log ("ERROR", 			 \
		GF_LOG_ERROR, 			 \
		"%s: %s: (%s) is true", 	 \
		__FILE__, __FUNCTION__, #cond);	 \
      }                                          \
  } while (0)

extern int32_t errno;

#define _FORMAT_WARN(domain, log_level, format, args...)  printf ("__DEBUG__" format, ##args);     

typedef struct trace_private
{
  int32_t debug_flag;
} trace_private_t;



static int32_t 
trace_create_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  fd_t *fd,
		  inode_t *inode,
		  struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this);
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));

    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, fd=%p, *inode=%p), *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, fd, inode, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    
  STACK_UNWIND (frame, op_ret, op_errno, fd, inode, buf);
  return 0;
}

static int32_t 
trace_open_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno,
		fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this);

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, *fd=%p)",
	  this, op_ret, op_errno, fd);

  STACK_UNWIND (frame, op_ret, op_errno, fd);
  return 0;
}

static int32_t 
trace_stat_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno,
		struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this);

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));

    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    
    
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_readv_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct iovec *vector,
		 int32_t count,
		 struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this);

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));

    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    
  
  STACK_UNWIND (frame, op_ret, op_errno, vector, count, buf);
  return 0;
}

static int32_t 
trace_writev_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this);

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));

    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_readdir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   dir_entry_t *entries,
		   int32_t count)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, count=%d)",
	  this, op_ret, op_errno, count);
  
  STACK_UNWIND (frame, op_ret, op_errno, entries, count);
  return 0;
}

static int32_t 
trace_fsync_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_chown_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    
    
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_chmod_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_fchmod_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    
    
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_fchown_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_unlink_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_rename_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *buf)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, buf=%p)",
	  this, op_ret, op_errno, buf);
  
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_readlink_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    const char *buf)
{
  ERR_EINVAL_NORETURN (!this );
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, buf=%s)",
	  this, op_ret, op_errno, buf);
  
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

int32_t 
trace_lookup_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  inode_t *inode,
		  struct stat *buf,
		  dict_t *dict)
{
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "callid: %lld (*this=%p, op_ret=%d, op_errno=%d, inode=%p, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld})",
	    (long long) frame->root->unique, this, op_ret, op_errno, inode, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, inode, buf, dict);
  return 0;
}

int32_t 
trace_forget_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_symlink_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   inode_t *inode,
		   struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, inode=%p, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, inode, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
  return 0;
}

static int32_t 
trace_mknod_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 inode_t *inode,
		 struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, inode=%p, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, inode, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
  return 0;
}
  

static int32_t 
trace_mkdir_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 inode_t *inode,
		 struct stat *buf)
{
  ERR_EINVAL_NORETURN (!this );
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, inode=%p",
	  this, op_ret, op_errno, inode);

  STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
  return 0;
}
  
static int32_t 
trace_link_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno,
		inode_t *inode,
		struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, inode=%p, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, inode, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
  return 0;
}

static int32_t 
trace_flush_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_close_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_opendir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, fd=%p)",
	  this, op_ret, op_errno, fd);
  
  STACK_UNWIND (frame, op_ret, op_errno, fd);
  return 0;
}

static int32_t 
trace_rmdir_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_truncate_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_utimens_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_statfs_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct statvfs *buf)
{
  ERR_EINVAL_NORETURN (!this);

  if (op_ret >= 0) {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, *buf=%p {f_bsize=%u, f_frsize=%u, f_blocks=%lu, f_bfree=%lu, f_bavail=%lu, f_files=%lu, f_ffree=%lu, f_favail=%lu, f_fsid=%u, f_flag=%u, f_namemax=%u}) => ret=%d, errno=%d",
	    this, buf, buf->f_bsize, buf->f_frsize, buf->f_blocks, buf->f_bfree, buf->f_bavail, buf->f_files, buf->f_ffree, buf->f_favail, buf->f_fsid, buf->f_flag, buf->f_namemax, op_ret, op_errno);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_setxattr_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_getxattr_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    dict_t *dict)
{
  ERR_EINVAL_NORETURN (!this || !dict);

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d, dict=%p)",
	  this, op_ret, op_errno, dict);
  
  STACK_UNWIND (frame, op_ret, op_errno, dict);
  return 0;
}

static int32_t 
trace_removexattr_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_closedir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_fsyncdir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_access_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, op_ret=%d, op_errno=%d)",
	  this, op_ret, op_errno);
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
trace_ftruncate_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_fstat_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *buf)
{
  char atime_buf[256], mtime_buf[256], ctime_buf[256];
  ERR_EINVAL_NORETURN (!this );
  
  if (op_ret >= 0) {
    strftime (atime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_atime));
    strftime (mtime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_mtime));
    strftime (ctime_buf, 256, "[%b %d %H:%M:%S]", localtime (&buf->st_ctime));
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d, *buf=%p {st_dev=%lld, st_ino=%lld, st_mode=%d, st_nlink=%d, st_uid=%d, st_gid=%d, st_rdev=%llx, st_size=%lld, st_blksize=%ld, st_blocks=%lld, st_atime=%s, st_mtime=%s, st_ctime=%s})",
	    this, op_ret, op_errno, buf, buf->st_dev, buf->st_ino, buf->st_mode, buf->st_nlink, buf->st_uid, buf->st_gid, buf->st_rdev, buf->st_size, buf->st_blksize, buf->st_blocks, atime_buf, mtime_buf, ctime_buf);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t 
trace_lk_cbk (call_frame_t *frame,
	      void *cookie,
	      xlator_t *this,
	      int32_t op_ret,
	      int32_t op_errno,
	      struct flock *lock)
{
  ERR_EINVAL_NORETURN (!this );

  if (op_ret >= 0) {
    gf_log (this->name,
	    GF_LOG_DEBUG,
	    "(*this=%p, op_ret=%d, op_errno=%d, *lock=%p {l_type=%d, l_whence=%d, l_start=%lld, l_len=%lld, l_pid=%ld})",
	    this, op_ret, op_errno, lock, 
	    lock->l_type, lock->l_whence, lock->l_start, lock->l_len, lock->l_pid);
  } else {
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "(*this=%p, op_ret=%d, op_errno=%d)",
	    this, op_ret, op_errno);
  }    

  STACK_UNWIND (frame, op_ret, op_errno, lock);
  return 0;
}


static int32_t 
trace_writedir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  ERR_EINVAL_NORETURN (!this );
  
  gf_log (this->name,
	  GF_LOG_DEBUG,
	  "*this=%p, op_ret=%d, op_errno=%d",
	  this, op_ret, op_errno);

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

int32_t 
trace_lookup (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t need_xattr)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, loc=%p {path=%s, inode=%p}, need_xattr=%d )",
	  (long long) frame->root->unique, this, loc, loc->path, loc->inode, need_xattr);
  
  STACK_WIND (frame, 
	      trace_lookup_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->lookup, 
	      loc,
	      need_xattr);
  
  return 0;
}

static int32_t 
trace_forget (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode)
{
  ERR_EINVAL_NORETURN (!this || !inode);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, inode=%p})",
	  (long long) frame->root->unique, this, inode);
  
  STACK_WIND (frame, 
	      trace_forget_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->forget, 
	      inode);
  
  return 0;
}

static int32_t 
trace_stat (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !loc );

#if 0
    dev_t     st_dev;     /* ID of device containing file */
    ino_t     st_ino;     /* inode number */
    mode_t    st_mode;    /* protection */
    nlink_t   st_nlink;   /* number of hard links */
    uid_t     st_uid;     /* user ID of owner */
    gid_t     st_gid;     /* group ID of owner */
    dev_t     st_rdev;    /* device ID (if special file) */
    off_t     st_size;    /* total size, in bytes */
    blksize_t st_blksize; /* blocksize for filesystem I/O */
    blkcnt_t  st_blocks;  /* number of blocks allocated */
    time_t    st_atime;   /* time of last access */
    time_t    st_mtime;   /* time of last modification */
    time_t    st_ctime;   /* time of last status change */
#endif 

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, loc=%p {path=%s, inode=%p})\n",
	  (long long) frame->root->unique, this, loc, loc->path, loc->inode);

  STACK_WIND (frame, 
	      trace_stat_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->stat, 
	      loc);
  
  return 0;
}

static int32_t 
trace_readlink (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		size_t size)
{
  ERR_EINVAL_NORETURN (!this || !loc || (size < 1));
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, size=%d)",
	  this, loc, loc->path, loc->inode, size);
  
  STACK_WIND (frame, 
	      trace_readlink_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->readlink, 
	      loc, 
	      size);
  
  return 0;
}

static int32_t 
trace_mknod (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode,
	     dev_t dev)
{
  ERR_EINVAL_NORETURN (!this || !loc->path);

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, mode=%d, dev=%lld)",
	  this, loc, loc->path, loc->inode, mode, dev);
  
  STACK_WIND (frame, 
	      trace_mknod_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->mknod, 
	      loc,
	      mode, 
	      dev);
  
  return 0;
}

static int32_t 
trace_mkdir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode)
{
  ERR_EINVAL_NORETURN (!this || !loc->path);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, path=%s, loc=%p {path=%s, inode=%p}, mode=%d)",
	  this, loc->path, loc, loc->inode, mode);
  
  STACK_WIND (frame, 
	      trace_mkdir_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->mkdir, 
	      loc,
	      mode);
  return 0;
}

static int32_t 
trace_unlink (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p{path=%s, inode=%p})",
	  this, loc, loc->path, loc->inode);
  
  STACK_WIND (frame, 
	      trace_unlink_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->unlink, 
	      loc);
  return 0;
}

static int32_t 
trace_rmdir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p})",
	  this, loc, loc->path, loc->inode);
  
  STACK_WIND (frame, 
	      trace_rmdir_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->rmdir, 
	      loc);
  
  return 0;
}

static int32_t 
trace_symlink (call_frame_t *frame,
	       xlator_t *this,
	       const char *linkpath,
	       loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !linkpath || !loc->path);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, linkpath=%s, loc=%p {path=%s, inode=%p})",
	  this, linkpath, loc, loc->path, loc->inode);
  
  STACK_WIND (frame, 
	      trace_symlink_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->symlink, 
	      linkpath,
	      loc);
  
  return 0;
}

static int32_t 
trace_rename (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *oldloc,
	      loc_t *newloc)
{  
  ERR_EINVAL_NORETURN (!this || !oldloc || !newloc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, oldloc=%p{path=%s, inode=%p, ino=%ld}, newloc=%p{path=%s, inode=%p, ino=%ld})",
	  this, oldloc, oldloc->path, oldloc->inode, oldloc->ino, newloc, newloc->path, newloc->inode, newloc->ino);
  
  STACK_WIND (frame, 
	      trace_rename_cbk,
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->rename, 
	      oldloc,
	      newloc);
  
  return 0;
}

static int32_t 
trace_link (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    const char *newpath)
{
  
  ERR_EINVAL_NORETURN (!this || !loc || !newpath);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, newpath=%s)",
	  this, loc, loc->path, loc->inode, newpath);

  STACK_WIND (frame, 
	      trace_link_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->link, 
	      loc,
	      newpath);
  return 0;
}

static int32_t 
trace_chmod (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, mode=%o)",
	  this, loc, loc->path, loc->inode, mode);

  STACK_WIND (frame, 
	      trace_chmod_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->chmod, 
	      loc,
	      mode);
  
  return 0;
}

static int32_t 
trace_chown (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     uid_t uid,
	     gid_t gid)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, uid=%d, gid=%d)",
	  this, loc, loc->path, loc->inode, uid, gid);
  
  STACK_WIND (frame, 
	      trace_chown_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->chown, 
	      loc,
	      uid,
	      gid);

  return 0;
}

static int32_t 
trace_truncate (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		off_t offset)
{
  ERR_EINVAL_NORETURN (!this || !loc);
 
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, offset=%lld)",
	  this, loc, loc->path, loc->inode, offset);

  STACK_WIND (frame, 
	      trace_truncate_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->truncate, 
	      loc,
	      offset);
  
  return 0;
}

static int32_t 
trace_utimens (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc,
	       struct timespec tv[2])
{
  char actime_str[256];
  char modtime_str[256];
  
  ERR_EINVAL_NORETURN (!this || !loc || !tv);
  
  strftime (actime_str, 256, "[%b %d %H:%M:%S]", localtime (&tv[0].tv_sec));
  strftime (modtime_str, 256, "[%b %d %H:%M:%S]", localtime (&tv[1].tv_sec));

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, *tv=%p {actime=%s, modtime=%s})",
	  this, loc, loc->path, loc->inode, tv, actime_str, modtime_str);

  STACK_WIND (frame, 
	      trace_utimens_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->utimens, 
	      loc,
	      tv);

  return 0;
}

static int32_t 
trace_open (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t flags,
	    fd_t *fd)
{

  ERR_EINVAL_NORETURN (!this || !loc);

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, flags=%d, fd=%p)",
	  this, loc, loc->path, loc->inode, flags, fd);
  
  STACK_WIND (frame, 
	      trace_open_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->open, 
	      loc,
	      flags,
	      fd);
  return 0;
}


static int32_t 
trace_create (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t flags,
	      mode_t mode,
	      fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !loc->path);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, flags=0%o mode=0%o)",
	  this, loc, loc->path, loc->inode, flags, mode);
  
  STACK_WIND (frame, 
	      trace_create_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->create, 
	      loc,
	      flags,
	      mode,
	      fd);
  return 0;
}

static int32_t 
trace_readv (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     size_t size,
	     off_t offset)
{
  ERR_EINVAL_NORETURN (!this || !fd || (size < 1));
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p, size=%d, offset=%lld)",
	  this, fd, size, offset);
  
  STACK_WIND (frame, 
	      trace_readv_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->readv,
	      fd,
	      size,
	      offset);
  return 0;
}

static int32_t 
trace_writev (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      struct iovec *vector,
	      int32_t count,
	      off_t offset)
{
  ERR_EINVAL_NORETURN (!this || !fd || !vector || (count < 1));

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p, *vector=%p, count=%d, offset=%lld)",
	  this, fd, vector, count, offset);

  STACK_WIND (frame, 
	      trace_writev_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->writev, 
	      fd,
	      vector,
	      count,
	      offset);
  return 0;
}

static int32_t 
trace_statfs (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p})",
	  this, loc, loc->path, loc->inode);

  STACK_WIND (frame, 
	      trace_statfs_cbk, 
	      FIRST_CHILD(this), FIRST_CHILD(this)->fops->statfs, 
	      loc);
  return 0; 
}

static int32_t 
trace_flush (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p)",
	  this, fd);

  STACK_WIND (frame, 
	      trace_flush_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->flush, 
	      fd);
  return 0;
}

static int32_t 
trace_close (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p)",
	  this, fd);
  
  STACK_WIND (frame, 
	      trace_close_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->close, 
	      fd);
  return 0;
}

static int32_t 
trace_fsync (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     int32_t flags)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, flags=%d, *fd=%p)",
	  this, flags, fd);

  STACK_WIND (frame, 
	      trace_fsync_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->fsync, 
	      fd,
	      flags);
  return 0;
}

static int32_t 
trace_setxattr (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		dict_t *dict,
		int32_t flags)
{
  ERR_EINVAL_NORETURN (!this || !loc || !dict);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, dict=%p, flags=%d)",
	  this, loc, loc->path, loc->inode, dict, flags);
  
  STACK_WIND (frame, 
	      trace_setxattr_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->setxattr, 
	      loc,
	      dict,
	      flags);
  return 0;
}

static int32_t 
trace_getxattr (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p})",
	  this, loc, loc->path, loc->inode);

  STACK_WIND (frame, 
	      trace_getxattr_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->getxattr,
	      loc);
  return 0;
}

static int32_t 
trace_removexattr (call_frame_t *frame,
		   xlator_t *this,
		   loc_t *loc,
		   const char *name)
{
  ERR_EINVAL_NORETURN (!this || !loc || !name);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, loc=%p {path=%s, inode=%p}, name=%s)",
	  this, loc, loc->path, loc->inode, name);

  STACK_WIND (frame, 
	      trace_removexattr_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->removexattr, 
	      loc,
	      name);

  return 0;
}

static int32_t 
trace_opendir (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc,
	       fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !loc );
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, loc=%p {path=%s, inode=%p}, fd=%p)",
	  (long long) frame->root->unique, this, loc, loc->path, loc->inode, fd);

  STACK_WIND (frame, 
	      trace_opendir_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->opendir, 
	      loc,
	      fd);
  return 0;
}

static int32_t 
trace_readdir (call_frame_t *frame,
	       xlator_t *this,
	       size_t size,
	       off_t offset,
	       fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !fd);  

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, size=%d, offset=%lld fd=%p)",
	  (long long) frame->root->unique, this, size, offset, fd);

  STACK_WIND (frame, 
	      trace_readdir_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->readdir, 
	      size, 
	      offset, 
	      fd);
  return 0;
}

static int32_t 
trace_closedir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd)
{  
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "callid: %lld (*this=%p, *fd=%p)",
	  (long long) frame->root->unique, this, fd);
  
  STACK_WIND (frame, 
	      trace_closedir_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->closedir, 
	      fd);
  return 0;
}

static int32_t 
trace_fsyncdir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int32_t datasync)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, datasync=%d, *fd=%p)",
	  this, datasync, fd);

  STACK_WIND (frame, 
	      trace_fsyncdir_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->fsyncdir, 
	      fd,
	      datasync);
  return 0;
}

static int32_t 
trace_access (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t mask)
{
  ERR_EINVAL_NORETURN (!this || !loc);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *loc=%p {path=%s, inode=%p}, mask=%d)",
	  this, loc, loc->path, loc->inode, mask);

  STACK_WIND (frame, 
	      trace_access_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->access, 
	      loc,
	      mask);
  return 0;
}

static int32_t 
trace_ftruncate (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 off_t offset)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, offset=%lld, *fd=%p)",
	  this, offset, fd);

  STACK_WIND (frame, 
	      trace_ftruncate_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->ftruncate, 
	      fd,
	      offset);

  return 0;
}

static int32_t 
trace_fchown (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      uid_t uid,
	      gid_t gid)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p, uid=%d, gid=%d)",
	  this, fd, uid, gid);

  STACK_WIND (frame, 
	      trace_fchown_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->fchown, 
	      fd,
	      uid,
	      gid);
  return 0;
}

static int32_t 
trace_fchmod (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      mode_t mode)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, mode=%o, *fd=%p)",
	  this, mode, fd);

  STACK_WIND (frame, 
	      trace_fchmod_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->fchmod, 
	      fd,
	      mode);
  return 0;
}

static int32_t 
trace_fstat (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p)",
	  this, fd);

  STACK_WIND (frame, 
	      trace_fstat_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->fstat, 
	      fd);
  return 0;
}

static int32_t 
trace_lk (call_frame_t *frame,
	  xlator_t *this,
	  fd_t *fd,
	  int32_t cmd,
	  struct flock *lock)
{
  ERR_EINVAL_NORETURN (!this || !fd);
  
  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p, cmd=%d, lock=%p {l_type=%d, l_whence=%d, l_start=%lld, l_len=%lld, l_pid=%ld})",
	  this, fd, cmd, lock,
	  lock->l_type, lock->l_whence, lock->l_start, lock->l_len, lock->l_pid);

  STACK_WIND (frame, 
	      trace_lk_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->lk, 
	      fd,
	      cmd,
	      lock);
  return 0;
}

int32_t 
trace_writedir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int32_t flags,
		dir_entry_t *entries,
		int32_t count)
{

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "(*this=%p, *fd=%p, flags=%d, entries=%p count=%d",
	  this, fd, flags, entries, count);

  STACK_WIND (frame, 
	      trace_writedir_cbk, 
	      FIRST_CHILD(this), 
	      FIRST_CHILD(this)->fops->writedir, 
	      fd,
	      flags,
	      entries,
	      count);
  return 0;
}
     
int32_t 
init (xlator_t *this)
{
  if (!this)
    return -1;

  if (!this->children) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "trace translator requires one subvolume");
    return -1;
  }
    
  if (this->children->next) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "trace translator does not support more than one sub-volume");
    return -1;
  }

  gf_log_set_loglevel (GF_LOG_DEBUG);
  
  void gf_log_xlator (xlator_t *this) {
    int32_t len;
    char *buf;
    
    if (!this)
      return;
    
    len = dict_serialized_length (this->options);
    buf = alloca (len);
    dict_serialize (this->options, buf);
    
    gf_log (this->name, 
	    GF_LOG_DEBUG, 
	    "init (xlator_t *this=%p {name=%s, *next=%p, *parent=%p, *children=%p {xlator=%p, next=%p}, *fops=%p {*open=%p, stat=%p, *readlink=%p, *mknod=%p, *mkdir=%p, *unlink=%p, *rmdir=%p, *symlink=%p, *rename=%p, *link=%p, *chmod=%p, *chown=%p, *truncate=%p, *utimens=%p, *read=%p, *write=%p, *statfs=%p, *flush=%p, *close=%p, *fsync=%p, *setxattr=%p, *getxattr=%p, *removexattr=%p, *opendir=%p, *readdir=%p, *closedir=%p, *fsyncdir=%p, *access=%p, *ftruncate=%p, *fstat=%p}, *mops=%p {*stats=%p, *fsck=%p, *lock=%p, *unlock=%p}, *fini()=%p, *init()=%p, *options=%p {%s}, *private=%p)", 
	    this, this->name, this->next, this->parent, this->children, this->children->xlator, this->children->next, this->fops, this->fops->open, this->fops->stat, this->fops->readlink, this->fops->mknod, this->fops->mkdir, this->fops->unlink, this->fops->rmdir, this->fops->symlink, this->fops->rename, this->fops->link, this->fops->chmod, this->fops->chown, this->fops->truncate, this->fops->utimens, this->fops->readv, this->fops->writev, this->fops->statfs, this->fops->flush, this->fops->close, this->fops->fsync, this->fops->setxattr, this->fops->getxattr, this->fops->removexattr, this->fops->opendir, this->fops->readdir, this->fops->closedir, this->fops->fsyncdir, this->fops->access, this->fops->ftruncate, this->fops->fstat, this->mops, this->mops->stats,  this->mops->fsck, this->mops->lock, this->mops->unlock, this->fini, this->init, this->options, buf, this->private);
  }
  
  //xlator_foreach (this, gf_log_xlator);
  
  /* Set this translator's inode table pointer to child node's pointer. */
  this->itable = FIRST_CHILD (this)->itable;

  return 0;
}

void
fini (xlator_t *this)
{
  if (!this)
    return;

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "fini (xlator_t *this=%p)", this);

  /* Free up the dictionary options */
  dict_destroy (FIRST_CHILD(this)->options);

  gf_log (this->name, 
	  GF_LOG_DEBUG, 
	  "trace translator unloaded");
  return;
}

struct xlator_fops fops = {
  .stat        = trace_stat,
  .readlink    = trace_readlink,
  .mknod       = trace_mknod,
  .mkdir       = trace_mkdir,
  .unlink      = trace_unlink,
  .rmdir       = trace_rmdir,
  .symlink     = trace_symlink,
  .rename      = trace_rename,
  .link        = trace_link,
  .chmod       = trace_chmod,
  .chown       = trace_chown,
  .truncate    = trace_truncate,
  .utimens     = trace_utimens,
  .open        = trace_open,
  .readv       = trace_readv,
  .writev      = trace_writev,
  .statfs      = trace_statfs,
  .flush       = trace_flush,
  .close       = trace_close,
  .fsync       = trace_fsync,
  .setxattr    = trace_setxattr,
  .getxattr    = trace_getxattr,
  .removexattr = trace_removexattr,
  .opendir     = trace_opendir,
  .readdir     = trace_readdir,
  .closedir    = trace_closedir,
  .fsyncdir    = trace_fsyncdir,
  .access      = trace_access,
  .ftruncate   = trace_ftruncate,
  .fstat       = trace_fstat,
  .create      = trace_create,
  .fchown      = trace_fchown,
  .fchmod      = trace_fchmod,
  .lk          = trace_lk,
  .lookup      = trace_lookup,
  .forget      = trace_forget,
  .writedir    = trace_writedir
};

static int32_t 
trace_stats_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct xlator_stats *stats)
{
  STACK_UNWIND (frame, op_ret, op_errno, stats);
  return 0;
}

static int32_t 
trace_stats (call_frame_t *frame,
	     xlator_t *this, 
	     int32_t flags)
{
  ERR_EINVAL_NORETURN (!this);
  
  {
    gf_log (this->name, GF_LOG_DEBUG, "trace_stats (*this=%p, flags=%d\n", this, flags);

    STACK_WIND (frame, 
		trace_stats_cbk, 
		FIRST_CHILD(this), 
		FIRST_CHILD(this)->mops->stats, 
		flags);
  }
  return 0;
}

struct xlator_mops mops = {
  .stats = trace_stats
};
