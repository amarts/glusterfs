/*
  (C) 2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

/**
 * xlators/cluster/stripe:
 *    Stripe translator, stripes the data accross its child nodes, 
 *    as per the options given in the spec file. The striping works 
 *    fairly simple. It writes files at different offset as per 
 *    calculation. So, 'ls -l' output at the real posix level will 
 *    show file size bigger than the actual size. But when one does 
 *    'df' or 'du <file>', real size of the file on the server is shown.
 *
 * WARNING:
 *    Stripe translator can't regenerate data if a child node gets disconnected.
 *    So, no 'self-heal' for stripe. Hence the advice, use stripe only when its 
 *    very much necessary, or else, use it in combination with AFR, to have a 
 *    backup copy. 
 */

#include "xlator.h"
#include "logging.h"
#include "defaults.h"
#include <fnmatch.h>

#ifdef STATIC
#undef STATIC
#endif
#define STATIC /* static */

#define STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR(_loc) do { \
  if (!(_loc && _loc->inode && _loc->inode->ctx &&          \
	dict_get (_loc->inode->ctx, this->name))) {         \
    gf_log (this->name, GF_LOG_ERROR, "inode_ctx");         \
    STACK_UNWIND (frame, -1, EINVAL, NULL, NULL, NULL);     \
    return 0;                                               \
  }                                                         \
} while(0)

struct stripe_local;

/**
 * struct stripe_options : This keeps the pattern and the block-size 
 *     information, which is used for striping on a file.
 */
struct stripe_options {
  struct stripe_options *next;
  char path_pattern[256];
  int32_t block_size;
};

/**
 * Private structure for stripe translator 
 */
struct stripe_private {
  struct stripe_options *pattern;
  xlator_t **xl_array;
  gf_lock_t lock;
  int8_t nodes_down;
  int8_t first_child_down;
  int8_t child_count;
};

/**
 * Used to keep info about the replies received from fops->readv calls 
 */
struct readv_replies {
  struct iovec *vector;
  int32_t count; //count of vector
  int32_t op_ret;   //op_ret of readv
  int32_t op_errno;
  struct stat stbuf; /* 'stbuf' is also a part of reply */
};

/**
 * Local structure to be passed with all the frames in case of STACK_WIND
 */
struct stripe_local {
  struct stripe_local *next;
  call_frame_t *orig_frame; //

  /* Used by _cbk functions */
  int8_t node_index;
  int8_t revalidate;
  int8_t failed;
  int8_t unwind;
  int8_t hint;

  int8_t call_count;
  int8_t wind_count; // used instead of child_cound in case of read and write */
  int32_t op_ret;
  int32_t op_errno; 
  int32_t count;
  int32_t flags;
  char *path;
  struct stat stbuf;
  struct readv_replies *replies;
  struct statvfs statvfs_buf;
  dir_entry_t *entry;
  dir_entry_t *last;
  struct xlator_stats stats;
  inode_t *inode;

  /* For File I/O fops */
  dict_t *ctx;

  /* General usage */
  off_t offset;
  off_t stripe_size;

  int8_t *list;
  struct flock lock;
  fd_t *fd;
  void *value;
};

typedef struct stripe_local   stripe_local_t;
typedef struct stripe_private stripe_private_t;

/**
 * stripe_get_matching_bs - Get the matching block size for the given path.
 */
STATIC int32_t 
stripe_get_matching_bs (const char *path, struct stripe_options *opts) 
{
  struct stripe_options *trav = opts;
  char *pathname = strdup (path);
  while (trav) {
    if (fnmatch (trav->path_pattern, pathname, FNM_NOESCAPE) == 0) {
      free (pathname);
      return trav->block_size;
    }
    trav = trav->next;
  }
  free (pathname);
  return 0;
}

/*
 * stripe_common_cbk -
 */
STATIC int32_t
stripe_common_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

/**
 * stripe_stack_unwind_cbk -  This function is used for all the _cbk without 
 *     any extra arguments (other than the minimum given)
 * This is called from functions like forget,fsync,unlink,rmdir,close,closedir etc.
 *
 */
STATIC int32_t 
stripe_stack_unwind_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *this,
			 int32_t op_ret,
			 int32_t op_errno)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
      } else {
	local->op_errno = op_errno;
      }
    }
    if (op_ret >= 0) 
      local->op_ret = op_ret;
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
      local->op_errno = EIO; /* TODO: Or should it be ENOENT? */
    }
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

STATIC int32_t 
stripe_common_buf_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       struct stat *buf)
{
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

/**
 * stripe_stack_unwind_buf_cbk -  This function is used for all the _cbk with 
 *    'struct stat *buf' as extra argument (other than minimum)
 * This is called from functions like, chmod,fchmod,chown,fchown,truncate,ftruncate,
 *   utimens etc.
 *
 * @cookie - this argument should be always 'xlator_t *' of child node 
 */
STATIC int32_t 
stripe_stack_unwind_buf_cbk (call_frame_t *frame,
			     void *cookie,
			     xlator_t *this,
			     int32_t op_ret,
			     int32_t op_errno,
			     struct stat *buf)
{
  stripe_local_t *local = (void *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND(frame,
	       stripe_setxattr_cbk,
	       trav->xlator,
	       trav->xlator->fops->setxattr,
	       path,
	       name,
	       value,
	       size,
	       flags);
    trav = trav->next;
  }
  return 0;
}

static int32_t
stripe_getxattr_cbk (call_frame_t *frame,
		     call_frame_t *prev_frame,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     void *value)
{
  STACK_UNWIND (frame, op_ret, op_errno, value);
  return 0;
}

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
      } else {
	local->op_errno = op_errno;
      }
    }
    
    if (op_ret == 0) {
      local->op_ret = 0;
      if (local->stbuf.st_blksize == 0)
	local->stbuf = *buf;

      if (FIRST_CHILD(this) == ((call_frame_t *)cookie)->this) {
	/* Always, pass the inode number of first child to the above layer */
	local->stbuf.st_ino = buf->st_ino;
      }

      if (local->stbuf.st_size < buf->st_size)
	local->stbuf.st_size = buf->st_size;
      local->stbuf.st_blocks += buf->st_blocks;
      if (local->stbuf.st_blksize != buf->st_blksize) {
	/* TODO: add to blocks in terms of original block size */
      }
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
      local->op_errno = ENOENT;
    }
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

STATIC int32_t 
stripe_common_inode_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *this,
			 int32_t op_ret,
			 int32_t op_errno,
			 inode_t *inode,
			 struct stat *buf)
{
  dict_set (inode->ctx, this->name, data_from_int8 (1)); // not stripped
  STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
  return 0;
}

/**
 * stripe_stack_unwind_inode_cbk - This is called by the function like, 
 *                   link (), symlink (), mkdir (), mknod () 
 *           This creates a inode for new inode. It keeps a list of all 
 *           the inodes received from the child nodes. It is used while 
 *           forwarding any fops to child nodes.
 *
 */
STATIC int32_t 
stripe_stack_unwind_inode_cbk (call_frame_t *frame,
			       void *cookie,
			       xlator_t *this,
			       int32_t op_ret,
			       int32_t op_errno,
			       inode_t *inode,
			       struct stat *buf)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    
    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
      } else {
	local->op_errno = op_errno;
      }
    }
 
    if (op_ret >= 0) {
      local->op_ret = 0;

      if (local->stbuf.st_blksize == 0) {
	local->inode = inode;
	local->stbuf = *buf;
      }
      if (FIRST_CHILD(this) == ((call_frame_t *)cookie)->this) {
	local->stbuf.st_ino = buf->st_ino;
	local->hint++;
      } else {
	local->hint = 2;
      }
      if (local->stbuf.st_size < buf->st_size)
	local->stbuf.st_size = buf->st_size;
      local->stbuf.st_blocks += buf->st_blocks;
      if (local->stbuf.st_blksize != buf->st_blksize) {
	/* TODO: add to blocks in terms of original block size */
      }
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
      local->op_errno = ENOENT;
    }
    if (local->op_ret == 0) {
      if (!local->revalidate) {
	if (local->hint == 1) {
	  dict_set (local->inode->ctx, 
		    this->name, 
		    data_from_int8 (1)); // not stripped
	} else {
	  dict_set (local->inode->ctx, 
		    this->name, 
		    data_from_int8 (2)); // stripped
	}
      }
    }

    STACK_UNWIND (frame, 
		  local->op_ret, 
		  local->op_errno, 
		  local->inode, 
		  &local->stbuf);
  }

  return 0;
}


/**
 * stripe_lookup -
 */
int32_t 
stripe_lookup (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc)
{
  stripe_local_t *local = NULL;
  xlator_list_t *trav = NULL;
  stripe_private_t *priv = this->private;

  if (!(loc && loc->inode && loc->inode->ctx)) {
    gf_log (this->name, GF_LOG_ERROR, "wrong argument");
    STACK_UNWIND (frame, -1, EINVAL, NULL, NULL);
    return 0;
  }

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;
  
  if (dict_get (loc->inode->ctx, this->name))
    local->hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  
  if (!local->hint) {
    /* Everytime in stripe lookup, all child nodes should be looked up */
    local->call_count = priv->child_count;
    trav = this->children;
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_inode_cbk,
		  trav->xlator,
		  trav->xlator->fops->lookup,
		  loc);
      trav = trav->next;
    }
  } else {
    local->revalidate = 1;
    local->inode = loc->inode;

    if (local->hint == 1) 
      local->call_count = 1;
    else 
      local->call_count = ((stripe_private_t *)this->private)->child_count;

    trav = this->children;
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_inode_cbk,
		  trav->xlator,
		  trav->xlator->fops->lookup,
		  loc);
      if (local->hint == 1)
	break;
      trav = trav->next;
    }
  }
  
  return 0;
}

static int32_t
stripe_open (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     int32_t flags,
	     mode_t mode)
{
  stripe_private_t *priv = xl->private;
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_ret = -1;
  local->op_errno = ENOENT;

  local->ctx = get_new_dict ();
  local->stripe_size = stripe_get_matching_bs (path, priv->pattern);
  LOCK_INIT (&frame->mutex);
  if (local->stripe_size) {
    while (trav) {
      STACK_WIND (frame,
		  stripe_open_cbk,
		  trav->xlator,
		  trav->xlator->fops->open,
		  path,
		  flags,
		  mode);
      trav = trav->next;
    }
  } else {
    STACK_WIND (frame,
		stripe_open_cbk,
		xl->children->xlator,
		xl->children->xlator->fops->open,
		path,
		flags,
		mode);
  }
  return 0;
}

static int32_t 
stripe_single_readv_cbk (call_frame_t *frame,
			 call_frame_t *prev_frame,
			 xlator_t *xl,
			 int32_t op_ret,
			 int32_t op_errno,
			 struct iovec *vector,
			 int32_t count)
{
  STACK_UNWIND (frame, op_ret, op_errno, vector, count);
  return 0;
}

static int32_t
stripe_readv_cbk (call_frame_t *frame,
		  call_frame_t *prev_frame,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct iovec *vector,
		  int32_t count)
{
  /* There is nothing to be done */
  return 0;
}


/**
 * stripe_stat -
 */
int32_t
stripe_stat (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc)
{ 
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  stripe_local_t *local = NULL;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->stat,
		loc);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = loc->inode;
    local->call_count = priv->child_count;
    
    trav = this->children;
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->stat,
		  loc);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_chmod -
 */
int32_t
stripe_chmod (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      mode_t mode)
{
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  stripe_local_t *local = NULL;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->chmod,
		loc,
		mode);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = loc->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->chmod,
		  loc,
		  mode);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_chown - 
 */
int32_t
stripe_chown (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      uid_t uid,
	      gid_t gid)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->chown,
		loc,
		uid,
		gid);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = loc->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->chown,
		  loc,
		  uid,
		  gid);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_statfs_cbk - 
 */
STATIC int32_t
stripe_statfs_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct statvfs *stbuf)
{
  stripe_local_t *local = (stripe_local_t *)frame->local;
  int32_t callcnt;
  LOCK(&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret != 0 && op_errno != ENOTCONN) {
      local->op_errno = op_errno;
    }
    if (op_ret == 0) {
      
      struct statvfs *dict_buf = &local->statvfs_buf;
      dict_buf->f_bsize   = stbuf->f_bsize;
      dict_buf->f_frsize  = stbuf->f_frsize;
      dict_buf->f_blocks += stbuf->f_blocks;
      dict_buf->f_bfree  += stbuf->f_bfree;
      dict_buf->f_bavail += stbuf->f_bavail;
      dict_buf->f_files  += stbuf->f_files;
      dict_buf->f_ffree  += stbuf->f_ffree;
      dict_buf->f_favail += stbuf->f_favail;
      dict_buf->f_fsid    = stbuf->f_fsid;
      dict_buf->f_flag    = stbuf->f_flag;
      dict_buf->f_namemax = stbuf->f_namemax;
      local->op_ret = 0;
    }
  }
  UNLOCK (&frame->lock);
  
  if (!callcnt) {
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->statvfs_buf);
  }
  
  return 0;
}

static int32_t
stripe_writev (call_frame_t *frame,
	       xlator_t *xl,
	       dict_t *file_ctx,
	       struct iovec *vector,
	       int32_t count,
	       off_t offset)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  int32_t total_size = 0;
  int32_t i = 0;
  int32_t tmp_count = count;
  struct iovec *tmp_vec = vector;
  stripe_private_t *priv = xl->private;

  local->stripe_size = data_to_int (dict_get (file_ctx, frame->this->name));
  if (!local->stripe_size) {
    /* File exists only on first node */
    data_t *ctx_data = dict_get (file_ctx, xl->children->xlator->name);
    dict_t *ctx = (void *)((long)data_to_int(ctx_data));
    STACK_WIND (frame,
		stripe_single_writev_cbk,
		xl->children->xlator,
		xl->children->xlator->fops->writev,
		ctx,
		vector,
		count,
		offset);
    return 0;
  }

/**
 * stripe_statfs - 
 */
int32_t
stripe_statfs (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc)
{
  stripe_local_t *local = NULL;
  xlator_list_t *trav = this->children;

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;

  local->call_count = ((stripe_private_t *)this->private)->child_count;
  while (trav) {
    STACK_WIND (frame,
		stripe_statfs_cbk,
		trav->xlator,
		trav->xlator->fops->statfs,
		loc);
    trav = trav->next;
  }

  return 0;
}


/**
 * stripe_truncate - 
 */
int32_t
stripe_truncate (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc,
		 off_t offset)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->truncate,
		loc,
		offset);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = loc->inode;
    local->call_count = priv->child_count;
    
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->truncate,
		  loc,
		  offset);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_utimens - 
 */
int32_t 
stripe_utimens (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		struct timespec tv[2])
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->utimens,
		loc,
		tv);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = loc->inode;
    local->call_count = priv->child_count;
    
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->utimens,
		  loc,
		  tv);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_rename - 
 */
int32_t
stripe_rename (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *oldloc,
	       loc_t *newloc)
{
  stripe_private_t *priv = this->private;
  stripe_local_t *local = NULL;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (oldloc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (oldloc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->rename,
		oldloc,
		newloc);
    /* TODO: if oldloc is unstripped, and newloc is stripped file, send unlink to those nodes */

  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = oldloc->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->rename,
		  oldloc,
		  newloc);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_access - 
 */
int32_t
stripe_access (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc,
	       int32_t mask)
{
  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  STACK_WIND (frame,
	      stripe_common_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->access,
	      loc,
	      mask);

  return 0;
}


/**
 * stripe_readlink_cbk - 
 */
STATIC int32_t 
stripe_readlink_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     const char *path)
{
  STACK_UNWIND (frame, op_ret, op_errno, path);

  return 0;
}


/**
 * stripe_readlink - 
 */
int32_t
stripe_readlink (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc,
		 size_t size)
{
  stripe_private_t *priv = this->private;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  STACK_WIND (frame,
	      stripe_readlink_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->readlink,
	      loc,
	      size);

  return 0;
}


/**
 * stripe_unlink - 
 */
int32_t
stripe_unlink (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO);
    return 0;
  }
 
  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_cbk,
		trav->xlator,
		trav->xlator->fops->unlink,
		loc);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->call_count = priv->child_count;
    
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_cbk,
		  trav->xlator,
		  trav->xlator->fops->unlink,
		  loc);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_rmdir - 
 */
int32_t
stripe_rmdir (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_cbk,
		trav->xlator,
		trav->xlator->fops->rmdir,
		loc);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_cbk,
		  trav->xlator,
		  trav->xlator->fops->rmdir,
		  loc);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_setxattr - 
 */
int32_t
stripe_setxattr (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc,
		 dict_t *dict,
		 int32_t flags)
{
  stripe_private_t *priv = this->private;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN);
    return 0;
  }

  STACK_WIND (frame,
	      stripe_common_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->setxattr,
	      loc,
	      dict,
	      flags);

  return 0;
}


/**
 * stripe_mknod - 
 */
int32_t
stripe_mknod (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      mode_t mode,
	      dev_t rdev)
{
  stripe_private_t *priv = this->private;
  
  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL, NULL);
    return 0;
  }

  STACK_WIND (frame,
	      stripe_common_inode_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->mknod,
	      loc,
	      mode,
	      rdev);

  return 0;
}


/**
 * stripe_mkdir - 
 */
int32_t
stripe_mkdir (call_frame_t *frame,
	      xlator_t *this,
	      loc_t *loc,
	      mode_t mode)
{
  stripe_private_t *priv = this->private;
  stripe_local_t *local = NULL;
  xlator_list_t *trav = NULL;
  
  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL, NULL);
    return 0;
  }

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  local->call_count = priv->child_count;
  frame->local = local;

  /* Everytime in stripe lookup, all child nodes should be looked up */
  trav = this->children;
  while (trav) {
    STACK_WIND (frame,
		stripe_stack_unwind_inode_cbk,
		trav->xlator,
		trav->xlator->fops->mkdir,
		loc,
		mode);
    trav = trav->next;
  }

  return 0;
}


/**
 * stripe_symlink - 
 */
int32_t
stripe_symlink (call_frame_t *frame,
		xlator_t *this,
		const char *linkpath,
		loc_t *loc)
{
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = NULL;
  
  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL, NULL);
    return 0;
  }

  /* send symlink to only first node */
  trav = this->children;
  STACK_WIND (frame,
	      stripe_common_inode_cbk,
	      trav->xlator,
	      trav->xlator->fops->symlink,
	      linkpath,
	      loc);

  return 0;
}

/**
 * stripe_link -
 */
int32_t
stripe_link (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     const char *newname)
{
  stripe_private_t *priv = this->private;
  stripe_local_t *local = NULL;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;
  
  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL, NULL);
    return 0;
  }

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,
		stripe_common_inode_cbk,
		trav->xlator,
		trav->xlator->fops->link,
		loc,
		newname);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->call_count = priv->child_count;

    /* Everytime in stripe lookup, all child nodes should be looked up */
    while (trav) {
      STACK_WIND (frame,
		  stripe_stack_unwind_inode_cbk,
		  trav->xlator,
		  trav->xlator->fops->link,
		  loc,
		  newname);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_create_setxattr_cbk - 
 */
STATIC int32_t
stripe_create_setxattr_cbk (call_frame_t *frame,
			    void *cookie,
			    xlator_t *this,
			    int32_t op_ret,
			    int32_t op_errno)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    
    if (op_ret == -1) {
      local->op_ret = -1;
      local->op_errno = op_errno;
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    freee (local->path);

    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->fd,
		  local->inode,
		  &local->stbuf);
  }
  return 0;
}

/**
 * stripe_create_cbk - 
 */
STATIC int32_t
stripe_create_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   fd_t *fd,
		   inode_t *inode,
		   struct stat *buf)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;
  stripe_private_t *priv = this->private;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    
    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
      } else {
	local->op_errno = op_errno;
      }
    }
    
    if (op_ret >= 0) {
      local->op_ret = op_ret;
      /* Get the mapping in inode private */
      /* Get the stat buf right */
      if (local->stbuf.st_blksize == 0) {
	local->stbuf = *buf;
      }
      
      if (strcmp (FIRST_CHILD(this)->name, ((xlator_t *)cookie)->name) == 0) {
	/* Always, pass the inode number of first child to the above layer */
	local->stbuf.st_ino = buf->st_ino;
      }
      
      if (local->stbuf.st_size < buf->st_size)
	local->stbuf.st_size = buf->st_size;
      local->stbuf.st_blocks += buf->st_blocks;
      if (local->stbuf.st_blksize != buf->st_blksize) {
	/* TODO: add to blocks in terms of original block size */
      }
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
      local->op_errno = EIO; /* TODO: Or should it be ENOENT? */
    }
    if (local->op_ret >= 0) {
      if (local->stripe_size) {
	dict_set (local->inode->ctx, 
		  this->name, 
		  data_from_int8 (2)); //stripped file
      } else {
	dict_set (local->inode->ctx, 
		  this->name, 
		  data_from_int8 (1)); //unstripped file
      }

      dict_set (local->fd->ctx, 
		this->name, 
		data_from_uint64 (local->stripe_size));
    }
    if (local->op_ret != -1 && local->stripe_size) {
      /* Send a setxattr request to nodes where the files are created */
      int32_t index = 0;
      char size_key[256] = {0,};
      char index_key[256] = {0,};
      char count_key[256] = {0,};
      xlator_list_t *trav = this->children;
      dict_t *dict = get_new_dict ();

      sprintf (size_key, "trusted.%s.stripe-size", this->name);
      sprintf (count_key, "trusted.%s.stripe-count", this->name);
      sprintf (index_key, "trusted.%s.stripe-index", this->name);

      dict_set (dict, size_key, data_from_int64 (local->stripe_size));
      dict_set (dict, count_key, data_from_int32 (local->call_count));

      if (local->stripe_size)
	local->call_count = priv->child_count;
      else 
	local->call_count = 1;
	
      while (trav) {
	loc_t tmp_loc = {
	  .inode = local->inode,
	  .path = local->path
	};
	dict_set (dict, index_key, data_from_int32 (index));

	STACK_WIND (frame,
		    stripe_create_setxattr_cbk,
		    trav->xlator,
		    trav->xlator->fops->setxattr,
		    &tmp_loc,
		    dict,
		    0);
	
	index++;
	trav = trav->next;
      }
      dict_destroy (dict);
    } else {
      /* Create itself has failed.. so return without setxattring */
      freee (local->path);
      
      STACK_UNWIND (frame, 
		    local->op_ret, 
		    local->op_errno, 
		    local->fd, 
		    local->inode, 
		    &local->stbuf);
    }
  }
  
  return 0;
}


/**
 * stripe_create - If a block-size is specified for the 'name', create the 
 *    file in all the child nodes. If not, create it in only first child.
 *
 * @name- complete path of the file to be created.
 */
int32_t
stripe_create (call_frame_t *frame,
	       xlator_t *this,
	       loc_t *loc,
	       int32_t flags,
	       mode_t mode,
	       fd_t *fd)
{
  stripe_private_t *priv = this->private;
  stripe_local_t *local = NULL;
  xlator_list_t *trav = NULL;
  off_t stripe_size = 0;

  stripe_size = stripe_get_matching_bs (loc->path, priv->pattern);
  
  if (priv->first_child_down || (stripe_size && priv->nodes_down)) {
    STACK_UNWIND (frame, -1, EIO, fd, loc->inode, NULL);
    return 0;
  }

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  local->stripe_size = stripe_size;
  local->path = strdup (loc->path);
  frame->local = local;
  local->fd = fd;
  local->inode = loc->inode;

  if (local->stripe_size) {
    /* Everytime in stripe lookup, all child nodes should be looked up */
    local->call_count = ((stripe_private_t *)this->private)->child_count;
    
    trav = this->children;
    while (trav) {
      _STACK_WIND (frame,
		   stripe_create_cbk,
		   trav->xlator,  /* cookie */
		   trav->xlator,
		   trav->xlator->fops->create,
		   loc,
		   flags,
		   mode,
		   fd);
      trav = trav->next;
    }
  } else {
    /* This path doesn't match any pattern, create the file only in first node */
    local->call_count = 1;
    _STACK_WIND (frame,
		 stripe_create_cbk,
		 FIRST_CHILD(this),
		 FIRST_CHILD(this),
		 FIRST_CHILD(this)->fops->create,
		 loc,
		 flags,
		 mode,
		 fd);
  }
       
  return 0;
}


/**
 * stripe_open_cbk - 
 */
STATIC int32_t
stripe_open_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 fd_t *fd)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
	local->op_errno = EIO; /* TODO: Or should it be ENOENT? */
      } else {
	local->op_ret = -1;
	local->op_errno = op_errno;
      }
    }
    
    if (op_ret >= 0) {
      local->op_ret = op_ret;
    }
  }
  UNLOCK (&frame->lock);
  
  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
    }
    if (local->op_ret >= 0) {
      dict_set (local->fd->ctx, 
		this->name, 
		data_from_uint64 (local->stripe_size));
    }
    freee (local->path);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, local->fd);
  }

  return 0;
}


/**
 * stripe_getxattr_cbk - 
 */
STATIC int32_t
stripe_open_getxattr_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *this,
			  int32_t op_ret,
			  int32_t op_errno,
			  dict_t *dict)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;
  xlator_list_t *trav = this->children;
  stripe_private_t *priv = this->private;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
	local->op_errno = EIO;
      } else {
	local->op_ret = -1;
	local->op_errno = op_errno;
      }
    }
  }
  UNLOCK (&frame->lock);
  
  if (!callcnt) {
    if (!local->failed && (local->op_ret != -1)) {
      /* If getxattr doesn't fails, call open */
      char size_key[256] = {0,};
      data_t *stripe_size_data = NULL;

      sprintf (size_key, "trusted.%s.stripe-size", this->name);
      stripe_size_data = dict_get (dict, size_key);

      if (stripe_size_data) {
	local->stripe_size = data_to_int64 (stripe_size_data);
      } else {
	/* if the file was created using earlier versions of stripe */
	local->stripe_size = stripe_get_matching_bs (local->path, 
						     ((stripe_private_t *)this->private)->pattern);
	if (local->stripe_size) {
	  gf_log (this->name, 
		  GF_LOG_WARNING,
		  "Seems like file(%s) created using earlier version",
		  local->path);
	} else {
	  gf_log (this->name,
		  GF_LOG_WARNING,
		  "no pattern found for file(%s), opening only in first node",
		  local->path);
	}
      }
    }
    
    local->call_count = priv->child_count;

    while (trav) {
      loc_t tmp_loc = {
	.path = local->path, 
	.inode = local->inode
      };
      _STACK_WIND (frame,
		   stripe_open_cbk,
		   trav->xlator->name,
		   trav->xlator,
		   trav->xlator->fops->open,
		   &tmp_loc,
		   local->flags,
		   local->fd);
      trav = trav->next;
    }
  }
  return 0;
}

/**
 * stripe_open - 
 */
int32_t
stripe_open (call_frame_t *frame,
	     xlator_t *this,
	     loc_t *loc,
	     int32_t flags,
	     fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->inode = loc->inode;
  local->fd = fd;
  frame->local = local;
  local->path = strdup (loc->path);

  hint = data_to_int8 (dict_get (loc->inode->ctx, this->name));

  if (local->hint == 1) {
    local->call_count = 1;

    /* File is present only in one node, no xattr's present */
    _STACK_WIND (frame,
		 stripe_open_cbk,
		 trav->xlator->name,
		 trav->xlator,
		 trav->xlator->fops->open,
		 loc,
		 flags,
		 fd);
  } else {
    /* Striped files */
    local->flags = flags;
    local->call_count = priv->child_count;
    while (trav) {
      STACK_WIND (frame,
		  stripe_open_getxattr_cbk,
		  trav->xlator,
		  trav->xlator->fops->getxattr,
		  loc);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_opendir_cbk - 
 */
STATIC int32_t
stripe_opendir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    fd_t *fd)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;

    if (op_ret == -1) {
      local->op_ret = -1;
      local->op_errno = op_errno;
    }
    
    if (op_ret >= 0) {
      local->op_ret = op_ret;
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    STACK_UNWIND (frame, local->op_ret, local->op_errno, local->fd);
  }

  return 0;
}


/**
 * stripe_opendir - 
 */
int32_t
stripe_opendir (call_frame_t *frame,
		xlator_t *this,
		loc_t *loc,
		fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, EIO, NULL);
    return 0;
  }

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  frame->local = local;
  local->inode = loc->inode;
  local->fd = fd;
  local->call_count = priv->child_count;

  while (trav) {
    _STACK_WIND (frame,
		 stripe_opendir_cbk,
		 trav->xlator->name, //cookie
		 trav->xlator,
		 trav->xlator->fops->opendir,
		 loc,
		 fd);
    trav = trav->next;
  }
  
  return 0;
}


/**
 * stripe_getxattr_cbk - 
 */
STATIC int32_t
stripe_getxattr_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     dict_t *value)
{
  STACK_UNWIND (frame, op_ret, op_errno, value);
  return 0;
}


/**
 * stripe_getxattr - 
 */
int32_t
stripe_getxattr (call_frame_t *frame,
		 xlator_t *this,
		 loc_t *loc)
{
  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  STACK_WIND (frame,
	      stripe_getxattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->getxattr,
	      loc);

  return 0;
}

/**
 * stripe_removexattr - 
 */
int32_t
stripe_removexattr (call_frame_t *frame,
		     xlator_t *this,
		     loc_t *loc,
		     const char *name)
{
  stripe_private_t *priv = this->private;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (loc);

  if (priv->first_child_down) {
    STACK_UNWIND (frame, -1, ENOTCONN, NULL);
    return 0;
  }

  STACK_WIND (frame,
	      stripe_common_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->removexattr,
	      loc,
	      name);

  return 0;
}


/**
 * stripe_lk_cbk - 
 */
STATIC int32_t
stripe_lk_cbk (call_frame_t *frame,
	       void *cookie,
	       xlator_t *this,
	       int32_t op_ret,
	       int32_t op_errno,
	       struct flock *lock)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    if (op_ret == -1) {
      if (op_errno == ENOTCONN) {
	local->failed = 1;
      } else {
	local->op_errno = op_errno;
      }
    }
    if (op_ret == 0 && local->op_ret == -1) {
      /* First successful call, copy the *lock */
      local->op_ret = 0;
      local->lock = *lock;
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    if (local->failed) {
      local->op_ret = -1;
      local->op_errno = EIO; /* TODO: Or should it be ENOENT? */
    }
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->lock);
  }
  return 0;
}


/**
 * stripe_lk - 
 */
int32_t
stripe_lk (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    int32_t cmd,
	    struct flock *lock)
{
  stripe_local_t *local = NULL;
  xlator_list_t *trav = this->children;
  stripe_private_t *priv = this->private;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);
  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;
  
  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    local->call_count = 1;
    STACK_WIND (frame,	      
		stripe_lk_cbk,
		trav->xlator,
		trav->xlator->fops->lk,
		fd,
		cmd,
		lock);
  } else {
    local->call_count = priv->child_count;
    
    while (trav) {
      STACK_WIND (frame,	      
		  stripe_lk_cbk,
		  trav->xlator,
		  trav->xlator->fops->lk,
		  fd,
		  cmd,
		  lock);
      trav = trav->next;
    }
  }

  return 0;
}

/**
 * stripe_writedir - 
 */
int32_t
stripe_writedir (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 int32_t flags,
		 dir_entry_t *entries,
		 int32_t count)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;
  local->call_count = priv->child_count;

  while (trav) {
    STACK_WIND (frame,	      
		stripe_stack_unwind_cbk,
		trav->xlator,
		trav->xlator->fops->writedir,
		fd,
		flags,
		entries,
		count);
    trav = trav->next;
  }

  return 0;
}


/**
 * stripe_flush - 
 */
int32_t
stripe_flush (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_cbk,
		trav->xlator,
		trav->xlator->fops->flush,
		fd);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->call_count = priv->child_count;
    
    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_cbk,
		  trav->xlator,
		  trav->xlator->fops->flush,
		  fd);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_close - 
 */
int32_t
stripe_close (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_cbk,
		trav->xlator,
		trav->xlator->fops->close,
		fd);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    local->fd = fd;
    frame->local = local;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_cbk,
		  trav->xlator,
		  trav->xlator->fops->close,
		  fd);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_fsync - 
 */
int32_t
stripe_fsync (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      int32_t flags)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_cbk,
		trav->xlator,
		trav->xlator->fops->fsync,
		fd,
		flags);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_cbk,
		  trav->xlator,
		  trav->xlator->fops->fsync,
		  fd,
		  flags);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_fstat - 
 */
int32_t
stripe_fstat (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->fstat,
		fd);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = fd->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->fstat,
		  fd);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_fchmod - 
 */
int32_t 
stripe_fchmod (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd,
	       mode_t mode)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->fchmod,
		fd,
		mode);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = fd->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->fchmod,
		  fd,
		  mode);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_fchown - 
 */
int32_t 
stripe_fchown (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd,
	       uid_t uid,
	       gid_t gid)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->fchown,
		fd,
		uid,
		gid);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = fd->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->fchown,
		  fd,
		  uid,
		  gid);
      trav = trav->next;
    }
  }

  return 0;
}


/**
 * stripe_ftruncate - 
 */
int32_t
stripe_ftruncate (call_frame_t *frame,
		  xlator_t *this,
		  fd_t *fd,
		  off_t offset)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;
  int8_t hint = 0;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  hint = data_to_int8 (dict_get (fd->inode->ctx, this->name));
  if (hint == 1) {
    STACK_WIND (frame,	      
		stripe_common_buf_cbk,
		trav->xlator,
		trav->xlator->fops->ftruncate,
		fd,
		offset);
  } else {
    /* Initialization */
    local = calloc (1, sizeof (stripe_local_t));
    local->op_ret = -1;
    frame->local = local;
    local->inode = fd->inode;
    local->call_count = priv->child_count;

    while (trav) {
      STACK_WIND (frame,	      
		  stripe_stack_unwind_buf_cbk,
		  trav->xlator,
		  trav->xlator->fops->ftruncate,
		  fd,
		  offset);
      trav = trav->next;
    }
  }
  return 0;
}


/**
 * stripe_closedir - 
 */
int32_t
stripe_closedir (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;
  local->call_count = priv->child_count;
  
  while (trav) {
    STACK_WIND (frame,	      
		stripe_stack_unwind_cbk,
		trav->xlator,
		trav->xlator->fops->closedir,
		fd);
    trav = trav->next;
  }

  return 0;
}


/**
 * stripe_fsyncdir - 
 */
int32_t
stripe_fsyncdir (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd,
		 int32_t flags)
{
  stripe_local_t *local = NULL;
  stripe_private_t *priv = this->private;
  xlator_list_t *trav = this->children;

  STRIPE_CHECK_INODE_CTX_AND_UNWIND_ON_ERR (fd);

  /* Initialization */
  local = calloc (1, sizeof (stripe_local_t));
  local->op_ret = -1;
  frame->local = local;
  local->call_count = priv->child_count;

  while (trav) {
    STACK_WIND (frame,	      
		stripe_stack_unwind_cbk,
		trav->xlator,
		trav->xlator->fops->fsyncdir,
		fd,
		flags);
    trav = trav->next;
  }

  return 0;
}


/**
 * stripe_single_readv_cbk - This function is used as return fn, when the 
 *     file name doesn't match the pattern specified for striping.
 */
STATIC int32_t
stripe_single_readv_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *this,
			 int32_t op_ret,
			 int32_t op_errno,
			 struct iovec *vector,
			 int32_t count,
			 struct stat *stbuf)
{
  STACK_UNWIND (frame, op_ret, op_errno, vector, count, stbuf);
  return 0;
}

/**
 * stripe_readv_cbk - get all the striped reads, and order it properly, send it
 *        to above layer after putting it in a single vector.
 */
STATIC int32_t
stripe_readv_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct iovec *vector,
		  int32_t count,
		  struct stat *stbuf)
{
  int32_t index = 0;
  int32_t callcnt = 0;
  call_frame_t *main_frame = NULL;
  stripe_local_t *main_local = NULL;
  stripe_local_t *local = frame->local;

  index = local->node_index;
  main_frame = local->orig_frame;
  main_local = main_frame->local;

  LOCK (&main_frame->lock);
  {
    main_local->replies[index].op_ret = op_ret;
    main_local->replies[index].op_errno = op_errno;
    if (op_ret > 0) {
      main_local->replies[index].count  = count;
      main_local->replies[index].vector = iov_dup (vector, count);
      main_local->replies[index].stbuf = *stbuf;
      dict_copy (frame->root->rsp_refs, main_frame->root->rsp_refs);
    }
    callcnt = ++main_local->call_count;
  }
  UNLOCK(&main_frame->lock);

  if (callcnt == main_local->wind_count) {
    int32_t final_count = 0;
    struct iovec *final_vec = NULL;
    struct stat tmp_stbuf = {0,};
    dict_t *refs = main_frame->root->rsp_refs;

    op_ret = 0;
    memcpy (&tmp_stbuf, &main_local->replies[0].stbuf, sizeof (struct stat));
    for (index=0; index < main_local->wind_count; index++) {
      /* TODO: check whether each stripe returned 'expected'
       * number of bytes 
       */
      if (main_local->replies[index].op_ret == -1) {
        op_ret = -1;
        op_errno = main_local->replies[index].op_errno;
        break;
      }
      op_ret += main_local->replies[index].op_ret;
      final_count += main_local->replies[index].count;
      /* TODO: Do I need to send anything more in stbuf? */
      if (tmp_stbuf.st_size < main_local->replies[index].stbuf.st_size)
	tmp_stbuf.st_size = main_local->replies[index].stbuf.st_size;
    }

    if (op_ret != -1) {
      final_vec = calloc (final_count, sizeof (struct iovec));
      final_count = 0;

      for (index=0; index < main_local->wind_count; index++) {
        memcpy (final_vec + final_count,
                main_local->replies[index].vector,
                main_local->replies[index].count * sizeof (struct iovec));
        final_count += main_local->replies[index].count;

        free (main_local->replies[index].vector);
      }
    } else {
      final_vec = NULL;
      final_count = 0;
    }

    refs = main_frame->root->rsp_refs;
    STACK_UNWIND (main_frame, op_ret, op_errno, final_vec, final_count, &tmp_stbuf);

    dict_unref (refs);
    if (final_vec)
      free (final_vec);
  }

  STACK_DESTROY (frame->root);
  return 0;
}

/**
 * stripe_readv - 
 */
int32_t
stripe_readv (call_frame_t *frame,
	      xlator_t *this,
	      fd_t *fd,
	      size_t size,
	      off_t offset)
{
  int32_t index = 0;
  int32_t num_stripe = 0;
  size_t frame_size = 0;
  off_t rounded_end = 0;
  off_t stripe_size = 0;
  off_t rounded_start = 0;
  off_t frame_offset = offset;
  stripe_local_t *local = NULL;
  call_frame_t *rframe = NULL;
  stripe_local_t *rlocal = NULL;
  xlator_list_t *trav = this->children;
  stripe_private_t *priv = this->private;

  if (!(fd && fd->ctx && dict_get (fd->ctx, this->name))) {
    STACK_UNWIND (frame, -1, EBADFD, NULL);
    return 0;
  }

  stripe_size = data_to_uint64 (dict_get (fd->ctx, this->name));
  if (stripe_size) {
    /* The file is stripe across the child nodes. Send the read request to the 
     * child nodes appropriately after checking which region of the file is in
     * which child node. Always '0-<stripe_size>' part of the file resides in
     * the first child.
     */
    rounded_start = floor (offset, stripe_size);
    rounded_end = roof (offset+size, stripe_size);
    num_stripe = (rounded_end - rounded_start) / stripe_size;

    local = calloc (1, sizeof (stripe_local_t));
    local->wind_count = num_stripe;
    frame->local = local;
    frame->root->rsp_refs = dict_ref (get_new_dict ());
    
    /* This is where all the vectors should be copied. */
    local->replies = calloc (1, num_stripe * sizeof (struct readv_replies));
    
    for (index = 0;
	 index < ((offset / stripe_size) % priv->child_count);
	 index++) {
      trav = trav->next;
    }
    
    for (index = 0; index < num_stripe; index++) {
      rframe = copy_frame (frame);
      rlocal = calloc (1, sizeof (stripe_local_t));

      frame_size = min (roof (frame_offset+1, stripe_size),
			(offset + size)) - frame_offset;
      
      rlocal->node_index = index;
      rlocal->orig_frame = frame;
      rframe->local = rlocal;
      STACK_WIND (rframe,
		  stripe_readv_cbk,
		  trav->xlator,
		  trav->xlator->fops->readv,
		  fd,
		  frame_size,
		  frame_offset);
      
      frame_offset += frame_size;

      trav = trav->next ? trav->next : this->children;
    }
  } else {
    /* If stripe size is 0, that means, there is no striping. */
    STACK_WIND (frame,
		stripe_single_readv_cbk,
		FIRST_CHILD(this),
		FIRST_CHILD(this)->fops->readv,
		fd,
		size,
		offset);
  }

  return 0;
}


/**
 * stripe_writev_cbk - 
 */
STATIC int32_t
stripe_writev_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;
  LOCK(&frame->lock);
  {
    callcnt = ++local->call_count;
    
    if (op_ret == -1) {
      local->op_errno = op_errno;
      local->op_ret = -1;
    }
    if (op_ret >= 0) {
      local->op_ret += op_ret;
      local->stbuf = *stbuf;
    }
  }
  UNLOCK (&frame->lock);

  if ((callcnt == local->wind_count) && local->unwind) {
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

/**
 * stripe_single_writev_cbk - 
 */
STATIC int32_t
stripe_single_writev_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *this,
			  int32_t op_ret,
			  int32_t op_errno,
			  struct stat *stbuf)
{
  STACK_UNWIND (frame, op_ret, op_errno, stbuf);
  return 0;
}
/**
 * stripe_writev - 
 */
int32_t
stripe_writev (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd,
	       struct iovec *vector,
	       int32_t count,
	       off_t offset)
{
  int32_t idx = 0;
  int32_t total_size = 0;
  int32_t offset_offset = 0;
  int32_t remaining_size = 0;
  int32_t tmp_count = count;
  off_t fill_size = 0;
  off_t stripe_size = 0;
  struct iovec *tmp_vec = vector;
  stripe_private_t *priv = this->private;
  stripe_local_t *local = NULL;
  xlator_list_t *trav = NULL;

  if (!(fd && fd->ctx && dict_get (fd->ctx, this->name))) {
    STACK_UNWIND (frame, -1, EBADFD, NULL);
    return 0;
  }

  stripe_size = data_to_uint64 (dict_get (fd->ctx, this->name));
  if (stripe_size) {
    /* File has to be stripped across the child nodes */
    for (idx = 0; idx< count; idx ++) {
      total_size += tmp_vec[idx].iov_len;
    }
    remaining_size = total_size;
    
    local = calloc (1, sizeof (stripe_local_t));
    frame->local = local;
    local->stripe_size = stripe_size;

    while (1) {
      /* Send striped chunk of the vector to child nodes appropriately. */
      trav = this->children;
      
      idx = ((offset + offset_offset) / local->stripe_size) % priv->child_count;
      while (idx) {
	trav = trav->next;
	idx--;
      }
      fill_size = local->stripe_size - 
	((offset + offset_offset) % local->stripe_size);
      if (fill_size > remaining_size)
	fill_size = remaining_size;
      remaining_size -= fill_size;

      tmp_count = iov_subset (vector, count, offset_offset,
			      offset_offset + fill_size, NULL);
      tmp_vec = calloc (tmp_count, sizeof (struct iovec));
      tmp_count = iov_subset (vector, count, offset_offset,
			      offset_offset + fill_size, tmp_vec);

      local->wind_count++;
      if (remaining_size == 0)
	local->unwind = 1;
      STACK_WIND(frame,
		 stripe_writev_cbk,
		 trav->xlator,
		 trav->xlator->fops->writev,
		 fd,
		 tmp_vec,
		 tmp_count,
		 offset + offset_offset);
      free (tmp_vec);
      offset_offset += fill_size;
      if (remaining_size == 0)
	break;
    }
  } else {
    /* File will goto only the first child */
    STACK_WIND (frame,
		stripe_single_writev_cbk,
		FIRST_CHILD(this),
		FIRST_CHILD(this)->fops->writev,
		fd,
		vector,
		count,
		offset);
  }
  return 0;
}

static int32_t
stripe_chmod (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      mode_t mode)
{
  stripe_local_t *local = (stripe_local_t *) calloc (1, sizeof (stripe_local_t));
  xlator_list_t *trav = xl->children;
  frame->local = local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  LOCK_INIT (&frame->mutex);
  while (trav) {
    STACK_WIND (frame,
		stripe_chmod_cbk,
		trav->xlator,
		trav->xlator->fops->chmod,
		path,
		mode);
    trav = trav->next;
  }
  return 0;
}

/**
 * stripe_stats_cbk - Add all the fields received from different clients. 
 *    Once all the clients return, send stats to above layer.
 * 
 */
STATIC int32_t
stripe_stats_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct xlator_stats *stats)
{
  int32_t callcnt = 0;
  stripe_local_t *local = frame->local;

  LOCK(&frame->lock);
  {
    callcnt = --local->call_count;
    
    if (op_ret == -1) {
      local->op_ret = -1;
      local->op_errno = op_errno;
    }
    if (op_ret == 0) {
      if (local->op_ret == -2) {
	/* This is to make sure this is the frist time */
	local->stats = *stats;
	local->op_ret = 0;
      } else {
	local->stats.nr_files += stats->nr_files;
	local->stats.free_disk += stats->free_disk;
	local->stats.disk_usage += stats->disk_usage;
	local->stats.nr_clients += stats->nr_clients;
      }
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stats);
  }

  return 0;
}

/**
 * stripe_stats - 
 */
int32_t
stripe_stats (call_frame_t *frame,
	      xlator_t *this,
	      int32_t flags)
{
  stripe_local_t *local = NULL;
  xlator_list_t *trav = this->children;

  local = calloc (1, sizeof (stripe_local_t));
  frame->local = local;
  local->op_ret = -2; /* to be used as a flag in _cbk */
  local->call_count = ((stripe_private_t*)this->private)->child_count;
  while (trav) {
    STACK_WIND (frame,
                stripe_stats_cbk,
                trav->xlator,
                trav->xlator->mops->stats,
                flags);
    trav = trav->next;
  }
  return 0;
}


/**
 * notify
 */
int32_t
notify (xlator_t *this,
        int32_t event,
        void *data,
        ...)
{
  stripe_private_t *priv = this->private;

  switch (event) 
    {
    case GF_EVENT_CHILD_UP:
      {
	LOCK (&priv->lock);
	{
	  --priv->nodes_down;
	  if (data == FIRST_CHILD (this)) {
	    priv->first_child_down = 0;
	    default_notify (this, event, data);
	  }
	}
	UNLOCK (&priv->lock);
      }
      break;
    case GF_EVENT_CHILD_DOWN:
      {
	LOCK (&priv->lock);
	{
	  ++priv->nodes_down;
	  if (data == FIRST_CHILD (this)) {
	    priv->first_child_down = 1;
	    default_notify (this, event, data);
	  }
	}
	UNLOCK (&priv->lock);
      }
      break;
    default:
      {
	/* */
	default_notify (this, event, data);
      }
      break;
    }

  return 0;
}
/**
 * init - This function is called when xlator-graph gets initialized. 
 *     The option given in spec files are parsed here.
 * @this - 
 */
int32_t
init (xlator_t *this)
{
  stripe_private_t *priv = NULL;
  xlator_list_t *trav = NULL;
  data_t *stripe_data = NULL;
  int32_t count = 0;
  
  trav = this->children;
  while (trav) {
    count++;
    trav = trav->next;
  }

  if (!count) {
    gf_log (this->name, 
	    GF_LOG_ERROR,
	    "stripe configured without \"subvolumes\" option. exiting");
    return -1;
  }
  priv = calloc (1, sizeof (stripe_private_t));
  priv->child_count = count;
  LOCK_INIT (&priv->lock);

  /* option stripe-pattern *avi:1GB,*pdf:4096 */
  stripe_data = dict_get (this->options, "block-size");
  if (!stripe_data) {
    gf_log (this->name, 
	    GF_LOG_WARNING,
	    "No stripe pattern specified. check \"option block-size <x>\" in spec file");
  } else {
    char *tmp_str = NULL;
    char *tmp_str1 = NULL;
    char *dup_str = NULL;
    char *stripe_str = NULL;
    char *pattern = NULL;
    char *num = NULL;
    struct stripe_options *temp_stripeopt = NULL;
    struct stripe_options *stripe_opt = NULL;    
    /* Get the pattern for striping. "option block-size *avi:10MB" etc */
    stripe_str = strtok_r (stripe_data->data, ",", &tmp_str);
    while (stripe_str) {
      dup_str = strdup (stripe_str);
      stripe_opt = calloc (1, sizeof (struct stripe_options));
      pattern = strtok_r (dup_str, ":", &tmp_str1);
      num = strtok_r (NULL, ":", &tmp_str1);
      memcpy (stripe_opt->path_pattern, pattern, strlen (pattern));
      if (num) 
	stripe_opt->block_size = gf_str_to_long_long (num);
      else {
	stripe_opt->block_size = gf_str_to_long_long ("128KB");
      }
      gf_log (this->name, 
	      GF_LOG_DEBUG, 
	      "stripe block size : pattern %s : size %d", 
	      stripe_opt->path_pattern, 
	      stripe_opt->block_size);
      if (!priv->pattern) {
	priv->pattern = stripe_opt;
      } else {
	temp_stripeopt = priv->pattern;
	while (temp_stripeopt->next)
	  temp_stripeopt = temp_stripeopt->next;
	temp_stripeopt->next = stripe_opt;
      }
      stripe_str = strtok_r (NULL, ",", &tmp_str);
    }
  }

  /* notify related */
  priv->nodes_down = priv->child_count;
  priv->first_child_down = 1;
  this->private = priv;
  
  return 0;
} 

/** 
 * fini -   Free all the private variables
 * @this - 
 */
void 
fini (xlator_t *this)
{
  stripe_private_t *priv = this->private;
  struct stripe_options *prev = NULL;
  struct stripe_options *trav = priv->pattern;
  while (trav) {
    prev = trav;
    trav = trav->next;
    free (prev);
  }
  LOCK_DESTROY (&priv->lock);
  free (priv);
  return;
}


struct xlator_fops fops = {
  .stat        = stripe_stat,
  .unlink      = stripe_unlink,
  .symlink     = stripe_symlink,
  .rename      = stripe_rename,
  .link        = stripe_link,
  .chmod       = stripe_chmod,
  .chown       = stripe_chown,
  .truncate    = stripe_truncate,
  .utimens     = stripe_utimens,
  .create      = stripe_create,
  .open        = stripe_open,
  .readv       = stripe_readv,
  .writev      = stripe_writev,
  .statfs      = stripe_statfs,
  .flush       = stripe_flush,
  .close       = stripe_close,
  .fsync       = stripe_fsync,
  .setxattr    = stripe_setxattr,
  .getxattr    = stripe_getxattr,
  .removexattr = stripe_removexattr,
  .access      = stripe_access,
  .ftruncate   = stripe_ftruncate,
  .fstat       = stripe_fstat,
  .readlink    = stripe_readlink,
  .mkdir       = stripe_mkdir,
  .rmdir       = stripe_rmdir,
  .lk          = stripe_lk,
  .closedir    = stripe_closedir,
  .opendir     = stripe_opendir,
  .fsyncdir    = stripe_fsyncdir,
  .fchmod      = stripe_fchmod,
  .fchown      = stripe_fchown,
  .lookup      = stripe_lookup,
  .forget      = stripe_forget,
  .writedir    = stripe_writedir,
  .mknod       = stripe_mknod,
};

struct xlator_mops mops = {
  .stats  = stripe_stats,
  //.lock   = stripe_lock,
  //.unlock = stripe_unlock,
};
