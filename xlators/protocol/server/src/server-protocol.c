/*
  (C) 2006, 2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
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

#include "transport.h"
#include "fnmatch.h"
#include "xlator.h"
#include "protocol.h"
#include "lock.h"
#include "server-protocol.h"
#include <time.h>
#include <sys/uio.h>
#include "call-stub.h"
#include "defaults.h"
#include "list.h"

#if __WORDSIZE == 64
# define F_L64 "%l"
#else
# define F_L64 "%ll"
#endif

#define STATE(frame) ((server_state_t *)frame->root->state)
#define TRANSPORT_OF(frame) ((transport_t *) STATE(frame)->trans)
#define SERVER_PRIV(frame) ((server_proto_priv_t *) TRANSPORT_OF(frame)->xl_private)
#define BOUND_XL(frame) ((xlator_t *) SERVER_PRIV(frame)->bound_xl)

/*
 * str_to_ptr - convert a string to pointer
 * @string: string
 *
 */
static int32_t
server_inode_prune (xlator_t *bound_xl);

static void *
str_to_ptr (char *string)
{
  return (void *)strtoul (string, NULL, 16);
}

/*
 * ptr_to_str - convert a pointer to string
 * @ptr: pointer
 *
 */
static char *
ptr_to_str (void *ptr)
{
  char *str;
  asprintf (&str, "%p", ptr);
  return str;
}

/* 
 * stat_to_str - convert struct stat to a ASCII string
 * @stbuf: struct stat pointer
 *
 * not for external reference
 */
static char *
stat_to_str (struct stat *stbuf)
{
  char *tmp_buf = NULL;

  uint64_t dev = stbuf->st_dev;
  uint64_t ino = stbuf->st_ino;
  uint32_t mode = stbuf->st_mode;
  uint32_t nlink = stbuf->st_nlink;
  uint32_t uid = stbuf->st_uid;
  uint32_t gid = stbuf->st_gid;
  uint64_t rdev = stbuf->st_rdev;
  uint64_t size = stbuf->st_size;
  uint32_t blksize = stbuf->st_blksize;
  uint64_t blocks = stbuf->st_blocks;
  uint32_t atime = stbuf->st_atime;
  uint32_t atime_nsec = stbuf->st_atim.tv_nsec;
  uint32_t mtime = stbuf->st_mtime;
  uint32_t mtime_nsec = stbuf->st_mtim.tv_nsec;
  uint32_t ctime = stbuf->st_ctime;
  uint32_t ctime_nsec = stbuf->st_ctim.tv_nsec;

  asprintf (&tmp_buf,
	    GF_STAT_PRINT_FMT_STR,
	    dev,
	    ino,
	    mode,
	    nlink,
	    uid,
	    gid,
	    rdev,
	    size,
	    blksize,
	    blocks,
	    atime,
	    atime_nsec,
	    mtime,
	    mtime_nsec,
	    ctime,
	    ctime_nsec);
  
  return tmp_buf;
}

/*
 * generic_reply - generic reply, used to send reply packet to client
 * @frame: call frame
 * @type: reply type GF_MOP_REPLY/GF_FOP_REPLY
 * @op: operation to which this reply corresponds to
 * @params: parameter dictionary, actual data of the reply packet
 *
 * not for external reference
 */
static int32_t
generic_reply (call_frame_t *frame,
	       int32_t type,
	       glusterfs_fop_t op,
	       dict_t *params)
{
  gf_block_t *blk;
  transport_t *trans;
  int32_t count, i, ret;
  struct iovec *vector;

  trans = TRANSPORT_OF (frame);
  
  blk = gf_block_new (frame->root->unique);
  blk->data = NULL;
  blk->size = 0;
  blk->type = type;
  blk->op   = op;
  blk->dict = params;

  count = gf_block_iovec_len (blk);
  vector = alloca (count * sizeof (*vector));
  memset (vector, 0, count * sizeof (*vector));

  gf_block_to_iovec (blk, vector, count);
  for (i=0; i<count; i++)
    if (!vector[i].iov_base)
      vector[i].iov_base = alloca (vector[i].iov_len);
  gf_block_to_iovec (blk, vector, count);

  free (blk);

  ret = trans->ops->writev (trans, vector, count);
  if (ret != 0) {
    gf_log ("protocol/server", 
	    GF_LOG_ERROR,
	    "transport_writev failed");
    transport_except (trans);
  }

  return 0;
}

server_reply_t *
server_reply_dequeue (server_reply_queue_t *queue)
{
  server_reply_t *entry = NULL;

  pthread_mutex_lock (&queue->lock);

  while (list_empty (&queue->list))
    pthread_cond_wait (&queue->cond, &queue->lock);

  entry = list_entry (queue->list.next, server_reply_t, list);
  list_del_init (&entry->list);

  pthread_mutex_unlock (&queue->lock);

  return entry;
}


static void
server_reply_queue (server_reply_t *entry,
		    server_reply_queue_t *queue)
{
  pthread_mutex_lock (&queue->lock);
  list_add_tail (&entry->list, &queue->list);
  pthread_cond_broadcast (&queue->cond);
  pthread_mutex_unlock (&queue->lock);
}


static void *
server_reply_proc (void *data)
{
  server_reply_queue_t *queue = data;

  while (1) {
    server_reply_t *entry = NULL;
    server_state_t *state = NULL;

    entry = server_reply_dequeue (queue);

    generic_reply (entry->frame, entry->type, entry->op, entry->reply);

    server_inode_prune (BOUND_XL (entry->frame));

    state = STATE (entry->frame);
    {
      if (entry->refs)
	dict_unref (entry->refs);
      dict_destroy (entry->reply);
      STACK_DESTROY (entry->frame->root);
      free (entry);
    }
    {
      transport_unref (state->trans);
      if (state->inode)
	inode_unref (state->inode);
      if (state->inode2)
	inode_unref (state->inode2);
      free (state);
    }
  }

  return NULL;
}

static void
server_reply (call_frame_t *frame,
	      int32_t type,
	      glusterfs_fop_t op,
	      dict_t *reply,
	      dict_t *refs)
{
  server_reply_t *entry = NULL;
  transport_t *trans = frame->this->private;;

  entry = calloc (1, sizeof (*entry));
  entry->frame = frame;
  entry->type = type;
  entry->op = op;
  entry->reply = reply;
  if (refs)
    entry->refs = dict_ref (refs);

  server_reply_queue (entry, trans->xl_private);
}

/*
 * server_fchmod_cbk
 */
static int32_t
server_fchmod_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;

  dict_set (reply, "RET", data_from_uint64 (op_ret));
  dict_set (reply, "ERRNO", data_from_uint64 (op_errno));
  

  stat_str = stat_to_str (stbuf);
  dict_set (reply, "STAT", data_from_dynstr (stat_str));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FCHMOD, reply,
		frame->root->rsp_refs);

  return 0;
}

/*
 * server_fchmod
 *
 */
static int32_t
server_fchmod (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{  
  data_t *fd_data = dict_get (params, "FD");
  data_t *mode_data = dict_get (params, "MODE");
  char *fd_str = NULL;
  fd_t *fd = NULL;
  mode_t mode = 0;

  if (!fd_data || !mode_data) {
    struct stat stbuf = {0,};
    server_fchmod_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       &stbuf);
    return 0;
  }

  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  mode = data_to_uint64 (mode_data);

  STACK_WIND (frame, 
	      server_fchmod_cbk, 
	      bound_xl,
	      bound_xl->fops->fchmod,
	      fd,
	      mode);
  
  return 0;

}


/*
 * server_fchown_cbk 
 */
static int32_t
server_fchown_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;
  dict_set (reply, "RET", data_from_uint64 (op_ret));
  dict_set (reply, "ERRNO", data_from_uint64 (op_errno));

  if (op_ret >= 0) {
    stat_str = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_str));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FCHOWN,
		reply, frame->root->rsp_refs);
  return 0;

}

/*
 * server_fchown
 *
 */
static int32_t
server_fchown (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *uid_data = dict_get (params, "UID");
  data_t *gid_data = dict_get (params, "GID");
  uid_t uid = 0;
  gid_t gid = 0;
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !uid_data || !gid_data) {
    struct stat stbuf = {0,};
    server_fchown_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       &stbuf);
    return 0;
  }

  uid = data_to_uint64 (uid_data);
  gid = data_to_uint64 (gid_data);
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  STACK_WIND (frame, 
	      server_fchown_cbk, 
	      bound_xl,
	      bound_xl->fops->fchown,
	      fd,
	      uid,
	      gid);
  
  return 0;

}

/*
 * server_writedir_cbk - writedir callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_writedir_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_WRITEDIR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_lk_cbk - lk callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @lock:
 *
 * not for external reference
 */
static int32_t
server_lk_cbk (call_frame_t *frame,
	       void *cookie,
	       xlator_t *this,
	       int32_t op_ret,
	       int32_t op_errno,
	       struct flock *lock)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {
    dict_set (reply, "TYPE", data_from_int16 (lock->l_type));
    dict_set (reply, "WHENCE", data_from_int16 (lock->l_whence));
    dict_set (reply, "START", data_from_int64 (lock->l_start));
    dict_set (reply, "LEN", data_from_int64 (lock->l_len));
    dict_set (reply, "PID", data_from_uint64 (lock->l_pid));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_LK,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_access_cbk - access callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_access_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_ACCESS,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_utimens_cbk - utimens callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_utimens_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_UTIMENS,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_chmod_cbk - chmod callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_chmod_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_CHMOD,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_chown_cbk - chown callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_chown_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_CHOWN,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_rmdir_cbk - rmdir callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_rmdir_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_RMDIR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_inode_prune - procedure to prune inode. this procedure is called
 *                      from all fop_cbks where we get a valid inode. 
 * @bound_xl: translator this transport is bound to
 *
 * not for external reference
 */
static int32_t
server_inode_prune (xlator_t *bound_xl)
{
  struct list_head inode_list;
  inode_t *inode_curr = NULL, *inode_next = NULL;

  if (!bound_xl)
    return 0;

  INIT_LIST_HEAD (&inode_list);
  
  inode_table_prune (bound_xl->itable, &inode_list);
  
  if (!list_empty (&inode_list)) {
    list_for_each_entry_safe (inode_curr, inode_next, &inode_list, list) {
      
      gf_log (bound_xl->name, GF_LOG_DEBUG,
	      "pruning inode = %p & ino = %d. lru=%d/%d", 
	      inode_curr, inode_curr->buf.st_ino, bound_xl->itable->lru_size,
	      bound_xl->itable->lru_limit);
      
      inode_curr->ref++; /* manual ref++, to avoid moving inode_curr to active list. :( */
      list_del_init (&inode_curr->list);
      inode_forget (inode_curr, 0);
      inode_unref (inode_curr);	
    }
  }

  return 0;
}

/*
 * server_mkdir_cbk - mkdir callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_mkdir_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  inode_t *inode,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *statbuf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    inode_lookup (inode);
    statbuf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (statbuf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_MKDIR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_mknod_cbk - mknod callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_mknod_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  inode_t *inode,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    inode_lookup (inode);
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
    dict_set (reply, "INODE", data_from_uint64 (inode->ino));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_MKNOD,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_fsyncdir_cbk - fsyncdir callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_fsyncdir_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FSYNCDIR,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_readdir_cbk - readdir callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 * @entries:
 * @count:
 *
 * not for external reference
 */
static int32_t
server_readdir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    dir_entry_t *entries,
		    int32_t count)
{
  dict_t *reply = get_new_dict ();
  char *buffer = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {

    dict_set (reply, "NR_ENTRIES", data_from_int32 (count));
    
    {   
      dir_entry_t *trav = entries->next;
      uint32_t len = 0;
      char *tmp_buf = NULL;
      while (trav) {
	len += strlen (trav->name);
	len += 1;
	len += 256; // max possible for statbuf;
	trav = trav->next;
      }
      
      buffer = calloc (1, len);
      char *ptr = buffer;
      trav = entries->next;
      while (trav) {
	int this_len;
	tmp_buf = stat_to_str (&trav->buf);
	this_len = sprintf (ptr, "%s/%s", 
			    trav->name,
			    tmp_buf);
	
	free (tmp_buf);
	trav = trav->next;
	ptr += this_len;
      }
      dict_set (reply, "DENTRIES", data_from_dynstr (buffer));
    }
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_READDIR,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_closedir_cbk - closedir callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_closedir_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_CLOSEDIR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_opendir_cbk - opendir callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 * @fd: file descriptor structure of opened directory
 *
 * not for external reference
 */
static int32_t
server_opendir_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    fd_t *fd)
{
  dict_t *reply = get_new_dict ();
  char *fd_str = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    server_proto_priv_t *priv = SERVER_PRIV (frame);
    char ctx_buf[32] = {0,};
    
    fd_str = ptr_to_str (fd);
    dict_set (reply, "FD", data_from_dynstr (fd_str));
    
    sprintf (ctx_buf, "%p", fd);
    pthread_mutex_lock (&priv->lock);
    dict_set (priv->open_dirs, ctx_buf, str_to_data (""));
    pthread_mutex_unlock (&priv->lock);
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_OPENDIR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_statfs_cbk - statfs callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 * @buf:
 *
 * not for external reference
 */
static int32_t
server_statfs_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct statvfs *buf)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret == 0) {
    char buffer[256] = {0,};
    
    uint32_t bsize = buf->f_bsize;
    uint32_t frsize = buf->f_frsize;
    uint64_t blocks = buf->f_blocks;
    uint64_t bfree = buf->f_bfree;
    uint64_t bavail = buf->f_bavail;
    uint64_t files = buf->f_files;
    uint64_t ffree = buf->f_ffree;
    uint64_t favail = buf->f_favail;
    uint32_t fsid = buf->f_fsid;
    uint32_t flag = buf->f_flag;
    uint32_t namemax = buf->f_namemax;
    
    sprintf (buffer, GF_STATFS_PRINT_FMT_STR,
	     bsize,
	     frsize,
	     blocks,
	     bfree,
	     bavail,
	     files,
	     ffree,
	     favail,
	     fsid,
	     flag,
	     namemax);
    
    dict_set (reply, "BUF", data_from_dynstr (strdup (buffer)));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_STATFS,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_removexattr_cbk - removexattr callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_removexattr_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *this,
			int32_t op_ret,
			int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_REMOVEXATTR,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_getxattr_cbk - getxattr callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 * @value:
 *
 * not for external reference
 */
static int32_t
server_getxattr_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     dict_t *dict)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  {
    /* Serialize the dictionary and set it as a parameter in 'reply' dict */
    int32_t len = 0;
    char *dict_buf = NULL;

    dict_set (dict, "key", str_to_data ("value"));
    len = dict_serialized_length (dict);
    dict_buf = calloc (len, 1);
    dict_serialize (dict, dict_buf);
    dict_set (reply, "DICT", data_from_dynptr (dict_buf, len));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_GETXATTR,
		reply, frame->root->rsp_refs);

  return 0;
}
/*
 * server_setxattr_cbk - setxattr callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_setxattr_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_SETXATTR,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_rename_cbk - rename callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_rename_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  if (op_ret >= 0) {
    stat_str = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_str));
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_RENAME,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_unlink_cbk - unlink callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_unlink_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_UNLINK,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_symlink_cbk - symlink callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_symlink_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    inode_t *inode,
		    struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    inode_lookup (inode);
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_SYMLINK,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_link_cbk - link callback for server protocol
 * @frame: call frame
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_link_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 inode_t *inode,
		 struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    inode_lookup (inode);
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
    dict_set (reply, "INODE", data_from_uint64 (inode->ino));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_LINK,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_truncate_cbk - truncate callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_truncate_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_TRUNCATE,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_fstat_cbk - fstat callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_fstat_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FSTAT,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_ftruncate_cbk - ftruncate callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_ftruncate_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FTRUNCATE,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_flush_cbk - flush callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_flush_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FLUSH,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_fsync_cbk - fsync callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_fsync_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FSYNC,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_close_cbk - close callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_close_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_CLOSE,
		reply, frame->root->rsp_refs);

  return 0;
}


/* 
 * server_writev_cbk - writev callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_writev_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    stat_str = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_str)); 
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_WRITE,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_readv_cbk - readv callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @vector:
 * @count:
 *
 * not for external reference
 */
static int32_t
server_readv_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct iovec *vector,
		  int32_t count,
		  struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    dict_set (reply, "BUF", data_from_iovec (vector, count));
    stat_str = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_str));
  }
  else
    dict_set (reply, "BUF", str_to_data (""));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_READ,
		reply, frame->root->rsp_refs);

  return 0;
}


/* 
 * server_open_cbk - open callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @fd:
 *
 * not for external reference
 */ 
static int32_t
server_open_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 fd_t *fd)
{
  dict_t *reply = get_new_dict ();
  char *fd_str = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    server_proto_priv_t *priv = NULL;
    char ctx_buf[32] = {0,};

    fd_str = ptr_to_str (fd);
    priv = SERVER_PRIV (frame);
    dict_set (reply, "FD", data_from_dynstr (fd_str));
  
    sprintf (ctx_buf, "%p", fd);
    pthread_mutex_lock (&priv->lock);
    dict_set (priv->open_files, ctx_buf, str_to_data (""));
    pthread_mutex_unlock (&priv->lock);
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_OPEN,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_create_cbk - create callback for server
 * @frame: call frame
 * @cookie:
 * @this:  translator structure
 * @op_ret: 
 * @op_errno:
 * @fd: file descriptor
 * @inode: inode structure
 * @stbuf: struct stat of created file
 *
 * not for external reference
 */
static int32_t
server_create_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   fd_t *fd,
		   inode_t *inode,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  if (op_ret >= 0) {
    char ctx_buf[32] = {0,};
    server_proto_priv_t *priv = NULL;
    char *fd_str = ptr_to_str (fd);

    inode_lookup (inode);

    dict_set (reply, "FD", data_from_dynstr (fd_str));
  
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));

    priv = SERVER_PRIV (frame);
    sprintf (ctx_buf, "%p", fd);
    pthread_mutex_lock (&priv->lock);
    dict_set (priv->open_files, ctx_buf, str_to_data (""));
    pthread_mutex_unlock (&priv->lock);
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_CREATE,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_readlink_cbk - readlink callback for server protocol
 * @frame: call frame
 * @cookie: 
 * @this:
 * @op_ret:
 * @op_errno:
 * @buf:
 *
 * not for external reference
 */
static int32_t
server_readlink_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     const char *buf)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  dict_set (reply, "LINK", data_from_dynstr (buf ? strdup (buf) : strdup ("") ));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_READLINK,
		reply, frame->root->rsp_refs);

  return 0;
}

/* 
 * server_stat_cbk - stat callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @stbuf:
 *
 * not for external reference
 */

static int32_t
server_stat_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_buf = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret >= 0) {
    stat_buf = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_buf));
  }

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_STAT,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_forget_cbk - forget callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 *
 * not for external reference
 */
static int32_t
server_forget_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_FORGET,
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * server_lookup_cbk - lookup callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @inode:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_lookup_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   inode_t *inode,
		   struct stat *stbuf)
{
  dict_t *reply = get_new_dict ();
  char *stat_str = NULL;

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (op_ret == 0) {
    inode_lookup (inode);
    stat_str = stat_to_str (stbuf);
    dict_set (reply, "STAT", data_from_dynstr (stat_str));
  }
  
  server_reply (frame, GF_OP_TYPE_FOP_REPLY, GF_FOP_LOOKUP,
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * server_stub_cbk - this is callback function used whenever an fop does
 *                   STACK_WIND to fops->lookup in order to lookup the inode
 *                   for a pathname. this case of doing fops->lookup arises
 *                   when fop searches in inode table for pathname and search
 *                   fails.
 *
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret:
 * @op_errno:
 * @inode:
 * @stbuf:
 *
 * not for external reference
 */
static int32_t
server_stub_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 inode_t *inode,
		 struct stat *stbuf)
{
  /* TODO: should inode pruning be done here or not??? */
  if (inode)
    inode_lookup (inode);

  if (frame->local) {
    /* we have a call stub to wind to */
    call_stub_t *stub = (call_stub_t *)frame->local;
    
    if (stub->fop != GF_FOP_RENAME)
      /* to make sure that STACK_DESTROY() does not try to free 
       * frame->local. frame->local points to call_stub_t, which is
       * free()ed in call_resume(). */
      frame->local = NULL;
#if 0   
    /* classic bug.. :O, intentionally left for sweet memory.
     *                                              --benki
     */
    if (op_ret < 0) {
      if (stub->fop != GF_FOP_RENAME) {
	/* TODO: STACK_UNWIND helps prevent memory leak. how?? */
	STACK_UNWIND (stub->frame, -1, ENOENT, 0, 0);
	free (stub);
	return 0;
      }
    }
#endif

    switch (stub->fop)
      {
      case GF_FOP_RENAME:
	if (!stub->args.rename.old.inode) {
	  loc_t *newloc = NULL;
	  /* now we are called by lookup of oldpath. */
	  if (op_ret < 0) {
	    /* to make sure that STACK_DESTROY() does not try to free 
	     * frame->local. frame->local points to call_stub_t, which is
	     * free()ed in call_resume(). */
	    frame->local = NULL;
	    
	    /* lookup of oldpath failed, UNWIND to server_rename_cbk with
	     * ret=-1 and errno=ENOENT */
	    server_rename_cbk (stub->frame,
			       NULL,
			       stub->frame->this,
			       -1,
			       ENOENT,
			       NULL);
	    
	    free ((char *)stub->args.rename.old.path);
	    free ((char *)stub->args.rename.new.path);
	    free (stub);
	    return 0;
	  }
	  
	  /* store inode information of oldpath in our stub and search for 
	   * newpath in inode table. 
	   * inode_ref()ed because, we might do a STACK_WIND to fops->lookup()
	   * again to lookup for newpath */
	  stub->args.rename.old.inode = inode_ref (inode);
	  stub->args.rename.old.ino = stbuf->st_ino;
	  
	  /* now lookup for newpath */
	  newloc = &stub->args.rename.new;
	  newloc->inode = inode_search (BOUND_XL(frame)->itable, 
					newloc->ino,
					NULL);
	  
	  if (!newloc->inode) {
	    /* lookup for newpath */
	    STACK_WIND (stub->frame,
			server_stub_cbk,
			BOUND_XL (stub->frame),
			BOUND_XL (stub->frame)->fops->lookup,
			newloc);
	    
	    break;
	  } else {
	    /* found newpath in inode cache */
	    
	    /* to make sure that STACK_DESTROY() does not try to free 
	     * frame->local. frame->local points to call_stub_t, which is
	     * free()ed in call_resume(). */
	    frame->local = NULL;
	    call_resume (stub);
	    break;
	  }
	} else {
	  /* we are called by the lookup of newpath */
	  
	  /* to make sure that STACK_DESTROY() does not try to free 
	   * frame->local. frame->local points to call_stub_t, which is
	   * free()ed in call_resume(). */
	  frame->local = NULL;
	  
	  if (inode) {	  
	    stub->args.rename.new.inode = inode_ref (inode);
	    stub->args.rename.new.ino = stbuf->st_ino;
	  }
	}      
	
	/* after looking up for oldpath as well as newpath, 
	 * we are ready to resume */
	{
	  call_resume (stub);
	}
	break;
      case GF_FOP_OPEN:
	{
	  if (op_ret < 0) {
	    server_open_cbk (stub->frame,
			     NULL,
			     stub->frame->this,
			     -1,
			     ENOENT,
			     NULL);
	    free ((char *)stub->args.open.loc.path);
	    free (stub);
	    return 0;
	  }
    	  stub->args.open.loc.inode = inode_ref (inode);
	  stub->args.open.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
      case GF_FOP_STAT:
	{
	  if (op_ret < 0) {
	    server_stat_cbk (stub->frame,
			     NULL,
			     stub->frame->this,
			     -1,
			     ENOENT,
			     NULL);
	    free ((char *)stub->args.stat.loc.path);
	    free (stub);
	    return 0;
	  }

	  /* TODO: reply from here only, we already have stat structure */
	  stub->args.stat.loc.inode = inode_ref (inode);
	  stub->args.stat.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_UNLINK:
	{
	  if (op_ret < 0) {
	    server_unlink_cbk (stub->frame,
			       NULL,
			       stub->frame->this,
			       -1,
			       ENOENT);
	    free ((char *)stub->args.unlink.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.unlink.loc.inode = inode_ref (inode);
	  stub->args.unlink.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_RMDIR:
	{
	  if (op_ret < 0) {
	    server_rmdir_cbk (stub->frame,
			       NULL,
			       stub->frame->this,
			       -1,
			       ENOENT);
	    free ((char *)stub->args.rmdir.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.rmdir.loc.inode = inode_ref (inode);
	  stub->args.rmdir.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_CHMOD:
	{
	  if (op_ret < 0) {
	    server_chmod_cbk (stub->frame,
			      NULL,
			      stub->frame->this,
			      -1,
			      ENOENT,
			      NULL);
	    free ((char *)stub->args.chmod.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.chmod.loc.inode = inode_ref (inode);
	  stub->args.chmod.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
      case GF_FOP_CHOWN:
	{
	  if (op_ret < 0) {
	    server_chown_cbk (stub->frame,
			      NULL,
			      stub->frame->this,
			      -1,
			      ENOENT,
			      NULL);
	    free ((char *)stub->args.chown.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.chown.loc.inode = inode_ref (inode);
	  stub->args.chown.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}

      case GF_FOP_LINK:
	{
	  if (op_ret < 0) {
	    server_link_cbk (stub->frame,
			     NULL,
			     stub->frame->this,
			     -1,
			     ENOENT,
			     NULL,
			     NULL);
	    free ((char *)stub->args.link.oldloc.path);
	    free ((char *)stub->args.link.newpath);
	    free (stub);
	    return 0;
	  }

	  stub->args.link.oldloc.inode = inode_ref (inode);
	  stub->args.link.oldloc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}

      case GF_FOP_TRUNCATE:
	{
	  if (op_ret < 0) {
	    server_truncate_cbk (stub->frame,
				 NULL,
				 stub->frame->this,
				 -1,
				 ENOENT,
				 NULL);
	    free ((char *)stub->args.truncate.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.truncate.loc.inode = inode_ref (inode);
	  stub->args.truncate.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_STATFS:
	{
	  if (op_ret < 0) {
	    server_statfs_cbk (stub->frame,
			       NULL,
			       stub->frame->this,
			       -1,
			       ENOENT,
			       NULL);
	    free ((char *)stub->args.statfs.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.statfs.loc.inode = inode_ref (inode);
	  stub->args.statfs.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_SETXATTR:
	{
	  dict_t *dict = stub->args.setxattr.dict;
	  if (op_ret < 0) {
	    server_setxattr_cbk (stub->frame,
				 NULL,
				 stub->frame->this,
				 -1,
				 ENOENT);
	    free ((char *)stub->args.setxattr.loc.path);
	    dict_destroy (dict);
	    free (stub);
	    return 0;
	  }

	  stub->args.setxattr.loc.inode = inode_ref (inode);
	  stub->args.setxattr.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  //	  dict_destroy (dict);
	  break;
	}
	
      case GF_FOP_GETXATTR:
	{
	  if (op_ret < 0) {
	    server_getxattr_cbk (stub->frame,
				 NULL,
				 stub->frame->this,
				 -1,
				 ENOENT,
				 NULL);
	    free ((char *)stub->args.getxattr.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.getxattr.loc.inode = inode_ref (inode);
	  stub->args.getxattr.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_REMOVEXATTR:
	{
	  if (op_ret < 0) {
	    server_removexattr_cbk (stub->frame,
				    NULL,
				    stub->frame->this,
				    -1,
				    ENOENT);
	    free ((char *)stub->args.removexattr.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.removexattr.loc.inode = inode_ref (inode);
	  stub->args.removexattr.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_OPENDIR:
	{
	  if (op_ret < 0) {
	    server_opendir_cbk (stub->frame,
				NULL,
				stub->frame->this,
				-1,
				ENOENT,
				NULL);
	    free ((char *)stub->args.opendir.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.opendir.loc.inode = inode_ref (inode);
	  stub->args.opendir.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      case GF_FOP_ACCESS:
	{
	  if (op_ret < 0) {
	    server_access_cbk (stub->frame,
			       NULL,
			       stub->frame->this,
			       -1,
			       ENOENT);
	    free ((char *)stub->args.access.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.access.loc.inode = inode_ref (inode);
	  stub->args.access.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
	
      case GF_FOP_UTIMENS:
	{	  
	  if (op_ret < 0) {
	    server_utimens_cbk (stub->frame,
				NULL,
				stub->frame->this,
				-1,
				ENOENT,
				NULL);
	    free ((char *)stub->args.utimens.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.utimens.loc.inode = inode_ref (inode);
	  stub->args.utimens.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}

      case GF_FOP_READLINK:
	{	  
	  if (op_ret < 0) {
	    server_readlink_cbk (stub->frame,
				 NULL,
				 stub->frame->this,
				 -1,
				 ENOENT,
				 NULL);
	    free ((char *)stub->args.readlink.loc.path);
	    free (stub);
	    return 0;
	  }

	  stub->args.utimens.loc.inode = inode_ref (inode);
	  stub->args.utimens.loc.ino = stbuf->st_ino;
	  call_resume (stub);
	  break;
	}
	
      default:
	call_resume (stub);
      }
  } 
  return 0;
}




/*
 * server_lookup - lookup function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_lookup (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};
  server_state_t *state = STATE (frame);
  
  if (!path_data || !inode_data) {
    server_lookup_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       NULL,
		       NULL);
    return 0;
  }
		       
  loc.ino  = data_to_uint64 (inode_data);
  loc.path = strdup (data_to_str (path_data));
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  if (loc.inode)
    state->inode = inode_ref (loc.inode);

  STACK_WIND (frame,
	      server_lookup_cbk,
	      bound_xl,
	      bound_xl->fops->lookup,
	      &loc);

  if (loc.inode)
    inode_unref (loc.inode);

  free ((char *)loc.path);
	      
  return 0;
}


/*
 * server_forget - forget function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_forget (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *inode_data = dict_get (params, "INODE");
  inode_t *inode = NULL;

  if (!inode_data) {
    server_forget_cbk (frame,
		       NULL,
		       bound_xl,
		       -1,
		       EINVAL);
    return 0;
  }

  inode = inode_search (bound_xl->itable, 
			data_to_uint64 (inode_data),
			NULL);

  if (inode) {
    inode_forget (inode, 0);
    inode_unref (inode);
  }

  server_forget_cbk (frame, NULL, bound_xl, 0, 0);
  
  return 0;
}



static int32_t
server_stat_resume (call_frame_t *frame,
		    xlator_t *this,
		    loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_stat_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->stat,
	      loc);
  return 0;
}

/*
 * server_stat - stat function for server
 * @frame: call frame
 * @bound_xl: translator this server is bound to
 * @params: parameters dictionary
 *
 * not for external reference
 */

static int32_t
server_stat (call_frame_t *frame,
	     xlator_t *bound_xl,
	     dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  struct stat buf = {0, };
  loc_t loc = {0,};

  if (!path_data || !inode_data) {
    server_stat_cbk (frame,
		     NULL,
		     frame->this,
		     -1,
		     EINVAL,
		     &buf);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);
  call_stub_t *stat_stub = fop_stat_stub (frame, 
					  server_stat_resume,
					  &loc);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = stat_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (stat_stub);
  }
  return 0;
}


static int32_t
server_readlink_resume (call_frame_t *frame,
			xlator_t *this,
			loc_t *loc,
			size_t size)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_readlink_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->readlink,
	      loc,
	      size);
  return 0;
}

/*
 * server_readlink - readlink function for server
 * @frame: call frame
 * @bound_xl: translator this server is bound to
 * @params: parameters dictionary
 *
 * not for external reference
 */

static int32_t
server_readlink (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *len_data = dict_get (params, "LEN");
  int32_t len = data_to_int32 (len_data);
  loc_t loc = {0,};

  if (!path_data || !len_data) {
    server_readlink_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL,
			 "");
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *readlink_stub = fop_readlink_stub (frame, 
						  server_readlink_resume,
						  &loc,
						  len);
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = readlink_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (readlink_stub);
  }
  return 0;
}



/*
 * server_create - create function for server
 * @frame: call frame
 * @bound_xl: translator this server is bound to
 * @params: parameters dictionary
 *
 * not for external reference
 */
static int32_t
server_create (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  int32_t flags = 0;
  data_t *path_data = dict_get (params, "PATH");
  data_t *mode_data = dict_get (params, "MODE");
  data_t *flag_data = dict_get (params, "FLAGS");

  if (!path_data || !mode_data) {
    struct stat buf = {0, };
    server_create_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       NULL,
		       NULL,
		       &buf);
    return 0;
  }

  if (flag_data) {
    flags = data_to_int32 (flag_data);
  }

  STACK_WIND (frame, 
	      server_create_cbk, 
	      bound_xl,
	      bound_xl->fops->create,
	      data_to_str (path_data),
	      flags,
	      data_to_int64 (mode_data));
  
  return 0;
}


static int32_t
server_open_resume (call_frame_t *frame,
		    xlator_t *this,
		    loc_t *loc,
		    int32_t flags)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_open_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->open,
	      loc,
	      flags);

  return 0;	      
}

/*
 * server_open - open function for server protocol
 * @frame: call frame
 * @bound_xl: translator this server protocol is bound to
 * @params: parameters dictionary
 *
 * not for external reference
 */
static int32_t
server_open (call_frame_t *frame,
	     xlator_t *bound_xl,
	     dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *flag_data = dict_get (params, "FLAGS");
  int32_t flags = data_to_int32 (flag_data);
  loc_t loc = {0,};
  char *path = NULL;
  
  if (!path_data || !inode_data || !flag_data) {
    server_open_cbk (frame,
		     NULL,
		     frame->this,
		     -1,
		     EINVAL,
		     NULL);
    return 0;
  }

  path = data_to_str (path_data);
  loc.path = path;
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *open_stub = fop_open_stub (frame, 
					  server_open_resume,
					  &loc,
					  flags);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = open_stub;
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);
    
  } else {
    /* we are fine with everything, go ahead with open of our child */
    call_resume (open_stub);
  }
  return 0;
}


/*
 * server_readv - readv function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_readv (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *len_data = dict_get (params, "LEN");
  data_t *off_data = dict_get (params, "OFFSET");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !len_data || !off_data) {
    struct iovec vec;
    struct stat stbuf = {0,};
    vec.iov_base = "";
    vec.iov_len = 0;
    server_readv_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      &vec,
		      0,
		      &stbuf);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  
  STACK_WIND (frame, 
	      server_readv_cbk,
	      bound_xl,
	      bound_xl->fops->readv,
	      fd,
	      data_to_int32 (len_data),
	      data_to_int64 (off_data));
  
  return 0;
}


/*
 * server_writev - writev function for server
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference 
 */
static int32_t
server_writev (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *len_data = dict_get (params, "LEN");
  data_t *off_data = dict_get (params, "OFFSET");
  data_t *buf_data = dict_get (params, "BUF");
  struct iovec iov;
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !len_data || !off_data || !buf_data) {
    struct stat stbuf = {0,};
    server_writev_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       &stbuf);
    return 0;
  }

  iov.iov_base = buf_data->data;
  iov.iov_len = data_to_int32 (len_data);
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  
  STACK_WIND (frame, 
	      server_writev_cbk, 
	      bound_xl,
	      bound_xl->fops->writev,
	      fd,
	      &iov,
	      1,
	      data_to_int64 (off_data));
  
  return 0;
}


/*
 * server_close - close function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference 
 */
static int32_t
server_close (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data) {
    server_close_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  
  {
    char str[32];
    server_proto_priv_t *priv = SERVER_PRIV (frame);
    sprintf (str, "%p", fd);
    pthread_mutex_lock (&priv->lock);
    dict_del (priv->open_files, str);
    pthread_mutex_unlock (&priv->lock);
  }

  STACK_WIND (frame, 
	      server_close_cbk, 
	      bound_xl,
	      bound_xl->fops->close,
	      fd);

  return 0;
}


/*
 * server_fsync - fsync function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameters dictionary
 *
 * not for external reference
 */
static int32_t
server_fsync (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *flag_data = dict_get (params, "FLAGS");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !flag_data) {
    server_fsync_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  STACK_WIND (frame, 
	      server_fsync_cbk, 
	      bound_xl,
	      bound_xl->fops->fsync,
	      fd,
	      data_to_int64 (flag_data));

  return 0;
}


/* 
 * server_flush - flush function for server protocol
 * @frame: call frame
 * @bound_xl: 
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_flush (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data) {
    server_flush_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL);
    return 0;
  }

  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  STACK_WIND (frame, 
	      server_flush_cbk, 
	      bound_xl,
	      bound_xl->fops->flush,
	      fd);
  
  return 0;
}


/* 
 * server_ftruncate - ftruncate function for server protocol
 * @frame: call frame
 * @bound_xl: 
 * @params: parameters dictionary
 *
 * not for external reference
 */
static int32_t
server_ftruncate (call_frame_t *frame,
		  xlator_t *bound_xl,
		  dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *off_data = dict_get (params, "OFFSET");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !off_data) {
    struct stat buf = {0, };
    server_ftruncate_cbk (frame,
			  NULL,
			  frame->this,
			  -1,
			  EINVAL,
			  &buf);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);

  STACK_WIND (frame, 
	      server_ftruncate_cbk, 
	      bound_xl,
	      bound_xl->fops->ftruncate,
	      fd,
	      data_to_int64 (off_data));

  return 0;
}


/*
 * server_fstat - fstat function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_fstat (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  char *fd_str = NULL;
  fd_t *fd = NULL; 

  if (!fd_data) {
    struct stat buf = {0, };
    server_fstat_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      &buf);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  STACK_WIND (frame, 
	      server_fstat_cbk, 
	      bound_xl,
	      bound_xl->fops->fstat,
	      fd);
  
  return 0;
}


static int32_t
server_truncate_resume (call_frame_t *frame,
			xlator_t *this,
			loc_t *loc,
			off_t offset)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_truncate_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->truncate,
	      loc,
	      offset);
  return 0;
}


/*
 * server_truncate - truncate function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 * not for external reference
 */
static int32_t
server_truncate (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *off_data = dict_get (params, "OFFSET");
  off_t offset = data_to_uint64 (off_data);
  loc_t loc = {0,};

  if (!path_data || !off_data || !inode_data) {
    struct stat buf = {0, };
    server_truncate_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL,
			 &buf);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *truncate_stub = fop_truncate_stub (frame, 
						  server_truncate_resume,
						  &loc,
						  offset);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = truncate_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (truncate_stub);
  }


  return 0;
}



static int32_t
server_link_resume (call_frame_t *frame,
		    xlator_t *this,
		    loc_t *oldloc,
		    const char *newpath)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (oldloc->inode);

  STACK_WIND (frame,
	      server_link_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->link,
	      oldloc,
	      newpath);
  return 0;
}

/* 
 * server_link - link function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 * not for external reference
 */
static int32_t
server_link (call_frame_t *frame,
	     xlator_t *bound_xl,
	     dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *buf_data = dict_get (params, "LINK");
  loc_t oldloc = {0,};
  char *newpath = NULL;

  if (!path_data || !buf_data) {
    struct stat buf = {0, };
    server_link_cbk (frame,
		     NULL,
		     frame->this,
		     -1,
		     EINVAL,
		     NULL,
		     &buf);
    return 0;
  }
  
  oldloc.path = data_to_str (path_data);
  oldloc.inode = inode_search (bound_xl->itable, 
			       data_to_uint64 (inode_data), 
			       NULL);

  newpath = data_to_str (buf_data);  

  call_stub_t *link_stub = fop_link_stub (frame, 
					  server_link_resume,
					  &oldloc,
					  newpath);
  if (oldloc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (oldloc.inode);
  }
  
  if (!oldloc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = link_stub;
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&oldloc);
    
  } else {
    call_resume (link_stub);
  }
  return 0;
}


/* 
 * server_symlink- symlink function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */

static int32_t
server_symlink (call_frame_t *frame,
		xlator_t *bound_xl,
		dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *buf_data = dict_get (params, "SYMLINK");
  char *path = NULL;
  char *link = NULL;

  if (!path_data || !buf_data) {
    struct stat buf = {0, };
    server_symlink_cbk (frame,
			NULL,
			frame->this,
			-1,
			EINVAL,
			NULL,
			&buf);
    return 0;
  }
  
  path = strdup (data_to_str (path_data));
  link = strdup (data_to_str (buf_data));
  
  STACK_WIND (frame, 
	      server_symlink_cbk, 
	      bound_xl,
	      bound_xl->fops->symlink,
	      path,
	      link);

  free (path);
  free (link);

  return 0;
}


static int32_t
server_unlink_resume (call_frame_t *frame,
		      xlator_t *this,
		      loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_unlink_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->unlink,
	      loc);
  return 0;
}

/* 
 * server_unlink - unlink function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */

static int32_t
server_unlink (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};
  if (!path_data || !inode_data) {
    server_unlink_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *unlink_stub = fop_unlink_stub (frame, 
					      server_unlink_resume,
					      &loc);
  
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = unlink_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (unlink_stub);
  }
  return 0;
}



static int32_t
server_rename_resume (call_frame_t *frame,
		      xlator_t *this,
		      loc_t *oldloc,
		      loc_t *newloc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (oldloc->inode);
  if (newloc->inode)
    state->inode2 = inode_ref (newloc->inode);

  STACK_WIND (frame,
	      server_rename_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->rename,
	      oldloc,
	      newloc);
  return 0;
}

/* 
 * server_rename - rename function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */

static int32_t
server_rename (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  char *path = NULL; 
  data_t *inode_data = dict_get (params, "INODE");
  data_t *newpath_data = dict_get (params, "NEWPATH");
  data_t *newinode_data = dict_get (params, "NEWINODE");
  char *newpath = NULL; 
  loc_t oldloc = {0,};
  loc_t newloc = {0,};
  call_stub_t *rename_stub = NULL;

  if (!path_data || !newpath_data || !inode_data || !newinode_data) {
    server_rename_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       NULL);
    return 0;
  }

  path = data_to_str (path_data);
  newpath = data_to_str (newpath_data);

  oldloc.path = path;
  newloc.path = newpath;

  oldloc.ino = data_to_uint64 (inode_data);
  oldloc.inode = inode_search (bound_xl->itable, oldloc.ino, NULL);

  newloc.ino = data_to_uint64 (newinode_data);
  newloc.inode = inode_search (bound_xl->itable, newloc.ino, NULL);

  /* :O
     frame->this = bound_xl;
  */
  rename_stub = fop_rename_stub (frame,
				 server_rename_resume,
				 &oldloc,
				 &newloc);
  if (oldloc.inode)
    inode_unref (oldloc.inode);
  
  if (newloc.inode)
    inode_unref (newloc.inode);

  frame->local = rename_stub;
  
  if (!oldloc.inode){
    /*    search of oldpath in inode cache _failed_.
     *    we need to do a lookup for oldpath. we do a fops->lookup() for 
     * oldpath. call-back being server_stub_cbk(). server_stub_cbk() takes
     * care of searching/lookup of newpath, if it already exists. 
     * server_stub_cbk() resumes to fops->rename(), after trying to lookup 
     * for newpath also.
     *    if lookup of oldpath fails, server_stub_cbk() UNWINDs to 
     * server_rename_cbk() with ret=-1 and errno=ENOENT.
     */
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&oldloc);
  } else if (!newloc.inode){
    /* inode for oldpath found in inode cache and search for newpath in inode
     * cache_failed_.
     * we need to lookup for newpath, with call-back being server_stub_cbk().
     * since we already have found oldpath in inode cache, server_stub_cbk()
     * continues with fops->rename(), irrespective of success or failure of
     * lookup for newpath.
     */
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&newloc);
  } else {
    /* we have found inode for both oldpath and newpath in inode cache.
     * we are continue with fops->rename() */

    frame->local = NULL;

    call_resume (rename_stub);
  }

  return 0;
}


static int32_t
server_setxattr_resume (call_frame_t *frame,
			xlator_t *this,
			loc_t *loc,
			dict_t *dict,
			int32_t flags)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_setxattr_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->setxattr,
	      loc,
	      dict,
	      flags);
  return 0;
}

/* 
 * server_setxattr - setxattr function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */

static int32_t
server_setxattr (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *flag_data = dict_get (params, "FLAGS");
  data_t *dict_data = dict_get (params, "DICT");
  int32_t flags = 0; 
  loc_t loc = {0,};
  dict_t *dict = NULL;

  if (!path_data || !inode_data || !flag_data || !dict_data) {
    server_setxattr_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL);
    return 0;
  }

  flags = data_to_int32 (flag_data);
  {
    /* Unserialize the dictionary */
    char *buf = memdup (dict_data->data, dict_data->len);
    dict = get_new_dict ();
    dict_unserialize (buf, dict_data->len, &dict);
    dict->extra_free = buf;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *setxattr_stub = fop_setxattr_stub (frame, 
						  server_setxattr_resume,
						  &loc,
						  dict,
						  flags);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = setxattr_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (setxattr_stub);
  }
  return 0;
}



static int32_t
server_getxattr_resume (call_frame_t *frame,
			xlator_t *this,
			loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_getxattr_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->getxattr,
	      loc);
  return 0;
}

/* 
 * server_getxattr - getxattr function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */

static int32_t
server_getxattr (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};

  if (!path_data || !inode_data) {
    server_getxattr_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL,
			 NULL);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);
  call_stub_t *getxattr_stub = fop_getxattr_stub (frame, 
						  server_getxattr_resume,
						  &loc);
  
  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = getxattr_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (getxattr_stub);
  }
  return 0;
}



static int32_t
server_removexattr_resume (call_frame_t *frame,
			   xlator_t *this,
			   loc_t *loc,
			   const char *name)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_removexattr_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->removexattr,
	      loc,
	      name);
  return 0;
}

/* 
 * server_removexattr - removexattr function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_removexattr (call_frame_t *frame,
		    xlator_t *bound_xl,
		    dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *name_data = dict_get (params, "NAME");
  char *name = NULL;
  loc_t loc = {0,};

  if (!path_data || !name_data) {
    server_removexattr_cbk (frame,
			    NULL,
			    frame->this,
			    -1,
			    EINVAL);
    return 0;
  }

  name = data_to_str (name_data);
  
  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *removexattr_stub = fop_removexattr_stub (frame, 
							server_removexattr_resume,
							&loc,
							name);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = removexattr_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (removexattr_stub);
  }
  return 0;
}


static int32_t
server_statfs_resume (call_frame_t *frame,
		      xlator_t *this,
		      loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_statfs_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->statfs,
	      loc);
  return 0;
}

/* 
 * server_statfs - statfs function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_statfs (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};

  if (!path_data || !inode_data) {
    struct statvfs buf = {0,};
    server_statfs_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL,
		       &buf);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *statfs_stub = fop_statfs_stub (frame, 
					      server_statfs_resume,
					      &loc);
  
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_statfs_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = statfs_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (statfs_stub);
  }
  return 0;
}



static int32_t
server_opendir_resume (call_frame_t *frame,
		       xlator_t *this,
		       loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_opendir_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->opendir,
	      loc);
  return 0;
}


/* 
 * server_opendir - opendir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_opendir (call_frame_t *frame,
		xlator_t *bound_xl,
		dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};

  if (!path_data || !inode_data) {
    server_opendir_cbk (frame,
			NULL,
			frame->this,
			-1,
			EINVAL,
			NULL);
    return 0;
  }

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);
  
  call_stub_t *opendir_stub = fop_opendir_stub (frame, 
						server_opendir_resume,
						&loc);
  
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = opendir_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);
  } else {
    call_resume (opendir_stub);
  }

  return 0;
}


/* 
 * server_closedir - closedir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_closedir (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data) {
    server_closedir_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL);
    return 0;
  }

  fd_str = data_to_str (fd_data);  
  fd = str_to_ptr (fd_str);

  {
    char str[32] = {0,};
    server_proto_priv_t *priv = NULL;

    priv = SERVER_PRIV (frame);
    sprintf (str, "%p", fd);
    pthread_mutex_lock (&priv->lock);
    dict_del (priv->open_dirs, str);
    pthread_mutex_unlock (&priv->lock);
  }

  STACK_WIND (frame, 
	      server_closedir_cbk, 
	      bound_xl,
	      bound_xl->fops->closedir,
	      fd);
  
  return 0;
}


/* 
 * server_readdir - readdir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_readdir (call_frame_t *frame,
		xlator_t *bound_xl,
		dict_t *params)
{
  data_t *size_data = dict_get (params, "SIZE");;
  data_t *offset_data = dict_get (params, "OFFSET");
  data_t *fd_data = dict_get (params, "FD");
  char *fd_str = NULL; 
  fd_t *fd = NULL; 

  if (!fd_data || !offset_data || !size_data) {
    dir_entry_t tmp = {0,};
    server_readdir_cbk (frame,
			NULL,
			frame->this,
			-1,
			EINVAL,
			&tmp,
			0);
    return 0;
  }
  
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);

  STACK_WIND (frame, 
	      server_readdir_cbk, 
	      bound_xl,
	      bound_xl->fops->readdir,
	      data_to_uint64 (size_data),
	      data_to_uint64 (offset_data),
	      fd);
  
  return 0;
}


/*
 * server_fsyncdir - fsyncdir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 * 
 * not for external reference
 */
static int32_t
server_fsyncdir (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *flag_data = dict_get (params, "FLAGS");
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data || !flag_data) {
    server_fsyncdir_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL);
    return 0;
  }
  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);

  STACK_WIND (frame, 
	      server_fsyncdir_cbk, 
	      bound_xl,
	      bound_xl->fops->fsyncdir,
	      fd,
	      data_to_int64 (flag_data));

  return 0;
}


/*
 * server_mknod - mknod function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_mknod (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *mode_data = dict_get (params, "MODE");
  data_t *dev_data = dict_get (params, "DEV");

  if (!path_data || !mode_data || !dev_data) {
    struct stat buf = {0, };
    server_mknod_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      NULL,
		      &buf);
    return 0;
  }

  STACK_WIND (frame, 
	      server_mknod_cbk, 
	      bound_xl,
	      bound_xl->fops->mknod,
	      data_to_str (path_data),
	      data_to_int64 (mode_data),
	      data_to_int64 (dev_data));

  return 0;
}


/*
 * server_mkdir - mkdir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 * not for external reference
 */
static int32_t
server_mkdir (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *mode_data = dict_get (params, "MODE");

  if (!path_data || !mode_data) {
    server_mkdir_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      NULL,
		      NULL);
    return 0;
  }
  
  STACK_WIND (frame, 
	      server_mkdir_cbk, 
	      bound_xl,
	      bound_xl->fops->mkdir,
	      data_to_str (path_data),
	      data_to_int64 (mode_data));
  
  return 0;
}


static int32_t
server_rmdir_resume (call_frame_t *frame,
		     xlator_t *this,
		     loc_t *loc)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_rmdir_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->rmdir,
	      loc);
  return 0;
}

/* 
 * server_rmdir - rmdir function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 * not for external reference
 */
static int32_t
server_rmdir (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  loc_t loc = {0,};

  if (!path_data || !inode_data) {
    server_rmdir_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL);
    return 0;
  }
  

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *rmdir_stub = fop_rmdir_stub (frame, 
					    server_rmdir_resume,
					    &loc);
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = rmdir_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);
  } else {
    call_resume (rmdir_stub);
   }
  return 0;
}


static int32_t
server_chown_resume (call_frame_t *frame,
		     xlator_t *this,
		     loc_t *loc,
		     uid_t uid,
		     gid_t gid)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame, server_chown_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->chown,
	      loc, uid, gid);
  return 0;
}


/*
 * server_chown - chown function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_chown (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *uid_data = dict_get (params, "UID");
  uid_t uid = 0;
  data_t *gid_data = dict_get (params, "GID");
  gid_t gid = 0;
  loc_t loc = {0,};

  if (!path_data || !inode_data || !uid_data || !gid_data) {
    struct stat buf = {0, };
    server_chown_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      &buf);
    return 0;
  }
  
  uid = data_to_uint64 (uid_data);
  gid = data_to_uint64 (gid_data);

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *chown_stub = fop_chown_stub (frame, 
					    server_chown_resume,
					    &loc,
					    uid,
					    gid);
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }


  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = chown_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (chown_stub);
  }
  return 0;
}


static int32_t 
server_chmod_resume (call_frame_t *frame,
		     xlator_t *this,
		     loc_t *loc,
		     mode_t mode)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_chmod_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->chmod,
	      loc,
	      mode);
  return 0;

}

/*
 * server_chmod - chmod function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_chmod (call_frame_t *frame,
	      xlator_t *bound_xl,
	      dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *mode_data = dict_get (params, "MODE");
  mode_t mode = 0; 
  loc_t loc = {0,};

  if (!path_data || !inode_data || !mode_data) {
    struct stat buf = {0, };
    server_chmod_cbk (frame,
		      NULL,
		      frame->this,
		      -1,
		      EINVAL,
		      &buf);
    return 0;
  }
  
  mode = data_to_uint64 (mode_data);

  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);
  call_stub_t *chmod_stub = fop_chmod_stub (frame, 
					    server_chmod_resume,
					    &loc,
					    mode);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = chmod_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (chmod_stub);
  }
  
  return 0;
}


static int32_t 
server_utimens_resume (call_frame_t *frame,
		       xlator_t *this,
		       loc_t *loc,
		       struct timespec *tv)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_utimens_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->utimens,
	      loc,
	      tv);
  return 0;
}

/*
 * server_utimens - utimens function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_utimens (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *atime_sec_data = dict_get (params, "ACTIME_SEC");
  data_t *mtime_sec_data = dict_get (params, "MODTIME_SEC");
  data_t *atime_nsec_data = dict_get (params, "ACTIME_NSEC");
  data_t *mtime_nsec_data = dict_get (params, "MODTIME_NSEC");
  struct timespec buf[2] = {{0,}, {0,}};
  loc_t loc = {0,};

  if (!path_data || 
      !inode_data || 
      !atime_sec_data || 
      !mtime_sec_data ||
      !atime_nsec_data ||
      !mtime_nsec_data) {
    struct stat buf = {0, };
    server_utimens_cbk (frame,
			NULL,
			frame->this,
			-1,
			EINVAL,
			&buf);
    return 0;
  }


  buf[0].tv_sec  = data_to_int64 (atime_sec_data);
  buf[0].tv_nsec = data_to_int64 (atime_nsec_data);
  buf[1].tv_sec  = data_to_int64 (mtime_sec_data);
  buf[1].tv_nsec = data_to_int64 (mtime_nsec_data);


  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *utimens_stub = fop_utimens_stub (frame, 
						server_utimens_resume,
						&loc,
						buf);

  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = utimens_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);
  } else {
    call_resume (utimens_stub);
  }
  return 0;
}


static int32_t
server_access_resume (call_frame_t *frame,
		      xlator_t *this,
		      loc_t *loc,
		      int32_t mask)
{
  server_state_t *state = STATE (frame);

  state->inode = inode_ref (loc->inode);

  STACK_WIND (frame,
	      server_access_cbk,
	      BOUND_XL (frame),
	      BOUND_XL (frame)->fops->access,
	      loc,
	      mask);
  return 0;
}
/*
 * server_access - access function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_access (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  data_t *inode_data = dict_get (params, "INODE");
  data_t *mode_data = dict_get (params, "MODE");
  mode_t mode = 0; 
  loc_t loc = {0,};

  if (!path_data || !inode_data || !mode_data) {
    server_access_cbk (frame,
		       NULL,
		       frame->this,
		       -1,
		       EINVAL);
    return 0;
  }

  mode = data_to_uint64 (mode_data);
  loc.path = data_to_str (path_data);
  loc.ino = data_to_uint64 (inode_data);
  loc.inode = inode_search (bound_xl->itable, loc.ino, NULL);

  call_stub_t *access_stub = fop_access_stub (frame, 
					      server_access_resume,
					      &loc,
					      mode);
  if (loc.inode) {
    /* unref()ing ref() from inode_search(), since fop_open_stub has kept
     * a reference for inode */
    inode_unref (loc.inode);
  }

  if (!loc.inode) {
    /* make a call stub and call lookup to get the inode structure.
     * resume call after lookup is successful */
    frame->local = access_stub;
    
    STACK_WIND (frame,
		server_stub_cbk,
		bound_xl,
		bound_xl->fops->lookup,
		&loc);

  } else {
    call_resume (access_stub);
  }

  return 0;
}


/* 
 * server_lk - lk function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 * not for external reference
 */
static int32_t
server_lk (call_frame_t *frame,
	   xlator_t *bound_xl,
	   dict_t *params)
{
  data_t *fd_data = dict_get (params, "FD");
  data_t *cmd_data = dict_get (params, "CMD");
  data_t *type_data = dict_get (params, "TYPE");
  data_t *whence_data = dict_get (params, "WHENCE");
  data_t *start_data = dict_get (params, "START");
  data_t *len_data = dict_get (params, "LEN");
  data_t *pid_data = dict_get (params, "PID");
  struct flock lock = {0, };
  int32_t cmd = 0;
  char *fd_str = NULL;
  fd_t *fd = NULL;

  if (!fd_data ||
      !cmd_data ||
      !type_data ||
      !whence_data ||
      !start_data ||
      !len_data ||
      !pid_data) {

    server_lk_cbk (frame,
		   NULL,
		   frame->this,
		   -1,
		   EINVAL,
		   &lock);
    return 0;
  }
  
  cmd =  data_to_int32 (cmd_data);
  lock.l_type =  data_to_int16 (type_data);
  lock.l_whence =  data_to_int16 (whence_data);
  lock.l_start =  data_to_int64 (start_data);
  lock.l_len =  data_to_int64 (len_data);
  lock.l_pid =  data_to_uint32 (pid_data);

  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);

  STACK_WIND (frame, 
	      server_lk_cbk, 
	      bound_xl,
	      bound_xl->fops->lk,
	      fd,
	      cmd,
	      &lock);
  return 0;
}



/*
 * server_writedir -
 *
 * @frame:
 * @bound_xl:
 * @params:
 *
 */
int32_t 
server_writedir (call_frame_t *frame,
		 xlator_t *bound_xl,
		 dict_t *params)
{
  data_t *count_data = dict_get (params, "NR_ENTRIES");;
  data_t *buf_data = dict_get (params, "DENTRIES");
  data_t *flag_data = dict_get (params, "FLAGS");
  data_t *fd_data = dict_get (params, "FD");
  dir_entry_t *entry = NULL;
  char *fd_str = NULL; 
  fd_t *fd = NULL; 
  int32_t nr_count = 0;

  if (!fd_data || !flag_data || !buf_data || !count_data) {
    server_writedir_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL);
    return 0;
  }
  
  nr_count = data_to_int32 (count_data);

  {
    dir_entry_t *trav = NULL, *prev = NULL;
    int32_t count, i, bread;
    char *ender = NULL, *buffer_ptr = NULL;
    char tmp_buf[512] = {0,};

    entry = calloc (1, sizeof (dir_entry_t));
    prev = entry;
    buffer_ptr = data_to_str (buf_data);
    
    for (i = 0; i < nr_count ; i++) {
      bread = 0;
      trav = calloc (1, sizeof (dir_entry_t));
      ender = strchr (buffer_ptr, '/');
      count = ender - buffer_ptr;
      trav->name = calloc (1, count + 2);
      strncpy (trav->name, buffer_ptr, count);
      bread = count + 1;
      buffer_ptr += bread;
      
      ender = strchr (buffer_ptr, '\n');
      count = ender - buffer_ptr;
      strncpy (tmp_buf, buffer_ptr, count);
      bread = count + 1;
      buffer_ptr += bread;
      
      /* TODO: use str_to_stat instead */
      {
	uint64_t dev;
	uint64_t ino;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t rdev;
	uint64_t size;
	uint32_t blksize;
	uint64_t blocks;
	uint32_t atime;
	uint32_t atime_nsec;
	uint32_t mtime;
	uint32_t mtime_nsec;
	uint32_t ctime;
	uint32_t ctime_nsec;
	
	sscanf (tmp_buf, GF_STAT_PRINT_FMT_STR,
		&dev,
		&ino,
		&mode,
		&nlink,
		&uid,
		&gid,
		&rdev,
		&size,
		&blksize,
		&blocks,
		&atime,
		&atime_nsec,
		&mtime,
		&mtime_nsec,
		&ctime,
		&ctime_nsec);
	
	trav->buf.st_dev = dev;
	trav->buf.st_ino = ino;
	trav->buf.st_mode = mode;
	trav->buf.st_nlink = nlink;
	trav->buf.st_uid = uid;
	trav->buf.st_gid = gid;
	trav->buf.st_rdev = rdev;
	trav->buf.st_size = size;
	trav->buf.st_blksize = blksize;
	trav->buf.st_blocks = blocks;
	trav->buf.st_atime = atime;
	trav->buf.st_atim.tv_nsec = atime_nsec;
	trav->buf.st_mtime = mtime;
	trav->buf.st_mtim.tv_nsec = mtime_nsec;
	trav->buf.st_ctime = ctime;
	trav->buf.st_ctim.tv_nsec = ctime_nsec;
      }    
      prev->next = trav;
      prev = trav;
    }
  }

  fd_str = data_to_str (fd_data);
  fd = str_to_ptr (fd_str);
  
  STACK_WIND (frame, 
	      server_writedir_cbk, 
	      bound_xl,
	      bound_xl->fops->writedir,
	      fd,
	      data_to_int32 (flag_data),
	      entry,
	      nr_count);

  {
    /* Free the variables allocated in this fop here */
    dir_entry_t *trav = entry->next;
    dir_entry_t *prev = entry;
    while (trav) {
      prev->next = trav->next;
      free (trav->name);
      free (trav);
      trav = prev->next;
    }
    free (entry);
  }
  return 0;
}

/* Management Calls */
/*
 * mop_getspec - getspec function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 */
int32_t 
mop_getspec (call_frame_t *frame,
	     xlator_t *bound_xl,
	     dict_t *params)
{
  int32_t ret = -1;
  int32_t spec_fd = -1;
  
  void *file_data = NULL;
  int32_t file_data_len = 0;
  dict_t *dict = get_new_dict ();
  char *filename = GLUSTERFSD_SPEC_PATH;
  struct stat *stbuf = alloca (sizeof (struct stat));

  if (dict_get (frame->this->options,
		"client-volume-filename")) {
    filename = data_to_str (dict_get (frame->this->options,
				      "client-volume-filename"));
  }
  ret = open (filename, O_RDONLY);
  spec_fd = ret;
  if (spec_fd < 0){
    gf_log ("protocol/server",
	    GF_LOG_DEBUG,
	    "Unable to open %s (%s)",
	    filename,
	    strerror (errno));
    goto fail;
  }
  
  /* to allocate the proper buffer to hold the file data */
  {
    ret = stat (filename, stbuf);
    if (ret < 0){
      goto fail;
    }
    
    file_data_len = stbuf->st_size;
    file_data = calloc (1, file_data_len + 1);
  }
  
  gf_full_read (spec_fd, file_data, file_data_len);
  dict_set (dict, "spec-file-data",
	    data_from_dynstr (file_data));

 fail:
    
  dict_set (dict, "RET", data_from_int32 (ret));
  dict_set (dict, "ERRNO", data_from_int32 (errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_GETSPEC, 
		dict, frame->root->rsp_refs);

  return ret;
}

/*
 * mop_setspec - setspec function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 */
int32_t 
mop_setspec (call_frame_t *frame,
	     xlator_t *bound_xl,
	     dict_t *params)
{
  int32_t ret = -1;
  int32_t spec_fd = -1;
  int32_t remote_errno = 0;
  void *file_data = NULL;
  int32_t file_data_len = 0;
  int32_t offset = 0;
  dict_t *dict = get_new_dict ();

  data_t *data = dict_get (params, "spec-file-data");

  if (!data) {
    goto fail;
  }
  
  file_data = data_to_bin (data);
  file_data_len = data->len;
  

  ret = mkdir (GLUSTERFSD_SPEC_DIR, 0x777);
  
  if (ret < 0 && errno != EEXIST){
    remote_errno = errno;
    goto fail;
  }
  
  ret = open (GLUSTERFSD_SPEC_PATH, O_WRONLY | O_CREAT | O_SYNC);
  spec_fd = ret;
  if (spec_fd < 0){
    remote_errno = errno;
    goto fail;
  }

  while ((ret = write (spec_fd, file_data + offset, file_data_len))){
    if (ret < 0){
      remote_errno = errno;
      goto fail;
    }
    
    if (ret < file_data_len){
      offset = ret + 1;
      file_data_len = file_data_len - ret;
    }
  }
      
 fail:
  
  dict_set (dict, "RET", data_from_int32 (ret));
  dict_set (dict, "ERRNO", data_from_int32 (remote_errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_SETSPEC, 
		dict, frame->root->rsp_refs);
  
  return ret;
}

/*
 * server_mop_lock_cbk - lock callback for server management operation
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_mop_lock_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_LOCK, 
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * mop_lock - lock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params:
 *
 */
int32_t 
mop_lock (call_frame_t *frame,
	  xlator_t *bound_xl,
	  dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  char *path;
  
  path_data = dict_get (params, "PATH");

  if (!path_data) {
    server_mop_lock_cbk (frame,
			 NULL,
			 frame->this,
			 -1,
			 EINVAL);
    return 0;
  }

  path = data_to_str (path_data);

  STACK_WIND (frame,
	      server_mop_lock_cbk,
	      frame->this,
	      frame->this->mops->lock,
	      path);

  return 0;
}


/*
 * server_mop_unlock_cbk - unlock callback for server management operation
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
mop_unlock_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno)
{
  dict_t *reply = get_new_dict ();

  dict_set (reply, "RET", data_from_int32 (op_ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_UNLOCK, 
		reply, frame->root->rsp_refs);

  return 0;
}

/*
 * mop_unlock - unlock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
int32_t 
mop_unlock (call_frame_t *frame,
	    xlator_t *bound_xl,
	    dict_t *params)
{
  data_t *path_data = dict_get (params, "PATH");
  char *path = NULL;
  
  path_data = dict_get (params, "PATH");

  if (!path_data) {
    mop_unlock_cbk (frame, NULL, frame->this, -1, EINVAL);
    return 0;
  }

  path = data_to_str (path_data);

  STACK_WIND (frame, mop_unlock_cbk, frame->this,
	      frame->this->mops->unlock, path);

  return 0;
}

/*
 * mop_unlock - unlock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
int32_t 
mop_listlocks (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  int32_t ret = -1;
  dict_t *dict = get_new_dict ();

  /* logic to read locks and send them to the person who requested for it */

  errno = 0;

  dict_set (dict, "RET_OP", data_from_uint64 (0xbabecafe));
  dict_set (dict, "RET", data_from_int32 (ret));
  dict_set (dict, "ERRNO", data_from_int32 (errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_LISTLOCKS, 
		dict, frame->root->rsp_refs);

  return 0;
}



/*
 * mop_unlock - unlock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
int32_t 
mop_getvolume (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  return 0;
}


static xlator_t *
get_xlator_by_name (xlator_t *some_xl,
		    const char *name)
{
  auto void check_and_set (xlator_t *, void *);

  struct get_xl_struct {
    const char *name;
    xlator_t *reply;
  } get = {
    .name = name,
    .reply = NULL
  };

  void check_and_set (xlator_t *each,
		      void *data)
    {
      if (!strcmp (each->name,
		   ((struct get_xl_struct *) data)->name))
	((struct get_xl_struct *) data)->reply = each;
    }
      
  xlator_foreach (some_xl, check_and_set, &get);

  return get.reply;
}

/*
 * mop_setvolume - setvolume management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
int32_t 
mop_setvolume (call_frame_t *frame,
	       xlator_t *bound_xl,
	       dict_t *params)
{
  int32_t ret = -1;
  int32_t remote_errno = 0;
  dict_t *dict = get_new_dict ();
  server_proto_priv_t *priv;
  data_t *name_data;
  char *name;
  xlator_t *xl;

  priv = SERVER_PRIV (frame);

  name_data = dict_get (params,
			"remote-subvolume");
  if (!name_data) {
    remote_errno = EINVAL;
    dict_set (dict, "ERROR", 
	      str_to_data ("No remote-subvolume option specified"));
    goto fail;
  }

  name = data_to_str (name_data);
  xl = get_xlator_by_name (frame->this,
			   name);

  if (!xl) {
    char *msg;
    asprintf (&msg, "remote-subvolume \"%s\" is not found", name);
    dict_set (dict, "ERROR", data_from_dynstr (msg));
    remote_errno = ENOENT;
    goto fail;
  } else {
    char *searchstr = NULL;
    struct sockaddr_in *_sock = NULL;
    data_t *allow_ip = NULL;

    _sock = &(TRANSPORT_OF (frame))->peerinfo.sockaddr;
    asprintf (&searchstr, "auth.ip.%s.allow", xl->name);
    allow_ip = dict_get (frame->this->options,
				 searchstr);
    
    free (searchstr);
    
    if (allow_ip) {
      gf_log ("server-protocol", GF_LOG_DEBUG,
	      "mop_setvolume: received port = %d",
	      ntohs (_sock->sin_port));
      
      if (ntohs (_sock->sin_port) < 1024) {
	char *ip_addr_str = NULL;
	char *tmp;
	char *ip_addr_cpy = strdup (allow_ip->data);

	ip_addr_str = strtok_r (ip_addr_cpy,
				",",
				&tmp);

	while (ip_addr_str) {
	  gf_log ("server-protocol",  GF_LOG_DEBUG,
		  "mop_setvolume: IP addr = %s, received ip addr = %s", 
		  ip_addr_str, inet_ntoa (_sock->sin_addr));

	  if (fnmatch (ip_addr_str,
		       inet_ntoa (_sock->sin_addr),
		       0) == 0) {
	    ret = 0;
	    priv->bound_xl = xl; 

	    gf_log ("server-protocol",  GF_LOG_DEBUG,
		    "mop_setvolume: accepted client from %s",
		     inet_ntoa (_sock->sin_addr));

	    dict_set (dict, "ERROR", str_to_data ("Success"));
	    break;
	  }
	  ip_addr_str = strtok_r (NULL,
				  ",",
				  &tmp);
	}
	if (ret != 0) {
	  dict_set (dict, "ERROR", 
		    str_to_data ("Authentication Failed: IP address not allowed"));
	}
	free (ip_addr_cpy);
	goto fail;
      } else {
	dict_set (dict, "ERROR", 
		  str_to_data ("Authentication Range not specified in volume spec"));
	goto fail;
      }
    } else {
      char *msg;
      asprintf (&msg, "Volume \"%s\" is not attachable from host %s", 
		xl->name, inet_ntoa (_sock->sin_addr));
      dict_set (dict, "ERROR", data_from_dynstr (msg));
      goto fail;
    }
    if (!priv->bound_xl) {
      dict_set (dict, "ERROR", 
		str_to_data ("Check volume spec file and handshake options"));
      ret = -1;
      remote_errno = EACCES;
      goto fail;
    }
  }
  
 fail:
  dict_set (dict, "RET", data_from_int32 (ret));
  dict_set (dict, "ERRNO", data_from_int32 (remote_errno));

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_SETVOLUME, 
		dict, frame->root->rsp_refs);

  return ret;
}

/*
 * server_mop_stats_cbk - stats callback for server management operation
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 * @stats:err
 *
 * not for external reference
 */

int32_t 
server_mop_stats_cbk (call_frame_t *frame, 
		      void *cookie,
		      xlator_t *xl, 
		      int32_t ret, 
		      int32_t op_errno, 
		      struct xlator_stats *stats)
{
  /* TODO: get this information from somewhere else, not extern */
  int32_t glusterfsd_stats_nr_clients = 0;

  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));

  if (ret == 0) {
    char buffer[256] = {0,};
    sprintf (buffer, 
	     "%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64",%"PRIx64"\n",
	     stats->nr_files,
	     stats->disk_usage,
	     stats->free_disk,
	     stats->total_disk_size,
	     stats->read_usage,
	     stats->write_usage,
	     stats->disk_speed,
	     (int64_t)glusterfsd_stats_nr_clients);

    dict_set (reply, "BUF", data_from_dynstr (strdup (buffer)));
  }

  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_STATS, 
		reply, frame->root->rsp_refs);

  return 0;
}


/*
 * mop_unlock - unlock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
static int32_t
mop_stats (call_frame_t *frame,
	   xlator_t *bound_xl,
	   dict_t *params)
{
  data_t *flag_data = dict_get (params, "FLAGS");

  if (!flag_data) {
    server_mop_stats_cbk (frame, NULL, frame->this, -1, EINVAL, NULL);
    return 0;
  }
  
  STACK_WIND (frame, 
	      server_mop_stats_cbk, 
	      bound_xl,
	      bound_xl->mops->stats,
	      data_to_int64 (flag_data));
  
  return 0;
}

/*
 * server_mop_fsck_cbk - fsck callback for server management operation
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */

int32_t 
server_mop_fsck_cbk (call_frame_t *frame, 
		     void *cookie,
		     xlator_t *xl, 
		     int32_t ret, 
		     int32_t op_errno)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (ret));
  dict_set (reply, "ERRNO", data_from_int32 (op_errno));
  
  server_reply (frame, GF_OP_TYPE_MOP_REPLY, GF_MOP_FSCK, 
		reply, frame->root->rsp_refs);
  
  return 0;
}


/*
 * mop_unlock - unlock management function for server protocol
 * @frame: call frame
 * @bound_xl:
 * @params: parameter dictionary
 *
 */
int32_t 
mop_fsck (call_frame_t *frame,
	  xlator_t *bound_xl,
	  dict_t *params)
{
  data_t *flag_data = dict_get (params, "FLAGS");

  if (!flag_data) {
    server_mop_fsck_cbk (frame, NULL, frame->this, -1, EINVAL);
    return 0;
  }
  
  STACK_WIND (frame, 
	      server_mop_fsck_cbk, 
	      bound_xl,
	      bound_xl->mops->fsck,
	      data_to_int64 (flag_data));
  
  return 0;
}

/* 
 * unknown_op_cbk - This function is called when a opcode for unknown type is called 
 *                  Helps to keep the backward/forward compatiblity
 * @frame: call frame
 * @type:
 * @opcode:
 *
 */

int32_t 
unknown_op_cbk (call_frame_t *frame, 
		int32_t type,
		int32_t opcode)
{
  dict_t *reply = get_new_dict ();
  
  dict_set (reply, "RET", data_from_int32 (-1));
  dict_set (reply, "ERRNO", data_from_int32 (ENOSYS));
  
  server_reply (frame, type, opcode, reply, frame->root->rsp_refs);

  return 0;
}	     

/* 
 * get_frame_for_call - create a frame into the call_ctx_t capable of 
 *                      generating and replying the reply packet by itself.
 *                      By making a call with this frame, the last UNWIND 
 *                      function will have all needed state from its
 *                      frame_t->root to send reply.
 * @trans:
 * @blk:
 * @params:
 *
 * not for external reference
 */

static call_frame_t *
get_frame_for_call (transport_t *trans,
		    gf_block_t *blk,
		    dict_t *params)
{
  call_pool_t *pool = trans->xl->ctx->pool;
  call_ctx_t *_call = (void *) calloc (1, sizeof (*_call));
  data_t *d = NULL;
  server_state_t *state = calloc (1, sizeof (*state));

  if (!pool) {
    pool = trans->xl->ctx->pool = calloc (1, sizeof (*pool));
    pthread_mutex_init (&pool->lock, NULL);
    INIT_LIST_HEAD (&pool->all_frames);
  }

  _call->pool = pool;
  pthread_mutex_lock (&pool->lock);
  list_add (&_call->all_frames, &pool->all_frames);
  pthread_mutex_unlock (&pool->lock);

  state->trans = transport_ref (trans);
  _call->state = state;                        /* which socket */
  _call->unique = blk->callid;                 /* which call */

  _call->frames.root = _call;
  _call->frames.this = trans->xl;

  d = dict_get (params, "CALLER_UID");
  if (d)
    _call->uid = (uid_t) data_to_uint64 (d);
  d = dict_get (params, "CALLER_GID");
  if (d)
    _call->gid = (gid_t) data_to_uint64 (d);
  d = dict_get (params, "CALLER_PID");
  if (d)
    _call->pid = (gid_t) data_to_uint64 (d);

  return &_call->frames;
}

/*
 * prototype of operations function for each of mop and 
 * fop at server protocol level 
 *
 * @frame: call frame pointer
 * @bound_xl: the xlator that this frame is bound to
 * @params: parameters dictionary
 *
 * to be used by protocol interpret, _not_ for exterenal reference
 */
typedef int32_t (*gf_op_t) (call_frame_t *frame,
			    xlator_t *bould_xl,
			    dict_t *params);

static gf_op_t gf_fops[] = {
  server_stat,
  server_readlink,
  server_mknod,
  server_mkdir,
  server_unlink,
  server_rmdir,
  server_symlink,
  server_rename,
  server_link,
  server_chmod,
  server_chown,
  server_truncate,
  server_open,
  server_readv,
  server_writev,
  server_statfs,
  server_flush,
  server_close,
  server_fsync,
  server_setxattr,
  server_getxattr,
  server_removexattr,
  server_opendir,
  server_readdir,
  server_closedir,
  server_fsyncdir,
  server_access,
  server_create,
  server_ftruncate,
  server_fstat,
  server_lk,
  server_utimens,
  server_fchmod,
  server_fchown,
  server_lookup,
  server_forget,
  server_writedir
};

static gf_op_t gf_mops[] = {
  mop_setvolume,
  mop_getvolume,
  mop_stats,
  mop_setspec,
  mop_getspec,
  mop_lock,
  mop_unlock,
  mop_listlocks,
  mop_fsck
};

/*
 * server_protocol_interpret - protocol interpreter function for server 
 *
 * @trans: transport object
 * @blk: data block
 *
 */
static int32_t 
server_protocol_interpret (transport_t *trans,
			   gf_block_t *blk)
{
  int32_t ret = 0;
  dict_t *params = blk->dict;
  server_proto_priv_t *priv = trans->xl_private;
  /* the xlator to STACK_WIND into */
  xlator_t *bound_xl = (xlator_t *)priv->bound_xl; 
  call_frame_t *frame = NULL;
  
  switch (blk->type) {
  case GF_OP_TYPE_FOP_REQUEST:

    /* drop connection for unauthorized fs access */
    if (!bound_xl) {
      gf_log ("protocol/server",
	      GF_LOG_ERROR,
	      "bound_xl is null");
      ret = -1;
      break;
    }

    if (blk->op < 0) {
      gf_log ("protocol/server",
	      GF_LOG_ERROR,
	      "invalid operation is 0x%x", blk->op);
      ret = -1;
      break;
    }

    frame = get_frame_for_call (trans, blk, params);
    frame->root->req_refs = dict_ref (params);
    dict_set (params, NULL, trans->buf);

    if (blk->op > GF_FOP_MAXVALUE) {
      unknown_op_cbk (frame, GF_OP_TYPE_FOP_REQUEST, blk->op);
      break;
    }
    
    ret = gf_fops[blk->op] (frame, bound_xl, params);
    break;
    
  case GF_OP_TYPE_MOP_REQUEST:
    
    if (blk->op < 0) {
      gf_log ("protocol/server",
	      GF_LOG_ERROR,
	      "invalid management operation is 0x%x", blk->op);
      ret = -1;
      break;
    }
    
    frame = get_frame_for_call (trans, blk, params);
    frame->root->req_refs = dict_ref (params);
    dict_set (params, NULL, trans->buf);

    if (blk->op > GF_MOP_MAXVALUE) {
      unknown_op_cbk (frame, GF_OP_TYPE_MOP_REQUEST, blk->op);
      break;
    }

    gf_mops[blk->op] (frame, bound_xl, params);

    break;
  default:
    gf_log ("protocol/server",
	    GF_LOG_DEBUG,
	    "Unknown packet type: %d", blk->type);
    ret = -1;
  }

  dict_unref (params);
  return ret;  
}

/*
 * server_nop_cbk - nop callback for server protocol
 * @frame: call frame
 * @cookie:
 * @this:
 * @op_ret: return value
 * @op_errno: errno
 *
 * not for external reference
 */
static int32_t
server_nop_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno)
{
  /* TODO: cleanup frame->root->state */

  STACK_DESTROY (frame->root);
  return 0;
}

/*
 * get_frame_for_transport - get call frame for specified transport object
 *
 * @trans: transport object
 *
 */
static call_frame_t *
get_frame_for_transport (transport_t *trans)
{
  call_ctx_t *_call = (void *) calloc (1, sizeof (*_call));
  call_pool_t *pool = trans->xl->ctx->pool;
  server_state_t *state;

  if (!pool) {
    pool = trans->xl->ctx->pool = calloc (1, sizeof (*pool));
    pthread_mutex_init (&pool->lock, NULL);
    INIT_LIST_HEAD (&pool->all_frames);
  }

  _call->pool = pool;

  pthread_mutex_lock (&_call->pool->lock);
  list_add (&_call->all_frames, &_call->pool->all_frames);
  pthread_mutex_unlock (&_call->pool->lock);

  state = calloc (1, sizeof (*state));
  state->trans = transport_ref (trans);
  _call->state = state;        /* which socket */
  _call->unique = 0;           /* which call */

  _call->frames.root = _call;
  _call->frames.this = trans->xl;

  return &_call->frames;
}

/*
 * open_file_cleanup_fn - cleanup the open file related data from private data
 *
 * @this:
 * @key:
 * @value:
 * @data:
 *
 */
static void
open_file_cleanup_fn (dict_t *this,
		      char *key,
		      data_t *value,
		      void *cleanup)

{
  dict_t *file_ctx;
  fd_t *fd = NULL;
  transport_t *trans = ((open_file_cleanup_t *)cleanup)->trans;
  char isdir = ((open_file_cleanup_t *)cleanup)->isdir;
  server_proto_priv_t *priv = trans->xl_private;
  xlator_t *bound_xl = (xlator_t *) priv->bound_xl;
  call_frame_t *frame;

  file_ctx = (dict_t *) strtoul (key, NULL, 0);
  fd = (fd_t *) strtoul (key, NULL, 0);
  frame = get_frame_for_transport (trans);

  if (isdir) {
    gf_log ("protocol/server", 
	    GF_LOG_DEBUG,
	    "force releasing directory %p", fd);

    STACK_WIND (frame,
		server_nop_cbk,
		bound_xl,
		bound_xl->fops->closedir,
		fd);
  } else {
    gf_log ("protocol/server",
	    GF_LOG_DEBUG,
	    "force releasing file %p", fd);
    
    STACK_WIND (frame,
		server_nop_cbk,
		bound_xl,
		bound_xl->fops->close,
		fd);
  }

  return;
}

/* 
 * server_protocol_cleanup - cleanup function for server protocol
 *
 * @trans: transport object
 *
 */
static int32_t
server_protocol_cleanup (transport_t *trans)
{
  server_proto_priv_t *priv = trans->xl_private;
  open_file_cleanup_t cleanup;
  call_frame_t *frame;
  dict_t *open_files, *open_dirs;

  cleanup.trans = trans;

  pthread_mutex_lock (&priv->lock);
  open_files = priv->open_files;
  open_dirs = priv->open_dirs;
  priv->open_files = NULL;
  priv->open_dirs = NULL;
  priv->disconnected = 1;
  pthread_mutex_unlock (&priv->lock);

  if (open_files) {
    cleanup.isdir = 0;
    dict_foreach (open_files,
		  open_file_cleanup_fn,
		  &cleanup);
    dict_destroy (open_files);
    priv->open_files = NULL;
  }

  if (open_dirs) {
    cleanup.isdir = 1;
    dict_foreach (open_dirs,
		  open_file_cleanup_fn,
		  &cleanup);
    dict_destroy (open_dirs);
    priv->open_dirs = NULL;
  }

  /* ->unlock () with NULL path will cleanup
     lock manager's internals by remove all
     entries related to this transport
  */

  frame = get_frame_for_transport (trans);

  STACK_WIND (frame,
	      server_nop_cbk,
	      trans->xl,
	      trans->xl->mops->unlock,
	      NULL);

  gf_log ("protocol/server",
	  GF_LOG_DEBUG,
	  "cleaned up xl_private of %p",
	  trans);
  /* TODO: priv should be free'd when all frames are replied */
  free (priv);
  trans->xl_private = NULL;
  return 0;
}


/*
 * init - called during server protocol initialization
 *
 * @this:
 *
 */
int32_t
init (xlator_t *this)
{
  transport_t *trans;
  server_reply_queue_t *queue;

  gf_log (this->name, GF_LOG_DEBUG, "protocol/server xlator loaded");

  if (!this->children) {
    gf_log (this->name, GF_LOG_ERROR,
	    "protocol/server should have subvolume");
    return -1;
  }

  trans = transport_load (this->options,
			  this,
			  this->notify);

  if (!trans) {
    gf_log (this->name, GF_LOG_ERROR,
	    "cannot load transport");
    return -1;
  }

  this->private = trans;

  queue = calloc (1, sizeof (server_reply_queue_t));
  pthread_mutex_init (&queue->lock, NULL);
  pthread_cond_init (&queue->cond, NULL);
  INIT_LIST_HEAD (&queue->list);

  trans->xl_private = queue;

  pthread_create (&queue->thread, NULL, server_reply_proc, queue);

  /* set inode table pointer to point to nearest available inode table */
  xlator_list_t *trav = this->children;
  this->itable = trav->xlator->itable;
    
  return 0;
}

/* 
 * fini - finish function for server protocol, called before 
 *        unloading server protocol.
 *
 * @this:
 *
 */
void
fini (xlator_t *this)
{

  return;
}

/*
 * server_protocol_notify - notify function for server protocol
 * @this: 
 * @trans:
 * @event:
 *
 */

int32_t
notify (xlator_t *this,
	int32_t event,
	void *data,
	...)
{
  int ret = 0;

  switch (event)
    {
    case GF_EVENT_POLLIN:
      {
	transport_t *trans = data;
	gf_block_t *blk;
	server_proto_priv_t *priv = trans->xl_private;

	if (!priv) {
	  priv = (void *) calloc (1, sizeof (*priv));
	  trans->xl_private = priv;
	  priv->open_files = get_new_dict ();
	  priv->open_dirs = get_new_dict ();
	  pthread_mutex_init (&priv->lock, NULL);
	}

	blk = gf_block_unserialize_transport (trans);
	if (!blk) {
	  ret = -1;
	}

	if (!ret) {
	  ret = server_protocol_interpret (trans, blk);
	  if (ret == -1) {
	    /* TODO: Possible loss of frame? */
	    transport_except (trans);
	  }
	  free (blk);
	  break;
	} 
      }
      /* no break for ret check to happen below */
    case GF_EVENT_POLLERR:
      {
	transport_t *trans = data;

	ret = -1;
	server_protocol_cleanup (trans);
	transport_disconnect (trans);
      }
      break;
    default:
      default_notify (this, event, data);
      break;
    }

  return ret;
}


struct xlator_mops mops = {
  .lock = mop_lock_impl,
  .unlock = mop_unlock_impl
};

struct xlator_fops fops = {

};
