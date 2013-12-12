/*
   Copyright (c) 2008 Z RESEARCH, Inc. <http://www.zresearch.com>
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

/* bdb based storage translator - named as 'bdb' translator
 * 
 * 
 * There can be only two modes for files existing on bdb translator:
 * 1. DIRECTORY - directories are stored by bdb as regular directories on background file-system. 
 *                directories also have an entry in the ns_db.db of their parent directory.
 * 2. REGULAR FILE - regular files are stored as records in the storage_db.db present in the directory.
 *                   regular files also have an entry in ns_db.db
 *
 * Internally bdb has a maximum of three different types of logical files associated with each directory:
 * 1. storage_db.db - storage database, used to store the data corresponding to regular files in the
 *                   form of key/value pair. file-name is the 'key' and data is 'value'.
 * 2. directory (all subdirectories) - any subdirectory will have a regular directory entry.
 */
#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#define __XOPEN_SOURCE 500

#include <stdint.h>
#include <sys/time.h>
#include <errno.h>
#include <ftw.h>
#include <libgen.h>

#include "glusterfs.h"
#include "dict.h"
#include "logging.h"
#include "bdb.h"
#include "xlator.h"
#include "lock.h"
#include "defaults.h"
#include "common-utils.h"


static int32_t 
bdb_mknod (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode,
	   dev_t dev)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *db_path = NULL;
  char *pathname = NULL, *dir_name = NULL;
  struct bdb_ctx *bctx = NULL;
  struct stat stbuf = {0,};

  if (S_ISREG(mode)) {
    pathname = strdup (loc->path);
    dir_name = dirname (pathname);
    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, loc->path);
    if (((bctx = bdb_get_bctx_from (this, loc->path)) != NULL)) {
      char *key_string = NULL;
      
      lstat (db_path, &stbuf);
      MAKE_KEY_FROM_PATH (key_string, loc->path);
      op_ret = bdb_storage_put (this, bctx, key_string, NULL, 0, 0);
      if (!op_ret) {
	/* create successful */
	if (!bctx->directory)
	  bctx->directory = strdup (dir_name);

	lstat (db_path, &stbuf);
	stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	stbuf.st_mode  = mode;
      } /* if (!op_ret)...else */
    } else {
      op_ret = -1;
      op_errno = ENOENT;
    }/* if(bctx_data...)...else */
  } else {
    op_ret = -1;
    op_errno = EPERM;
  } /* if (S_ISREG(mode))...else */

  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno, NULL, NULL);
  return 0;
}

static int32_t 
bdb_rename (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *oldloc,
	    loc_t *newloc)
{
  /* TODO: hold a global lock, do bdb->del() followed by bdb_ns_put () */
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, NULL);
  return 0;
}

static int32_t 
bdb_link (call_frame_t *frame, 
	  xlator_t *this,
	  loc_t *oldloc,
	  const char *newpath)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, NULL, NULL);
  return 0;
}


static int32_t 
bdb_create (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t flags,
	    mode_t mode,
	    fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *pathname = strdup (loc->path);
  char *dir_name = NULL;
  char *db_path = NULL;
  struct stat stbuf = {0,};
  struct bdb_ctx *bctx = NULL;

  dir_name = dirname (pathname);
  MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, dir_name);
  if (((bctx = bdb_get_bctx_from (this, loc->path)) != NULL)) {
    char *key_string = NULL;
    
    lstat (db_path, &stbuf);
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    op_ret = bdb_storage_put (this, bctx, key_string, NULL, 0, 0);
    if (!op_ret) {
      /* create successful */
      struct bdb_fd *bfd = calloc (1, sizeof (*bfd));
      
      bfd->ctx = bdb_ctx_ref (bctx); 
      bfd->key = strdup (key_string);

      if (!bctx->directory)
	bctx->directory = strdup (dir_name);

      bfd->key = strdup (key_string);
      BDB_SET_BFD (this, fd, bfd);
      
      lstat (db_path, &stbuf);
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
      stbuf.st_mode  = mode;
      stbuf.st_size = 0;
    } /* if (!op_ret)...else */
  } else {
    op_ret = -1;
    op_errno = ENOENT;
  }/* if(bctx_data...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd, loc->inode, &stbuf);

  return 0;
}


/* bdb_open
 *
 * as input parameters bdb_open gets the file name, i.e key. bdb_open should effectively 
 * do: store key, open storage db, store storage-db pointer.
 *
 */
static int32_t 
bdb_open (call_frame_t *frame,
	  xlator_t *this,
	  loc_t *loc,
	  int32_t flags,
	  fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  struct bdb_ctx *bctx = NULL;
  struct bdb_fd  *bfd = NULL;
  char *path_name = NULL;

  path_name = strdup (loc->path);

  if (((bctx = bdb_get_bctx_from (this, loc->path)) == NULL)) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    char *key_string = NULL;
    /* we are ready to go now, wat do we do??? do nothing... just place the same ctx which is there in 
     * inode->ctx to fd->ctx... ashTe ashTe anta open storage_db for this directory and place that pointer too,
     * in ctx, check if someone else has opened the same storage db already. */

    /* successfully opened storage db */
    bfd = calloc (1, sizeof (*bfd));
    if (!bfd) {
      op_ret = -1;
      op_errno = ENOMEM;
    } else {
      bfd->ctx = bdb_ctx_ref (bctx);
      
      MAKE_KEY_FROM_PATH (key_string, loc->path);
      bfd->key = strdup (key_string);
      
      BDB_SET_BFD (this, fd, bfd);
    }/* if(!bfd)...else */
  } /* if((inode->ctx == NULL)...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}

/* TODO: don't return more than asked for */
static int32_t 
bdb_readv (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd,
	   size_t size,
	   off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EBADFD;
  struct iovec vec = {0,};
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;  
  dict_t *reply_dict = NULL;
  char *buf = NULL;

  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    struct bdb_cache *bcache = NULL;
    if ((bcache = bdb_lookup_cache (this, bfd->key)) != NULL) {
      vec.iov_base = bcache->data;
      
      if (bcache->size <= size)
	op_ret = vec.iov_len = bcache->size;
      else
	op_ret = vec.iov_len = size;

      reply_dict = get_new_dict ();
    } else {
      /* we are ready to go */
      op_ret = bdb_storage_get (this, bfd->ctx, bfd->key, &buf, size, offset);
      if (op_ret == -1) {
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to do db_storage_get()");
	op_ret = -1;
	op_errno = ENOENT;
      } else if (op_ret) {
	data_t *buf_data = get_new_data ();
	char *db_path = NULL;
	reply_dict = get_new_dict ();
	
	reply_dict->is_locked = 1;
	buf_data->is_locked = 1;
	buf_data->data      = buf;
	buf_data->len       = op_ret;
	
	dict_set (reply_dict, NULL, buf_data);
	
	frame->root->rsp_refs = dict_ref (reply_dict);
	vec.iov_base = buf;
	vec.iov_len = op_ret;
	
	MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bfd->ctx->directory);
	lstat (db_path, &stbuf);
	stbuf.st_ino = fd->inode->ino;
	stbuf.st_size = bdb_storage_get (this, bfd->ctx, bfd->key, &buf, size, offset);
      } /* if(op_ret == -1)...else */
    } /* if (bcache=...)...else */
  }/* if((fd->ctx == NULL)...)...else */
    
  STACK_UNWIND (frame, op_ret, op_errno, &vec, 1, &stbuf);

  if (reply_dict)
    dict_unref (reply_dict);

  return 0;
}


static int32_t 
bdb_writev (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    struct iovec *vector,
	    int32_t count,
	    off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;
 
  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    /* we are ready to go */
      int32_t idx = 0;
      int32_t buf_size = 0;
      char *buf = NULL, *buf_i = NULL;
    
      /* we are not doing writev, we are exposing writev to other, but doing a write using single buffer
       * internally */
      
      /* calculate the size of buffer we would require */
      for (idx = 0; idx < count; idx++) {
	buf_size += vector[idx].iov_len;
      } /* for(idx=0;...)... */
      
      buf = calloc (1, buf_size);
      buf_i = buf;
      
      /* copy to the buffer */
      for (idx = 0; idx < count; idx++) {
	/* page aligned buffer */
	memcpy (buf_i, vector[idx].iov_base, vector[idx].iov_len);
	
	buf_i += vector[idx].iov_len;
	op_ret += vector[idx].iov_len;
      } /* for(idx=0;...)... */
  
      /* we are ready to do bdb_ns_put */
      op_ret = bdb_storage_put (this, bfd->ctx, bfd->key, buf, buf_size, offset);

      if (op_ret) {
	/* write failed */
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to do bdb_storage_put(): %s", db_strerror (op_ret));
	op_ret = -1;
	op_errno = EBADFD; /* TODO: search for a more meaningful errno */
      } else {
	/* NOTE: we want to increment stbuf->st_size, as stored in db */
	stbuf.st_size = offset + buf_size;
	op_ret = buf_size;
	op_errno = 0;
      }/* if(op_ret)...else */
      /* cleanup */
      if (buf)
	free (buf);
  }/* if((fd->ctx == NULL)...)...else */
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  return 0;
}

static int32_t 
bdb_flush (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct bdb_fd *bfd = NULL;

  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "failed to extract fd data from fd=%p", fd);
    op_ret = -1;
    op_errno = EBADF;
  } else {
    /* do nothing, as posix says */
    op_ret = 0;
    op_errno = 0;
  } /* if((fd == NULL)...)...else */
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t 
bdb_close (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = -1;
  int32_t op_errno = EBADFD;
  struct bdb_fd *bfd = NULL;
  
  if ((bfd = bdb_extract_bfd (this, fd)) == NULL){
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    dict_del (fd->ctx, this->name);
    
    bdb_ctx_unref (bfd->ctx);
    bfd->ctx = NULL; 
    
    if (bfd->key)
      free (bfd->key); /* we did strdup() in bdb_open() */
    op_ret = 0;
    op_errno = 0;
  } /* if((fd->ctx == NULL)...)...else */

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_close */


static int32_t 
bdb_fsync (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd,
	   int32_t datasync)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, 0, 0);
  return 0;
}/* bdb_fsync */

static int32_t 
bdb_lk (call_frame_t *frame,
	xlator_t *this,
	fd_t *fd,
	int32_t cmd,
	struct flock *lock)
{
  struct flock nullock = {0, };

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, &nullock);
  return 0;
}/* bdb_lk */

static int32_t
bdb_forget (call_frame_t *frame,
	    xlator_t *this,
	    inode_t *inode)
{
  struct bdb_ctx *bctx = NULL;

  if (inode->ino) {
    MAKE_BCTX_FROM_INODE(this, bctx, inode);
    if (bctx == NULL){
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "forgeting a file, do nothing");
    } else {
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "forget called for directory %s", bctx->directory);
      dict_del (inode->ctx, this->name);
      bdb_remove_ctx (this, bctx);
      bdb_ctx_unref (bctx);
    }
  }
  return 0;
}/* bdb_forget */


/* bdb_lookup
 *
 * bdb_lookup looks up for a pathname in ns_db.db and returns the struct stat as read from ns_db.db,
 * if required
 */
static int32_t
bdb_lookup (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc,
	    int32_t need_xattr)
{
  struct stat stbuf = {0, };
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  dict_t *xattr = NULL;
  char *pathname = NULL, *dir_name = NULL, *real_path = NULL;
  struct bdb_ctx *bctx = NULL;
  char *db_path = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);
  MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, loc->path);
  pathname = strdup (loc->path);
  dir_name = dirname (pathname);

  if (!strcmp (dir_name, loc->path)) {
    /* this is valid only when we are looking up for root */
    /* SPECIAL CASE: looking up root */
    op_ret = lstat (real_path, &stbuf);
    op_errno = errno;
    
    if (op_ret == 0) {
      if ((bctx = bdb_lookup_ctx (this, (char *)loc->path)) != NULL) {
	gf_log (this->name,
		GF_LOG_DEBUG,
		"revalidating root");
      } else {
	bctx = calloc (1, sizeof (*bctx));
	stbuf.st_ino = 1;
	bctx->directory = strdup (dir_name);
	bctx->iseed = 1;
	bdb_ctx_ref (bctx);
	bdb_add_ctx (this, bctx);
	BDB_SET_BCTX (this, loc->inode, bctx);
      }
    } else {
      /* lstat failed, no way we can exist */
      gf_log (this->name,
	      GF_LOG_CRITICAL,
	      "failed to lookup root of this fs");
      op_ret = -1;
      op_errno = ENOTCONN;
    }/* if(op_ret == 0)...else */
  } else {
    char *key_string = NULL;
    
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    op_ret = lstat (real_path, &stbuf);
    if (op_ret == 0){
      /* directory, we do have additional work to do */
      if ((bctx = bdb_lookup_ctx (this, (char *)loc->path)) != NULL) {
	/* revalidating directory inode */
	gf_log (this->name,
		GF_LOG_DEBUG,
		"revalidating directory %s", (char *)loc->path);
	stbuf.st_ino = 	loc->inode->ino;
      } else {
	/* fresh lookup for a directory, lot of work :O */
	struct bdb_ctx *child_bctx = NULL;
	
	child_bctx = calloc (1, sizeof (*child_bctx));
	child_bctx->directory = strdup (loc->path);
	child_bctx->iseed = 1;
	
	bdb_ctx_ref (child_bctx);
	bdb_add_ctx (this, child_bctx);
	BDB_SET_BCTX (this, loc->inode, child_bctx);
	  
	/* getting inode number for loc, this is the right place. we will be 
	 * erroring out if we don't reach this point. */
	/* TODO: use proper iseed */
	stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, child_bctx);
      }/* if((bctx_data = ...)...)...else */
    } else {
      if ((bctx = bdb_get_bctx_from (this, loc->path)) != NULL){
	int32_t entry_size = 0;
	struct bdb_cache *bcache = NULL;

	if ((bcache = bdb_lookup_cache (this, (char *)loc->path))) {
	  op_ret = entry_size = bcache->size;
	} else {
	  op_ret = entry_size = bdb_storage_get (this, bctx, loc->path, NULL, 0, 1);
	}
	if (op_ret == -1) {
	  /* lookup failed, entry doesn't exist */
	  op_ret = -1;
	  op_errno = ENOENT;
	} else {
	  
	  MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, dir_name);
	  op_ret = lstat (db_path, &stbuf);
	  op_errno = errno;
	  if (loc->inode->ino) {
	    /* revalidate */
	    stbuf.st_ino = loc->inode->ino;
	    stbuf.st_size = entry_size;
	  } else {
	    /* fresh lookup, create an inode number */
	    stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
	    stbuf.st_size = entry_size;
	  }/* if(inode->ino)...else */
	}/* if(op_ret == DB_NOTFOUND)...else, after lstat() */
      }/* if(bctx = ...)...else, after bdb_ns_get() */
    } /* if(op_ret == 0)...else, after lstat(real_path, ...) */
  }/* if(loc->parent)...else */
  
  frame->root->rsp_refs = NULL;
  
  if (xattr)
    dict_ref (xattr);
  
  /* NOTE: ns_database of database which houses this entry is kept open */
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf, xattr);
  
  if (xattr)
    dict_unref (xattr);
  
  return 0;
  
}/* bdb_lookup */

static int32_t
bdb_stat (call_frame_t *frame,
	  xlator_t *this,
	  loc_t *loc)
{
 
  struct stat stbuf = {0,};
  char *real_path = NULL;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = lstat (real_path, &stbuf);
  op_errno = errno;
  
  if (op_ret == 0) {
    /* directory, we just need to transform inode number */
    stbuf.st_ino = loc->inode->ino;
  } else {
    struct bdb_ctx *bctx = NULL;

    if ((bctx = bdb_get_bctx_from (this, loc->path)) == NULL) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to get bdb_ctx for %s", loc->path);
      op_ret = -1;
      op_errno = ENOENT; /* TODO: better errno */
    } else {
      char *db_path = NULL;
      MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
      lstat (db_path, &stbuf);

      stbuf.st_size = bdb_storage_get (this, bctx, loc->path, NULL, 0, 0);
      stbuf.st_ino = loc->inode->ino;
    }    
  }

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_stat */



/* bdb_opendir - in the world of bdb, open/opendir is all about opening correspondind databases.
 *               opendir in particular, opens the database for the directory which is
 *               to be opened. after opening the database, a cursor to the database is also created.
 *               cursor helps us get the dentries one after the other, and cursor maintains the state
 *               about current positions in directory. pack 'pointer to db', 'pointer to the
 *               cursor' into struct bdb_dir and store it in fd->ctx, we get from our parent xlator.
 *
 * @frame: call frame
 * @this:  our information, as we filled during init()
 * @loc:   location information
 * @fd:    file descriptor structure (glusterfs internal)
 *
 * return value - immaterial, async call.
 *
 */
static int32_t 
bdb_opendir (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc, 
	     fd_t *fd)
{
  char *real_path;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  struct bdb_ctx *bctx = NULL;
  char *path_name = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);
  path_name = strdup (loc->path);

  if ((bctx = bdb_lookup_ctx (this, (char *)loc->path)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data from private data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    struct bdb_dir *dir_fd = calloc (1, sizeof (*dir_fd));
    
    if (!dir_fd) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to allocate memory for dir_fd. out of memory. :O");
      op_ret = -1;
      op_errno = ENOMEM;
    } else {
      DBC *cursorp = NULL;
      struct bdb_ctx *my_ctx = NULL;
      
      MAKE_BCTX_FROM_INODE(this, my_ctx, loc->inode);
      if (my_ctx) {
	dir_fd->dir = opendir (real_path);
	dir_fd->ctx = bdb_ctx_ref (my_ctx); 
	op_ret = bdb_open_db_cursor (this, my_ctx, &cursorp);
	if (op_ret != 0) {
	  gf_log (this->name,
		  GF_LOG_DEBUG,
		  "failed to open cursor for directory %s", my_ctx->directory);
	  op_ret = -1;
	  op_errno = ENOENT; /* TODO: db error, find better one */
	} else {
	  dir_fd->path = strdup (real_path);
	  dir_fd->key  = NULL;
	  dir_fd->cursorp = cursorp;
	  BDB_SET_BFD (this, fd, dir_fd);
	}
      } else{
	gf_log (this->name,
		GF_LOG_ERROR,
		"failed to get bdb_ctx for a directory: %s", loc->path);
	op_ret = -1;
	op_errno = ENOENT; /* TODO: find a better errno */
      }
    }
  }
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, fd);

  return 0;
}/* bdb_opendir */


static int32_t
bdb_getdents (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      size_t size,
	      off_t off,
	      int32_t flag)
{
  /* TODO: 
   *     1. do readdir.
   *     2. get all 'in-use' entries from database
   *     3. merge the list and give to user
   */
  int32_t op_ret = -1;
  int32_t op_errno = EBADFD;
  dir_entry_t entries = {0, };
  int count = 0;
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &entries, count);
  return 0;
}/* bdb_getdents */


static int32_t 
bdb_closedir (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd)
{
  int32_t op_ret;
  int32_t op_errno;
  struct bdb_dir *dir_fd;

  op_ret = 0;
  op_errno = errno;

  frame->root->rsp_refs = NULL;
  
  if ((dir_fd = (struct bdb_dir *)bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name, 
	    GF_LOG_ERROR, 
	    "failed to extract fd data from fd=%p", fd);
    op_ret = -1;
    op_errno = EBADF;
  } else {
    dict_del (fd->ctx, this->name);
	
    if (dir_fd->path) {
      free (dir_fd->path);
    } else {
      gf_log (this->name, GF_LOG_ERROR, "dir_fd->path was NULL. fd=%p bfd=%p",
	      fd, dir_fd);
    }
    
    if (dir_fd->dir) {
      closedir (dir_fd->dir);
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "dir_fd->dir is NULL.");
    }

    if (dir_fd->cursorp) {
      dir_fd->cursorp->close (dir_fd->cursorp);
      dir_fd->cursorp = NULL;
      bdb_ctx_unref (dir_fd->ctx);
      dir_fd->ctx = NULL; 
    } else {
      gf_log (this->name, GF_LOG_ERROR, "dir_fd->cursorp is NULL");
    }
    free (dir_fd);
  }

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_closedir */


static int32_t 
bdb_readlink (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      size_t size)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, NULL);

  return 0;
}/* bdb_readlink */


static int32_t 
bdb_mkdir (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EEXIST;
  char *real_path = NULL;
  struct stat stbuf = {0, };
  struct bdb_ctx *bctx = NULL;

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = mkdir (real_path, mode);
  op_errno = errno;
  
  if (op_ret == 0) {
    chown (real_path, frame->root->uid, frame->root->gid);
    lstat (real_path, &stbuf);
    
    if ((bctx = bdb_get_bctx_from (this, loc->path)) != NULL) {
      /* we don't have to open db here, lets do during directory revalidate */
      struct bdb_ctx *child_bctx = calloc (1, sizeof (*child_bctx));
	
      if (!child_bctx) {
	op_ret = -1;
	op_errno = ENOMEM;
      } else {
	child_bctx->directory = strdup (loc->path);

	bdb_ctx_ref (child_bctx);
	bdb_add_ctx (this, child_bctx);
	BDB_SET_BCTX (this, loc->inode, child_bctx);
	stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
      } /* if (!ctx)...else */
    }
  } else {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to create directory: %s", loc->path);
    op_ret = -1;
    op_errno = ENOENT;
  }
  
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, loc->inode, &stbuf);

  return 0;
}/* bdb_mkdir */


static int32_t 
bdb_unlink (call_frame_t *frame,
	    xlator_t *this,
	    loc_t *loc)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct bdb_ctx *bctx = NULL;

  if (((bctx = bdb_get_bctx_from (this, loc->path)) == NULL)) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific data", this->name);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    op_ret = bdb_storage_del (this, bctx, loc->path);
    if (op_ret == 0) {
      /* do nothing */
    } else {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to do bdb_storage_del()");
      op_ret = -1;
      op_errno = ENOENT;
    }
  }
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_unlink */

static int32_t 
bdb_remove (const char *path, 
	    const struct stat *stat, 
	    int32_t typeflag, 
	    struct FTW *ftw)
{
  /* TODO: regular remove won't work here, when path is a regular file */
  return remove (path);
} /* bdb_remove */

static int32_t
bdb_rmelem (call_frame_t *frame,
	    xlator_t *this,
	    const char *path)
{
  int32_t op_ret, op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, path);
  op_ret = nftw (real_path, bdb_remove, 20, FTW_DEPTH|FTW_PHYS);
  op_errno = errno;
  /* FTW_DEPTH = traverse subdirs first before calling bdb_remove
   * on real_path
   * FTW_PHYS = do not follow symlinks
   */
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
} /* bdb_rmelm */

static int32_t 
bdb_rmdir (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM);

  return 0;
} /* bdb_rmdir */

static int32_t 
bdb_symlink (call_frame_t *frame,
	     xlator_t *this,
	     const char *linkname,
	     loc_t *loc)
{
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, -1, EPERM, loc->inode, NULL);
  return 0;
} /* bdb_symlink */

static int32_t 
bdb_chmod (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = lstat (real_path, &stbuf);
  
  if (op_ret == 0) {
    /* directory */
    op_ret = chmod (real_path, mode);
    op_errno = errno;
  } else {
    op_ret = -1;
    op_errno = EPERM;
  }/* if(op_ret == 0)...else */
    
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_chmod */


static int32_t 
bdb_chown (call_frame_t *frame,
	   xlator_t *this,
	   loc_t *loc,
	   uid_t uid,
	   gid_t gid)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};

  MAKE_REAL_PATH (real_path, this, loc->path);
  
  op_ret = lstat (real_path, &stbuf);
  if (op_ret == 0) {
    /* directory */
    op_ret = lchown (real_path, uid, gid);
    op_errno = errno;
  } else {
    /* not a directory */
    op_ret = -1;
    op_errno = EPERM;
  }/* if(op_ret == 0)...else */
    
  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_chown */


static int32_t 
bdb_truncate (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  char *real_path;
  struct stat stbuf = {0,};
  char *db_path = NULL;
  struct bdb_ctx *bctx = NULL;
  char *key_string = NULL;

  if ((bctx = bdb_get_bctx_from (this, loc->path)) == NULL) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to fetch bdb_ctx for path: %s", loc->path);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    MAKE_REAL_PATH (real_path, this, loc->path);
    MAKE_KEY_FROM_PATH (key_string, loc->path);
    
    /* now truncate */
    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
    lstat (db_path, &stbuf);
    if (loc->inode->ino) {
      stbuf.st_ino = loc->inode->ino;
    }else {
      stbuf.st_ino = bdb_inode_transform (stbuf.st_ino, bctx);
    }
    
    op_ret = bdb_storage_put (this, bctx, key_string, NULL, 0, 1);
    if (op_ret == -1) {
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "failed to do bdb_storage_put");
      op_ret = -1;
      op_errno = ENOENT; /* TODO: better errno */
    } else {
      /* do nothing */
    }/* if(op_ret == -1)...else */
  }
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  
  return 0;
}/* bdb_truncate */


static int32_t 
bdb_utimens (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     struct timespec ts[2])
{
  int32_t op_ret = 0;
  int32_t op_errno = 0;
  struct stat stbuf = {0, };
  struct timeval tv[2];
  char *db_path = NULL, *dir_name = NULL;

  dir_name = strdup (loc->path);
  dir_name = dirname (dir_name);

  MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, dir_name);
  tv[0].tv_sec = ts[0].tv_sec;
  tv[0].tv_usec = ts[0].tv_nsec / 1000;
  tv[1].tv_sec = ts[1].tv_sec;
  tv[1].tv_usec = ts[1].tv_nsec / 1000;
  
  op_ret = lutimes (db_path, tv);
  if (op_ret == -1 && errno == ENOSYS) {
    op_ret = utimes (db_path, tv);
  }
  op_errno = errno;

  lstat (db_path, &stbuf);
  stbuf.st_ino = loc->inode->ino;

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);

  return 0;
}/* bdb_utimens */

static int32_t 
bdb_statfs (call_frame_t *frame,
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

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &buf);
  return 0;
}/* bdb_statfs */

static int32_t
bdb_incver (call_frame_t *frame,
	      xlator_t *this,
	      const char *path)
{
  /* TODO: version exists for directory, version is consistent for every entry in the directory */
  char *real_path;
  char version[50];
  int32_t size = 0;
  int32_t ver = 0;

  MAKE_REAL_PATH (real_path, this, path);

  size = lgetxattr (real_path, GLUSTERFS_VERSION, version, 50);
  if ((size == -1) && (errno != ENODATA)) {
    STACK_UNWIND (frame, -1, errno);
    return 0;
  } else {
    version[size] = '\0';
    ver = strtoll (version, NULL, 10);
  }
  ver++;
  sprintf (version, "%u", ver);
  lsetxattr (real_path, GLUSTERFS_VERSION, version, strlen (version), 0);
  STACK_UNWIND (frame, ver, 0);

  return 0;
}/* bdb_incver */

static int32_t 
bdb_setxattr (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      dict_t *dict,
	      int flags)
{
  int32_t ret = -1;
  int32_t op_ret = -1;
  int32_t op_errno = ENOENT;
  data_pair_t *trav = dict->members_list;
  struct bdb_ctx *bctx = NULL;
  
  MAKE_BCTX_FROM_INODE (this, bctx, loc->inode);

  if (S_ISDIR (loc->inode->st_mode)) {
    while (trav) {
      if ((ret = bdb_storage_put (this, bctx, trav->key, trav->value->data, trav->value->len, 0)) != 0) {
	op_ret   = -1;
	op_errno = ret;
	break;
      } else {
	op_ret = 0;
	op_errno = 0;
      }
      trav = trav->next;
    }
  } else {
    op_ret   = -1;
    op_errno = EPERM;
  }

  frame->root->rsp_refs = NULL;

  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;  
}/* bdb_setxattr */

static int32_t 
bdb_getxattr (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  int32_t op_ret = -1, op_errno = EPERM;
  dict_t *dict = get_new_dict ();
  /*  struct bdb_ctx *bctx = NULL; */

  STACK_UNWIND (frame, op_ret, op_errno, dict);

  if (dict)
    dict_destroy (dict);
  
  return 0;
#if 0  
  MAKE_BCTX_FROM_INODE (this, bctx, loc->inode);

  if (S_ISDIR (loc->inode->st_mode)) {
    op_ret = bdb_storage_get (this, bctx, key, &buf, 0, 0);
    if (op_ret == -1) {
      gf_log (this->name,
	      GF_LOG_ERROR,
	      "failed to do bdb_storage_get for key: %s in directory: %s", key, bctx->directory);
    } else {
      /* successfully read */
    }
  } else {
    op_ret   = -1;
    op_errno = EPERM;
  }

  STACK_UNWIND (frame, 0, 0, dict);

  if (dict)
    dict_destroy (dict);
  
  return 0;
#endif
}/* bdb_getxattr */


static int32_t 
bdb_removexattr (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc,
                 const char *name)
{
  int32_t op_ret = -1, op_errno = EPERM;
  struct bdb_ctx *bctx = NULL;

  MAKE_BCTX_FROM_INODE (this, bctx, loc->inode);
  
  op_ret = bdb_storage_del (this, bctx, name);
  
  if (op_ret == -1) {
    op_errno = ENOENT;
  } else {
    op_ret = 0;
    op_errno = 0;
  }
  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}/* bdb_removexattr */


static int32_t 
bdb_fsyncdir (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		int datasync)
{
  int32_t op_ret;
  int32_t op_errno;
  struct bdb_fd *bfd = NULL;

  frame->root->rsp_refs = NULL;

  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR,
	    "bfd is NULL fd=%p", fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {

    op_ret = 0;
    op_errno = errno;
  }

  STACK_UNWIND (frame, op_ret, op_errno);

  return 0;
}/* bdb_fsycndir */


static int32_t 
bdb_access (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t mask)
{
  int32_t op_ret;
  int32_t op_errno;
  char *real_path;

  MAKE_REAL_PATH (real_path, this, loc->path);

  op_ret = access (real_path, mask);
  op_errno = errno;

  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}/* bdb_access */


static int32_t 
bdb_ftruncate (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 off_t offset)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

static int32_t 
bdb_fchown (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      uid_t uid,
	      gid_t gid)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}


static int32_t 
bdb_fchmod (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      mode_t mode)
{
  int32_t op_ret = -1;
  int32_t op_errno = EPERM;
  struct stat buf = {0,};

  frame->root->rsp_refs = NULL;  
  STACK_UNWIND (frame, op_ret, op_errno, &buf);

  return 0;
}

static int32_t 
bdb_setdents (call_frame_t *frame,
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
  struct bdb_fd *bfd;

  frame->root->rsp_refs = NULL;

  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR, "bfd is NULL on fd=%p", fd);
  } else {

    real_path = bfd->key; /* TODO: fucked up, it was bfd->path earlier. we need to rebuild real_path
			   * using bfd->ctx->directory and bfd->key */
    
    if (!real_path) {
      gf_log (this->name, GF_LOG_ERROR,
	      "path is NULL on bfd=%p fd=%p", bfd, fd);
      STACK_UNWIND (frame, -1, EBADFD);
      return 0;
    }
    
    real_path_len = strlen (real_path);
    entry_path_len = real_path_len + 256;
    entry_path = calloc (1, entry_path_len);
    
    if (!entry_path) {
      STACK_UNWIND (frame, -1, ENOMEM);
      return 0;
    }
    
    strcpy (entry_path, real_path);
    entry_path[real_path_len] = '/';
    
    /* fd exists, and everything looks fine */
    {
      /**
       * create an entry for each one present in '@entries' 
       *  - if flag is set, create both directories and 
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
	} else if (flags == GF_SET_IF_NOT_PRESENT || flags != GF_SET_DIR_ONLY) {
	  /* Create a 0 byte file here */
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
	/* TODO: handle another flag, GF_SET_OVERWRITE */
	
	/* Change the mode */
	chmod (pathname, trav->buf.st_mode);
	/* change the ownership */
	chown (pathname, trav->buf.st_uid, trav->buf.st_gid);
	
	/* consider the next entry */
	trav = trav->next;
      }
    }
    //  op_errno = errno;
  }
  /* Return success all the time */
  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, 0, 0);
  
  freee (entry_path);
  return 0;
}

static int32_t 
bdb_fstat (call_frame_t *frame,
	   xlator_t *this,
	   fd_t *fd)
{
  int32_t op_ret = 0;
  int32_t op_errno = ENOENT;
  struct stat stbuf = {0,};
  struct bdb_fd *bfd = NULL;

  if ((bfd = bdb_extract_bfd (this, fd)) == NULL){
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "failed to extract %s specific information from fd:%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    struct bdb_ctx *bctx = bfd->ctx;
    char *db_path = NULL;

    MAKE_REAL_PATH_TO_STORAGE_DB (db_path, this, bctx->directory);
    lstat (db_path, &stbuf);

    stbuf.st_ino = fd->inode->ino;
    stbuf.st_size = bdb_storage_get (this, bctx, bfd->key, NULL, 0, 0);
  }

  frame->root->rsp_refs = NULL;

  STACK_UNWIND (frame, op_ret, op_errno, &stbuf);
  return 0;
}


static int32_t
bdb_readdir (call_frame_t *frame,
	     xlator_t *this,
	     fd_t *fd,
	     size_t size,
	     off_t off)
{
  struct bdb_dir *bfd = NULL;
  int32_t this_size = 0;
  int32_t op_ret = -1, op_errno = 0;
  char *buf = NULL;
  size_t filled = 0;
  struct stat stbuf = {0,};


  if ((bfd = bdb_extract_bfd (this, fd)) == NULL) {
    gf_log (this->name, GF_LOG_ERROR,
	    "failed to extract %s specific fd information from fd=%p", this->name, fd);
    op_ret = -1;
    op_errno = EBADFD;
  } else {
    buf = calloc (size, 1); /* readdir buffer needs 0 padding */
    
    if (!buf) {
      gf_log (this->name, GF_LOG_ERROR,
	      "malloc (%d) returned NULL", size);
      STACK_UNWIND (frame, -1, ENOMEM, NULL);
      return 0;
    }

    while (filled <= size) {
      gf_dirent_t *this_entry;
      struct dirent *entry;
      off_t in_case;
      int32_t this_size;

      in_case = telldir (bfd->dir);
      entry = readdir (bfd->dir);
      if (!entry)
	break;

      this_size = dirent_size (entry);

      if (this_size + filled > size) {
	seekdir (bfd->dir, in_case);
	break;
      }

      if (!IS_BDB_PRIVATE_FILE(entry->d_name)) {
	/* TODO - consider endianness here */
	this_entry = (void *)(buf + filled);
	this_entry->d_ino = entry->d_ino;
	
	this_entry->d_off = entry->d_off;
	
	this_entry->d_type = entry->d_type;
	this_entry->d_len = entry->d_reclen;
	strncpy (this_entry->d_name, entry->d_name, this_entry->d_len);
	
	filled += this_size;
      }/* if(!IS_BDB_PRIVATE_FILE()) */
    }

    lstat (bfd->path, &stbuf);
    
    if (filled < size) {
      /* hungry kyaa? */
      while (filled <= size) {
	gf_dirent_t *this_entry = NULL;
	DBT key = {0,}, value = {0,};
	
	key.flags = DB_DBT_MALLOC;
	value.flags = DB_DBT_MALLOC;
	op_ret = bdb_cursor_get (bfd->cursorp, &key, &value, DB_NEXT);
	
	if (op_ret == DB_NOTFOUND) {
	  /* we reached end of the directory */
	  break;
	} else if (op_ret == 0){
	  
	  if (key.data) {
	    this_size = bdb_dirent_size (&key);
	    if (this_size + filled > size)
	      break;
	    /* TODO - consider endianness here */
	    this_entry = (void *)(buf + filled);
	    /* FIXME: bug, if someone is going to use ->d_ino */
	    this_entry->d_ino = bdb_inode_transform (stbuf.st_ino, bfd->ctx);
	    this_entry->d_off = 0;
	    this_entry->d_type = 0;
	    this_entry->d_len = key.size;
	    strncpy (this_entry->d_name, (char *)key.data, key.size);
	   
	    if (key.data)
	      free (key.data);
	    if (value.data)
	      free (value.data);
	    
	    filled += this_size;
	  } else {
	    /* NOTE: currently ignore when we get key.data == NULL, TODO: we should not get key.data = NULL */
	    gf_log (this->name,
		    GF_LOG_DEBUG,
		    "null key read from db");
	  }/* if(key.data)...else */
	} else {
	  gf_log (this->name,
		  GF_LOG_DEBUG,
		  "database error during readdir");
	  op_ret = -1;
	  op_errno = ENOMEM;
	  break;
	} /* if (op_ret == DB_NOTFOUND)...else if...else */
      }
    } /* while */
  }

  frame->root->rsp_refs = NULL;
  gf_log (this->name,
	  GF_LOG_DEBUG,
	  "read %d bytes", filled);
  STACK_UNWIND (frame, filled, op_errno, buf);

  free (buf);
    
  return 0;
}


static int32_t 
bdb_stats (call_frame_t *frame,
	     xlator_t *this,
	     int32_t flags)

{
  int32_t op_ret = 0;
  int32_t op_errno = 0;

  struct xlator_stats xlstats = {0, }, *stats = &xlstats;
  struct statvfs buf;
  struct timeval tv;
  struct bdb_private *priv = (struct bdb_private *)this->private;
  int64_t avg_read = 0;
  int64_t avg_write = 0;
  int64_t _time_ms = 0; 

    
  op_ret = statvfs (priv->base_path, &buf);
  op_errno = errno;
    
  
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

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, stats);
  return 0;
}

static int32_t 
bdb_checksum (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      int32_t flag)
{
  char *real_path;
  DIR *dir;
  struct dirent *dirent;
  uint8_t file_checksum[4096] = {0,};
  uint8_t dir_checksum[4096] = {0,};
  int32_t op_ret = -1;
  int32_t op_errno = 2;
  int32_t i, length = 0;
  MAKE_REAL_PATH (real_path, this, loc->path);

  dir = opendir (real_path);
  
  if (!dir){
    gf_log (this->name, GF_LOG_DEBUG, 
	    "checksum: opendir() failed for `%s'", real_path);
    frame->root->rsp_refs = NULL;
    STACK_UNWIND (frame, -1, errno, NULL, NULL);
    return 0;
  } else {
    op_ret = 0;
    op_errno = 0;
  }

  /* TODO: read the filename in db file and send the checksum */
  while ((dirent = readdir (dir))) {
    //struct stat buf;

    if (!dirent)
      break;

    length = strlen (dirent->d_name);

    if (strcmp (dirent->d_name, "glusterfs.db") == 0) 
      continue;

    //lstat (dirent->d_name, &buf);
    
    for (i = 0; i < length; i++)
      dir_checksum[i] ^= dirent->d_name[i];
    {
      /* TODO: */
      /* There will be one file 'glusterfs.db' */
      /* retrive keys from it and send it accross */
    }
  }
  closedir (dir);

  frame->root->rsp_refs = NULL;
  STACK_UNWIND (frame, op_ret, op_errno, file_checksum, dir_checksum);

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
	/* Tell the parent that bdb xlator is up */
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
  int32_t ret;
  struct stat buf;
  struct bdb_private *_private = calloc (1, sizeof (*_private));
  data_t *directory = dict_get (this->options, "directory");

  if (this->children) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "FATAL: storage/bdb cannot have subvolumes");
    freee (_private);
    return -1;
  }

  if (!directory) {
    gf_log (this->name, GF_LOG_ERROR,
	    "export directory not specified in spec file");
    freee (_private);
    return -1;
  }
  umask (000); // umask `masking' is done at the client side
  if (mkdir (directory->data, 0777) == 0) {
    gf_log (this->name, GF_LOG_WARNING,
	    "directory specified not exists, created");
  }
  /* Check whether the specified directory exists, if not create it. */
  ret = stat (directory->data, &buf);
  if (ret != 0 && !S_ISDIR (buf.st_mode)) {
    gf_log (this->name, GF_LOG_ERROR, 
	    "Specified directory doesn't exists, Exiting");
    freee (_private);
    return -1;
  }

  {
    _private->dbenv = bdb_init_db_env (this, directory->data);
    
    if (!_private->dbenv) {
      gf_log (this->name, GF_LOG_ERROR,
	      "failed to initialize db environment");
      freee (_private);
      return -1;
    }
    
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
  {
    ret = bdb_init_db (this, directory->data);
    INIT_LIST_HEAD (&_private->c_list);
    
    if (ret == -1){
      gf_log (this->name,
	      GF_LOG_DEBUG,
	      "failed to initialize database");
      return -1;
    }
  }
  return 0;
}


void
fini (xlator_t *this)
{
  struct bdb_private *private = this->private;
  if (private->dbenv) {
    /* pick each of the 'struct bdb_ctx' from private->db_ctx and close all the databases that are open */
    dict_foreach (private->db_ctx, bdb_close_dbs_from_dict, NULL);
  } else {
    /* impossible to reach here */
  }

  freee (private);
  return;
}

struct xlator_mops mops = {
  .stats    = bdb_stats,
  .lock     = mop_lock_impl,
  .unlock   = mop_unlock_impl,
  .checksum = bdb_checksum,
};

struct xlator_fops fops = {
  .lookup      = bdb_lookup,
  .forget      = bdb_forget,
  .stat        = bdb_stat,
  .opendir     = bdb_opendir,
  .readdir     = bdb_readdir,
  .closedir    = bdb_closedir,
  .readlink    = bdb_readlink,
  .mknod       = bdb_mknod,
  .mkdir       = bdb_mkdir,
  .unlink      = bdb_unlink,
  .rmelem      = bdb_rmelem,
  .rmdir       = bdb_rmdir,
  .symlink     = bdb_symlink,
  .rename      = bdb_rename,
  .link        = bdb_link,
  .chmod       = bdb_chmod,
  .chown       = bdb_chown,
  .truncate    = bdb_truncate,
  .utimens     = bdb_utimens,
  .create      = bdb_create,
  .open        = bdb_open,
  .readv       = bdb_readv,
  .writev      = bdb_writev,
  .statfs      = bdb_statfs,
  .flush       = bdb_flush,
  .close       = bdb_close,
  .fsync       = bdb_fsync,
  .incver      = bdb_incver,
  .setxattr    = bdb_setxattr,
  .getxattr    = bdb_getxattr,
  .removexattr = bdb_removexattr,
  .fsyncdir    = bdb_fsyncdir,
  .access      = bdb_access,
  .ftruncate   = bdb_ftruncate,
  .fstat       = bdb_fstat,
  .lk          = bdb_lk,
  .fchown      = bdb_fchown,
  .fchmod      = bdb_fchmod,
  .setdents    = bdb_setdents,
  .getdents    = bdb_getdents,
};
