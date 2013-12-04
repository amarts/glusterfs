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
#include "dict.h"
#include "logging.h"
#include "posix.h"
#include "xlator.h"
#include "lock.h"
#include "defaults.h"
#include "common-utils.h"
#include <stdint.h>

#include <sys/time.h>
#include <errno.h>

#ifdef HAVE_SET_FSID

#define DECLARE_OLD_FS_UID_GID_VAR int32_t old_fsuid, old_fsgid

#define SET_FS_UID_GID(uid, gid) do {   \
 old_fsuid = setfsuid (uid);                                  \
 old_fsgid = setfsgid (gid);                                  \
} while (0)

#define SET_TO_OLD_FS_UID_GID() do {      \
  setfsuid (old_fsuid);                                       \
  setfsgid (old_fsgid);                                       \
} while (0);

#else

#define DECLARE_OLD_FS_UID_GID_VAR
#define SET_FS_UID_GID(uid, gid)
#define SET_TO_OLD_FS_UID_GID()

#endif

#define MAKE_REAL_PATH(var, this, path) do {                             \
  int base_len = ((struct posix_private *)this->private)->base_path_length; \
  var = alloca (strlen (path) + base_len + 2);                           \
  strcpy (var, ((struct posix_private *)this->private)->base_path);      \
  strcpy (&var[base_len], path);                                         \
} while (0)


static int32_t
posix_lookup (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  struct stat buf = {0, };
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = lstat (real_path, &buf);
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &buf);

  return 0;
}


static int32_t
posix_forget (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode)
{
  if (dict_get (inode->ctx, this->name)) {
    int32_t _fd = data_to_int32 (dict_get (inode->ctx, this->name));
    close (_fd);
  }
  return 0;
}

static int32_t
posix_stat (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc)
{
  struct stat buf;
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = lstat (real_path, &buf);
  op_errno = errno;

  SET_TO_OLD_FS_UID_GID();  

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}



static int32_t 
posix_opendir (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc, fd_t *fd)
{
  char *real_path;
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;

  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  _fd = open (real_path, O_DIRECTORY|O_RDONLY);
  op_errno = errno;
  op_ret = _fd;
  
  SET_TO_OLD_FS_UID_GID ();
  
  if (_fd != -1) {
    close (_fd);
    dict_set (fd->ctx, this->name, data_from_dynstr (strdup (real_path)));
  }

  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}


static int32_t
posix_readdir (call_frame_t *frame,
	       xlator_t *this,
	       size_t size,
	       off_t off,
	       fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  dir_entry_t entries = {0, };
  dir_entry_t *tmp;
  DIR *dir;
  struct dirent *dirent;
  int real_path_len;
  int entry_path_len;
  char *entry_path;
  int count = 0;
  data_t *path_data = NULL;
  DECLARE_OLD_FS_UID_GID_VAR ;

  if (fd && fd->ctx) {
    path_data = dict_get (fd->ctx, this->name);
    if (!path_data) {
      STACK_UNWIND (frame, -1, EBADFD, &entries, 0);
      return 0;
    }
  } else {
    STACK_UNWIND (frame, -1, EBADFD, &entries, 0);
    return 0;
  }
  real_path = data_to_str (path_data);
  real_path_len = strlen (real_path);
  entry_path_len = real_path_len + 1024;
  entry_path = calloc (1, entry_path_len);
  strcpy (entry_path, real_path);
  entry_path[real_path_len] = '/';

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  dir = opendir (real_path);
  
  if (!dir){
    gf_log (this->name, GF_LOG_DEBUG, 
	    "failed to do opendir for `%s'", real_path);

    SET_TO_OLD_FS_UID_GID ();

    STACK_UNWIND (frame, -1, errno, &entries, 0);
    freee (entry_path);
    return 0;
  } else {
    op_ret = 0;
    op_errno = 0;
  }
  
  while ((dirent = readdir (dir))) {
    if (!dirent)
      break;
    tmp = calloc (1, sizeof (*tmp));
    tmp->name = strdup (dirent->d_name);
    if (entry_path_len < real_path_len + 1 + strlen (tmp->name) + 1) {
      entry_path_len = real_path_len + strlen (tmp->name) + 1024;
      entry_path = realloc (entry_path, entry_path_len);
    }
    strcpy (&entry_path[real_path_len+1], tmp->name);
    lstat (entry_path, &tmp->buf);
    count++;
    
    tmp->next = entries.next;
    entries.next = tmp;
  }
  freee (entry_path);
  closedir (dir);

  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno, &entries, count);
  while (entries.next) {
    tmp = entries.next;
    entries.next = entries.next->next;
    freee (tmp->name);
    freee (tmp);
  }
  return 0;
}


static int32_t 
posix_closedir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;

  op_ret = 0;
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_readlink (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		size_t size)
{
  char *dest = alloca (size + 1);
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = readlink (real_path, dest, size);
  if (op_ret > 0) 
    dest[op_ret] = 0;
  op_errno = errno;
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, dest);

  return 0;
}

static int32_t 
posix_mknod (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode,
	     dev_t dev)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = { 0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = mknod (real_path, mode, dev);
  op_errno = errno;
  
  if (op_ret == 0) {
#ifndef HAVE_SET_FSID
    lchown (real_path, frame->root->uid, frame->root->gid);
#endif
    lstat (real_path, &stbuf);
  }
  
  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);

  return 0;
}

static int32_t 
posix_mkdir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = {0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = mkdir (real_path, mode);
  op_errno = errno;
  
  if (op_ret == 0) {
#ifndef HAVE_SET_FSID
    chown (real_path, frame->root->uid, frame->root->gid);
#endif
    lstat (real_path, &stbuf);
  }
  
  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);

  return 0;
}


static int32_t 
posix_unlink (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);
  /*
  _fd = open (real_path, O_RDWR);
  */
  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = unlink (real_path);
  op_errno = errno;
  
  SET_TO_OLD_FS_UID_GID ();

  /*
  if (op_ret == -1) {
    close (_fd);
  } else {
    dict_set (loc->inode->ctx, this->name, data_from_int32 (_fd));
  }
  */

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_rmdir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);
  /*
  _fd = open (real_path, O_DIRECTORY | O_RDONLY);
  */
  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = rmdir (real_path);
  op_errno = errno;
    
  SET_TO_OLD_FS_UID_GID ();

  /*
  if (op_ret == -1) {
    close (_fd);
  } else {
    dict_set (loc->inode->ctx, this->name, data_from_int32 (_fd));
  }
  */
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

static int32_t 
posix_symlink (call_frame_t *frame,
	       xlator_t *this,
	       const char *linkname,
	       loc_t *loc)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = { 0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = symlink (linkname, real_path);
  op_errno = errno;
  
  if (op_ret == 0) {
#ifndef HAVE_SET_FS_ID
    lchown (real_path, frame->root->uid, frame->root->gid);
#endif
    lstat (real_path, &stbuf);
  }
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);

  return 0;
}

static int32_t 
posix_rename (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *oldloc,
	      loc_t *newloc)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_oldpath;
  char *real_newpath;
  struct stat stbuf = {0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_oldpath, this, oldloc->path);
  MAKE_REAL_PATH (real_newpath, this, newloc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = rename (real_oldpath, real_newpath);
  op_errno = errno;
    
  if (op_ret == 0) {
    lstat (real_newpath, &stbuf);
  }

  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}

static int32_t 
posix_link (call_frame_t *frame, 
	    xlator_t *this,
	    loc_t *oldloc,
	    const char *newpath)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_oldpath;
  char *real_newpath;
  struct stat stbuf = {0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_oldpath, this, oldloc->path);
  MAKE_REAL_PATH (real_newpath, this, newpath);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = link (real_oldpath, real_newpath);
  op_errno = errno;
    
  if (op_ret == 0) {
#ifndef HAVE_SET_FSID
    lchown (real_newpath, frame->root->uid, frame->root->gid);
#endif
    lstat (real_newpath, &stbuf);
  }
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, oldloc->inode, &stbuf);

  return 0;
}


static int32_t 
posix_chmod (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;
  DECLARE_OLD_FS_UID_GID_VAR;
  
  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  op_ret = chmod (real_path, mode);
  op_errno = errno;
    
  if (op_ret == 0)
    lstat (real_path, &stbuf);
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_chown (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     uid_t uid,
	     gid_t gid)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = lchown (real_path, uid, gid);
  op_errno = errno;
    
  if (op_ret == 0)
    lstat (real_path, &stbuf);
    
  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_truncate (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = truncate (real_path, offset);
  op_errno = errno;
    
  if (op_ret == 0) {
    lstat (real_path, &stbuf);
  }
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_utimens (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc,
	       struct timespec ts[2])
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct stat stbuf = {0, };
  struct timeval tv[2];
  DECLARE_OLD_FS_UID_GID_VAR;
  
  MAKE_REAL_PATH (real_path, this, loc->path);

  /* TODO: fix timespec to timeval converstion 
   * Done: Check if this is correct */

  tv[0].tv_sec = ts[0].tv_sec;
  tv[0].tv_usec = ts[0].tv_nsec * 1000;
  tv[1].tv_sec = ts[1].tv_sec;
  tv[1].tv_usec = ts[1].tv_nsec * 1000;

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = utimes (real_path, tv);
  op_errno = errno;
    
  lstat (real_path, &stbuf);
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}

static int32_t 
posix_create (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t flags,
	      mode_t mode,
	      fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  int32_t _fd;
  char *real_path;
  struct stat stbuf = {0, };
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  if (!flags) {
    _fd = open (real_path, 
		O_CREAT|O_RDWR|O_EXCL,
		mode);
  } else {
    _fd = open (real_path, 
		flags|O_CREAT,
		mode);
  }

  op_errno = errno;
    
  if (_fd >= 0) {
      /* trigger readahead in the kernel */
#if 0
    char buf[1024 * 64];
    read (_fd, buf, 1024 * 64);
    lseek (_fd, 0, SEEK_SET);
#endif
#ifndef HAVE_SET_FSID
    chown (real_path, frame->root->uid, frame->root->gid);
#endif
    lstat (real_path, &stbuf);
    
  }
  SET_TO_OLD_FS_UID_GID ();

  if (_fd >= 0) {
    dict_set (fd->ctx, this->name, data_from_int32 (_fd));
    ((struct posix_private *)this->private)->stats.nr_files++;
    op_ret = 0;
  }

  STACK_UNWIND (frame, op_ret, op_errno, fd, loc->inode, &stbuf);

  return 0;
}

static int32_t 
posix_open (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t flags,
	    fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  char *real_path;
  int32_t _fd;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  _fd = open (real_path, flags, 0);
  op_errno = errno;
    
  SET_TO_OLD_FS_UID_GID ();

  if (_fd >= 0) {
    dict_set (fd->ctx, this->name, data_from_int32 (_fd));

    ((struct posix_private *)this->private)->stats.nr_files++;
    op_ret = 0;
#ifndef HAVE_SET_FSID
    if (flags & O_CREAT)
      chown (real_path, frame->root->uid, frame->root->gid);
#endif
  }

  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}

static int32_t 
posix_readv (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     size_t size,
	     off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  char *buf = NULL;
  int32_t _fd;
  struct posix_private *priv = this->private;
  dict_t *reply_dict = NULL;
  struct iovec vec;
  data_t *fd_data;
  struct stat stbuf = {0,};

  fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF, &vec, 0, &stbuf);
    return 0;
  }

  buf = calloc (1, size);
  if (size)
    buf[0] = '\0';

  _fd = data_to_int32 (fd_data);

  priv->read_value += size;
  priv->interval_read += size;

  if (lseek (_fd, offset, SEEK_SET) == -1) {
    STACK_UNWIND (frame, -1, errno, &vec, 0, &stbuf);
    return 0;
  }
  
  op_ret = read (_fd, buf, size);
  op_errno = errno;
  vec.iov_base = buf;
  vec.iov_len = op_ret;
    
  if (op_ret >= 0) {
    data_t *buf_data = get_new_data ();
    reply_dict = get_new_dict ();

    buf_data->data = buf;
    buf_data->len = op_ret;

    dict_set (reply_dict, NULL, buf_data);
    frame->root->rsp_refs = dict_ref (reply_dict);
    /* readv successful, we also need to get the stat of the file we read from */
    fstat (_fd, &stbuf);
  }
  
  STACK_UNWIND (frame, op_ret, op_errno, &vec, 1, &stbuf);

  if (reply_dict)
    dict_unref (reply_dict);
  return 0;
}


static int32_t 
posix_writev (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      struct iovec *vector,
	      int32_t count,
	      off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct posix_private *priv = this->private;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  struct stat stbuf = {0,};

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF, &stbuf);
    return 0;
  }
  _fd = data_to_int32 (fd_data);


  if (lseek (_fd, offset, SEEK_SET) == -1) {
    STACK_UNWIND (frame, -1, errno, &stbuf);
    return 0;
  }

  op_ret = writev (_fd, vector, count);
  op_errno = errno;

  priv->write_value += op_ret;
  priv->interval_write += op_ret;
  
  if (op_ret >= 0) {
    /* wiretv successful, we also need to get the stat of the file we read from */
    fstat (_fd, &stbuf);
  }

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}


static int32_t 
posix_statfs (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)

{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  struct statvfs buf = {0, };

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = statvfs (real_path, &buf);
  op_errno = errno;

  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}


static int32_t 
posix_flush (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  int32_t _fd;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);
  /* do nothing */

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

static int32_t 
posix_close (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct posix_private *priv = this->private;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  priv->stats.nr_files--;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  op_ret = close (_fd);
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}


static int32_t 
posix_fsync (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     int32_t datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  DECLARE_OLD_FS_UID_GID_VAR;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  if (datasync) {
    ;
#ifdef HAVE_FDATASYNC
    op_ret = fdatasync (_fd);
#endif
  }
  else
    op_ret = fsync (_fd);
  op_errno = errno;

  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno);
  
  return 0;
}


static int32_t 
posix_setxattr (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		dict_t *dict,
		int flags)
{
  int32_t op_ret = -1;
  int32_t op_errno = 0;
  char *real_path;
  data_pair_t *trav = dict->members_list;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  while (trav) {
    /* FIXME why is it len - 1 ? data should not be assumed to be NULL terminated string
     FIXED: removed "- 1" since fuse_setxattr now correctly uses bin_to_data (previously was using data_from_dynstr)*/
    op_ret = lsetxattr (real_path, 
			trav->key, 
			trav->value->data, 
			trav->value->len, 
			flags);
    op_errno = errno;
    trav = trav->next;
  }
    
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}

/**
 * posix_getxattr - this function returns a dictionary with all the 
 *       key:value pair present as xattr. used for both 'listxattr' and
 *       'getxattr'.
 */
static int32_t 
posix_getxattr (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc)
{
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  int32_t list_offset = 0;
  size_t size = 0;
  size_t remaining_size = 0;
  char key[1024] = {0,};
  char *value = NULL;
  char *list = NULL;
  char *real_path = NULL;
  dict_t *dict = NULL;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  /* Get the total size */
  dict = get_new_dict ();

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
  
  size = llistxattr (real_path, NULL, 0);
  op_errno = errno;
  if (size <= 0) {
    if (size == 0) {
      size = lgetxattr (real_path, "trusted.afr.key", NULL, 0);
      op_errno = errno;
    }
    SET_TO_OLD_FS_UID_GID ();
    /* There are no extended attributes, send an empty dictionary */
    
    if (dict) {
      dict_ref (dict);
    }
    
    STACK_UNWIND (frame, size, op_errno, dict);
    
    if (dict)
      dict_unref (dict);
    
    return 0;
  }

  list = alloca (size + 1);
  size = llistxattr (real_path, list, size);
  
  remaining_size = size;
  list_offset = 0;
  while (remaining_size > 0) {
    if(*(list+list_offset) == '\0')
      break;
    strcpy (key, list + list_offset);
    op_ret = lgetxattr (real_path, key, NULL, 0);
    if (op_ret == -1)
      break;
    value = calloc (op_ret + 1, sizeof(char));
    op_ret = lgetxattr (real_path, key, value, op_ret);
    if (op_ret == -1)
      break;
    value [op_ret] = '\0';
    dict_set (dict, key, data_from_dynptr (value, op_ret));
    remaining_size -= strlen (key) + 1;
    list_offset += strlen (key) + 1;
  }
  
  SET_TO_OLD_FS_UID_GID ();
  
  if (dict) {
    dict_ref (dict);
  }
  STACK_UNWIND (frame, size, op_errno, dict);
  if (dict)
    dict_unref (dict);
  return 0;
}
		     
static int32_t 
posix_removexattr (call_frame_t *frame,
		   xlator_t *this,
		   loc_t *loc,
		   const char *name)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = lremovexattr (real_path, name);
  op_errno = errno;

  SET_TO_OLD_FS_UID_GID ();    
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}


static int32_t 
posix_fsyncdir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  data_t *fd_data = dict_get (fd->ctx, this->name);

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  op_ret = 0;
  op_errno = errno;
  
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}


static int32_t 
posix_access (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t mask)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;
  DECLARE_OLD_FS_UID_GID_VAR;

  MAKE_REAL_PATH (real_path, this, loc->path);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = access (real_path, mask);
  op_errno = errno;

  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}


static int32_t 
posix_ftruncate (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 off_t offset)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd = 0;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  DECLARE_OLD_FS_UID_GID_VAR;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = ftruncate (_fd, offset);
  op_errno = errno;
    
  fstat (_fd, &buf);
  
  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

static int32_t 
posix_fchown (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      uid_t uid,
	      gid_t gid)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  DECLARE_OLD_FS_UID_GID_VAR;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = fchown (_fd, uid, gid);
  op_errno = errno;

  fstat (_fd, &buf);

  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}


static int32_t 
posix_fchmod (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      mode_t mode)
{
  int32_t op_ret;
  int32_t op_errno;
  int32_t _fd;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  DECLARE_OLD_FS_UID_GID_VAR;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }

  _fd = data_to_int32 (fd_data);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = fchmod (_fd, mode);
  op_errno = errno;
  
  fstat (_fd, &buf);

  SET_TO_OLD_FS_UID_GID ();
  
  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

static int32_t 
posix_writedir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int32_t flags,
		dir_entry_t *entries,
		int32_t count)
{
  char *real_path;
  char *entry_path;
  int32_t real_path_len;
  int32_t entry_path_len;
  int32_t ret = 0;

  if (!dict_get (fd->ctx, this->name)) {
    STACK_UNWIND (frame, -1, EBADFD);
    return 0;
  }

  /* fd exists, and everything looks fine */
  real_path = data_to_str (dict_get (fd->ctx, this->name));
  real_path_len = strlen (real_path);
  entry_path_len = real_path_len + 256;
  entry_path = calloc (1, entry_path_len);
  strcpy (entry_path, real_path);
  entry_path[real_path_len] = '/';

  {
    /**
     * create an entry for each one present in '@entries' 
     *  - if flag is set (ie, if its namespace), create both directories and 
     *    files 
     *  - if not set, create only directories.
     *
     *  after the entry is created, change the mode and ownership of the entry
     *  according to the stat present in entries->buf.  
     */
    dir_entry_t *trav = entries->next;
    while (trav) {
      char pathname[4096] = {0,};
      strcpy (pathname, entry_path);
      strcat (pathname, trav->name);

      if (S_ISDIR(trav->buf.st_mode)) {
	/* If the entry is directory, create it by calling 'mkdir'. If 
	 * directory is not present, it will be created, if its present, 
	 * no worries even if it fails.
	 */
	ret = mkdir (pathname, trav->buf.st_mode);
	if (!ret) {
	  gf_log (this->name, 
		  GF_LOG_DEBUG, 
		  "Creating directory %s with mode (0%o)", 
		  pathname,
		  trav->buf.st_mode);
	}
      } else if ((flags & GF_CREATE_MISSING_FILE) == GF_CREATE_MISSING_FILE) {
	/* Create a 0byte file here */
	if (S_ISREG (trav->buf.st_mode)) {
	  ret = open (pathname, O_CREAT|O_EXCL, trav->buf.st_mode);
	  if (ret > 0) {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "Creating file %s with mode (0%o)",
		    pathname, 
		    trav->buf.st_mode);
	    close (ret);
	  }
	} else if (S_ISLNK(trav->buf.st_mode)) {
	  ret = symlink (trav->name, pathname);
	  if (!ret) {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "Creating symlink %s",
		    pathname);
	  }
	} else if (S_ISBLK (trav->buf.st_mode) || 
		   S_ISCHR (trav->buf.st_mode) || 
		   S_ISFIFO (trav->buf.st_mode)) {
	  ret = mknod (pathname, trav->buf.st_mode, trav->buf.st_dev);
	  if (!ret) {
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "Creating device file %s",
		    pathname);
	  }
	}
      }
      /* Change the mode */
      chmod (pathname, trav->buf.st_mode);
      /* change the ownership */
      chown (pathname, trav->buf.st_uid, trav->buf.st_gid);

      /* consider the next entry */
      trav = trav->next;
    }
  }
  //  op_errno = errno;
  
  /* Return success all the time */
  STACK_UNWIND (frame, 0, 0);
  free (entry_path);
  
  freee (entry_path);
  return 0;
}

static int32_t 
posix_fstat (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd)
{
  int32_t _fd;
  int32_t op_ret;
  int32_t op_errno;
  struct stat buf;
  data_t *fd_data = dict_get (fd->ctx, this->name);
  DECLARE_OLD_FS_UID_GID_VAR;

  if (fd_data == NULL) {
    STACK_UNWIND (frame, -1, EBADF);
    return 0;
  }
  _fd = data_to_int32 (fd_data);

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);

  op_ret = fstat (_fd, &buf);
  op_errno = errno;

  SET_TO_OLD_FS_UID_GID ();

  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}


static int32_t 
posix_lk (call_frame_t *frame,
	  xlator_t *this,
	  fd_t *fd,
	  int32_t cmd,
	  struct flock *lock)
{
  struct flock nullock = {0, };
  STACK_UNWIND (frame, -1, ENOSYS, &nullock);
  return 0;
}

static int32_t 
posix_stats (call_frame_t *frame,
	     xlator_t *this,
	     int32_t flags)

{
  int32_t op_ret = 0;
  int32_t op_errno = 0;

  struct xlator_stats xlstats = {0, }, *stats = &xlstats;
  struct statvfs buf;
  struct timeval tv;
  struct posix_private *priv = (struct posix_private *)this->private;
  int64_t avg_read = 0;
  int64_t avg_write = 0;
  int64_t _time_ms = 0; 
  DECLARE_OLD_FS_UID_GID_VAR ;

  SET_FS_UID_GID (frame->root->uid, frame->root->gid);
    
  op_ret = statvfs (priv->base_path, &buf);
  op_errno = errno;
    
  SET_TO_OLD_FS_UID_GID ();
  
  stats->nr_files = priv->stats.nr_files;
  stats->nr_clients = priv->stats.nr_clients; /* client info is maintained at FSd */
  stats->free_disk = buf.f_bfree * buf.f_bsize; /* Number of Free block in the filesystem. */
  stats->total_disk_size = buf.f_blocks * buf.f_bsize; /* */
  stats->disk_usage = (buf.f_blocks - buf.f_bavail) * buf.f_bsize;

  /* Calculate read and write usage */
  gettimeofday (&tv, NULL);
  
  /* Read */
  _time_ms = (tv.tv_sec - priv->init_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->init_time.tv_usec) / 1000);

  avg_read = (_time_ms) ? (priv->read_value / _time_ms) : 0; /* KBps */
  avg_write = (_time_ms) ? (priv->write_value / _time_ms) : 0; /* KBps */
  
  _time_ms = (tv.tv_sec - priv->prev_fetch_time.tv_sec) * 1000 +
             ((tv.tv_usec - priv->prev_fetch_time.tv_usec) / 1000);
  if (_time_ms && ((priv->interval_read / _time_ms) > priv->max_read)) {
    priv->max_read = (priv->interval_read / _time_ms);
  }
  if (_time_ms && ((priv->interval_write / _time_ms) > priv->max_write)) {
    priv->max_write = priv->interval_write / _time_ms;
  }

  stats->read_usage = avg_read / priv->max_read;
  stats->write_usage = avg_write / priv->max_write;

  gettimeofday (&(priv->prev_fetch_time), NULL);
  priv->interval_read = 0;
  priv->interval_write = 0;

  STACK_UNWIND (frame, op_ret, op_errno, stats);
  return 0;
}

/**
 * notify - when parent sends PARENT_UP, send CHILD_UP event from here
 */
int32_t
notify (xlator_t *this,
        int32_t event,
        void *data,
        ...)
{
  switch (event)
    {
    case GF_EVENT_PARENT_UP:
      {
	/* Tell the parent that posix xlator is up */
	default_notify (this, GF_EVENT_CHILD_UP, data);
      }
      break;
    default:
      /* */
      break;
    }
  return 0;
}

/**
 * init - 
 */
int32_t 
init (xlator_t *this)
{
  struct posix_private *_private = calloc (1, sizeof (*_private));

  data_t *directory = dict_get (this->options, "directory");

  if (this->children) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "FATAL: storage/posix cannot have subvolumes");
    return -1;
  }

  if (!directory) {
    gf_log (this->name, GF_LOG_ERROR,
	    "export directory not specified in spec file");
    exit (1);
  }
  umask (000); // umask `masking' is done at the client side
  if (mkdir (directory->data, 0777) == 0) {
    gf_log (this->name, GF_LOG_WARNING,
	    "directory specified not exists, created");
  }

  _private->base_path = strdup (directory->data);
  _private->base_path_length = strlen (_private->base_path);

  {
    /* Stats related variables */
    gettimeofday (&_private->init_time, NULL);
    gettimeofday (&_private->prev_fetch_time, NULL);
    _private->max_read = 1;
    _private->max_write = 1;
  }

  this->private = (void *)_private;
  return 0;
}

void
fini (xlator_t *this)
{
  struct posix_private *priv = this->private;
  freee (priv);
  return;
}

struct xlator_mops mops = {
  .stats = posix_stats,
  .lock  = mop_lock_impl,
  .unlock = mop_unlock_impl
};

struct xlator_fops fops = {
  .lookup      = posix_lookup,
  .forget      = posix_forget,
  .stat        = posix_stat,
  .opendir     = posix_opendir,
  .readdir     = posix_readdir,
  .closedir    = posix_closedir,
  .readlink    = posix_readlink,
  .mknod       = posix_mknod,
  .mkdir       = posix_mkdir,
  .unlink      = posix_unlink,
  .rmdir       = posix_rmdir,
  .symlink     = posix_symlink,
  .rename      = posix_rename,
  .link        = posix_link,
  .chmod       = posix_chmod,
  .chown       = posix_chown,
  .truncate    = posix_truncate,
  .utimens     = posix_utimens,
  .create      = posix_create,
  .open        = posix_open,
  .readv       = posix_readv,
  .writev      = posix_writev,
  .statfs      = posix_statfs,
  .flush       = posix_flush,
  .close       = posix_close,
  .fsync       = posix_fsync,
  .setxattr    = posix_setxattr,
  .getxattr    = posix_getxattr,
  .removexattr = posix_removexattr,
  .fsyncdir    = posix_fsyncdir,
  .access      = posix_access,
  .ftruncate   = posix_ftruncate,
  .fstat       = posix_fstat,
  .lk          = posix_lk,
  .fchown      = posix_fchown,
  .fchmod      = posix_fchmod,
  .writedir    = posix_writedir,
};
