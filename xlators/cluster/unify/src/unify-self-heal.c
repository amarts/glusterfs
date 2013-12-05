/*
   Copyright (c) 2007 Z RESEARCH, Inc. <http://www.zresearch.com>
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
 * unify-self-heal.c : 
 *   This file implements few functions which enables 'unify' translator 
 *  to be consistent in its behaviour when 
 *     > a node fails, 
 *     > a node gets added, 
 *     > a failed node comes back
 *     > a new namespace server is added (ie, an fresh namespace server).
 * 
 *  This functionality of 'unify' will enable glusterfs to support storage system 
 *  failure, and maintain consistancy. This works both ways, ie, when an entry 
 *  (either file or directory) is found on namespace server, and not on storage 
 *  nodes, its created in storage nodes and vica-versa.
 * 
 *  The two fops, where it can be implemented are 'getdents ()' and 'lookup ()'
 *
 */

#include "glusterfs.h"
#include "unify.h"
#include "dict.h"
#include "xlator.h"
#include "hashfn.h"
#include "logging.h"
#include "stack.h"
#include "common-utils.h"

#ifdef STATIC
#undef STATIC
#endif
#define STATIC   /*static*/

#define UNIFY_SELF_HEAL_GETDENTS_SIZE 102400


/**
 * unify_background_cbk - this is called by the background functions which 
 *   doesn't return any of inode, or buf. eg: rmdir, unlink, close, etc.
 *
 */
STATIC int32_t 
unify_background_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno)
{
  int32_t callcnt = 0;
  unify_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    if (op_ret == 0)
      local->op_ret = 0;
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
     STACK_DESTROY (frame->root);
  }

  return 0;
}

/**
 * unify_sh_closedir_cbk -
 */
STATIC int32_t
unify_sh_closedir_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = frame->local;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    freee (local->path);
    local->op_ret = 0;
    if (local->offset_list)
      freee (local->offset_list);
    fd_destroy (local->fd);

    /* This is _cbk() of lookup (). */
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->inode,
		  &local->stbuf,
		  local->dict);
  }

  return 0;
}


STATIC int32_t 
unify_sh_setdents_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *this,
			  int32_t op_ret,
			  int32_t op_errno)
{
  int32_t callcnt = -1;
  unify_private_t *priv = this->private;
  unify_local_t *local = frame->local;
  int16_t *list = local->list;
  int32_t index = 0;

  LOCK (&frame->lock);
  {
    /* if local->call_count == 0, that means, setdents on storagenodes is 
     * still pending.
     */
    if (local->call_count)
      callcnt = --local->call_count;
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    for (index = 0; list[index] != -1; index++)
      local->call_count++;
	  
    for (index = 0; list[index] != -1; index++) {
      char need_break = (list[index+1] == -1);
      STACK_WIND (frame,
		  unify_sh_closedir_cbk,
		  priv->xl_array[list[index]],
		  priv->xl_array[list[index]]->fops->closedir,
		  local->fd);
      if (need_break)
	break;
    }
  }
  
  return 0;
}


STATIC int32_t
unify_sh_ns_getdents_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *this,
			  int32_t op_ret,
			  int32_t op_errno,
			  dir_entry_t *entry,
			  int32_t count)
{
  unify_local_t *local = frame->local;
  unify_private_t *priv = this->private;
  int16_t *list = local->list;
  long index = 0;
  
  if (count < UNIFY_SELF_HEAL_GETDENTS_SIZE) {
    LOCK (&frame->lock);
    {
      /* local->call_count will be '0' till now. make it 1 so, it can be 
       * UNWIND'ed for the last call. 
       */
      for (index = 0; list[index] != -1; index++) {
	if (NS(this) != priv->xl_array[list[index]]) {
	  local->call_count++;
	}
      }
    }
    UNLOCK (&frame->lock);
  } else {
    /* count == size, that means, there are more entries to read from */
    //local->call_count = 0;
    local->offset_list[0] += UNIFY_SELF_HEAL_GETDENTS_SIZE;
    STACK_WIND (frame,
		unify_sh_ns_getdents_cbk,
		NS(this),
		NS(this)->fops->getdents,
		local->fd,
		UNIFY_SELF_HEAL_GETDENTS_SIZE,
		local->offset_list[0],
		GF_GET_DIR_ONLY);
  }
  
  for (index = 0; list[index] != -1; index++) {
    if (NS(this) != priv->xl_array[list[index]]) {
      if (entry) {
	/* There is nothing to set */
	STACK_WIND (frame,
		    unify_sh_setdents_cbk,
		    priv->xl_array[list[index]],
		    priv->xl_array[list[index]]->fops->setdents,
		    local->fd,
		    GF_SET_IF_NOT_PRESENT,
		    entry,
		    count);
      } else {
	/* send closedir to all the nodes */
	local->call_count = 0;
	for (index = 0; list[index] != -1; index++)
	  local->call_count++;
	
	for (index = 0; list[index] != -1; index++) {
	  char need_break = (list[index+1] == -1);
	  STACK_WIND (frame,
		      unify_sh_closedir_cbk,
		      priv->xl_array[list[index]],
		      priv->xl_array[list[index]]->fops->closedir,
		      local->fd);
	  if (need_break)
	    break;
	}
      }
    }
  }
  
  return 0;
}

STATIC int32_t 
unify_sh_ns_setdents_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *this,
			  int32_t op_ret,
			  int32_t op_errno)
{
  /* Nothing needs to be done here, as we still need to do setdents of 
   * storage nodes 
   */
  return 0;
}


/**
 * unify_sh_getdents_cbk -
 */
STATIC int32_t
unify_sh_getdents_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       dir_entry_t *entry,
		       int32_t count)
{
  int32_t callcnt = -1;
  unify_local_t *local = frame->local;
  unify_private_t *priv = this->private;
  long index = (long)cookie;
  
  if (op_ret >= 0 && count > 0) {
    /* There is some dentry found, just send the dentry to NS */

    /* use this flag to make sure that setdents is sent atleast once */
    local->entry_count = 1; 

    STACK_WIND (frame,
		unify_sh_ns_setdents_cbk,
		NS(this),
		NS(this)->fops->setdents,
		local->fd,
		GF_SET_IF_NOT_PRESENT,
		entry,
		count);
  }
  
  if (count < UNIFY_SELF_HEAL_GETDENTS_SIZE) {
    LOCK (&frame->lock);
    {
      callcnt = --local->call_count;
    }
    UNLOCK (&frame->lock);
  } else {
    /* count == size, that means, there are more entries to read from */
    local->offset_list[index] += UNIFY_SELF_HEAL_GETDENTS_SIZE;
    _STACK_WIND (frame,
		 unify_sh_getdents_cbk,
		 cookie,
		 priv->xl_array[index],
		 priv->xl_array[index]->fops->getdents,
		 local->fd,
		 UNIFY_SELF_HEAL_GETDENTS_SIZE,
		 local->offset_list[index],
		 GF_GET_ALL);

    gf_log (this->name, GF_LOG_DEBUG, "readdir on (%s) with offset %lld", 
	    priv->xl_array[index]->name, 
	    local->offset_list[index]);
    
  }

  if (!callcnt) {
    /* All storage nodes have done unified setdents on NS node.
     * Now, do getdents from NS and do setdents on storage nodes.
     */
    
    /* offset_list is no longer required for storage nodes now */
    local->offset_list[0] = 0; /* reset */

    STACK_WIND (frame,
		unify_sh_ns_getdents_cbk,
		NS(this),
		NS(this)->fops->getdents,
		local->fd,
		UNIFY_SELF_HEAL_GETDENTS_SIZE,
		0, /* In this call, do send '0' as offset */
		GF_GET_DIR_ONLY);
  }

  return 0;
}

/**
 * unify_sh_opendir_cbk -
 *
 * @cookie: 
 */
STATIC int32_t 
unify_sh_opendir_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      fd_t *fd)
{
  int32_t callcnt = 0;
  unify_local_t *local = frame->local;
  unify_private_t *priv = this->private;
  int16_t *list = NULL;
  int16_t index = 0;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    
    if (op_ret >= 0) {
      local->op_ret = op_ret;
    }
    if (op_ret == -1)
      local->failed = 1;
  }
  UNLOCK (&frame->lock);
  
  if (!callcnt) {
    /* opendir returned from all nodes, do getdents and do setdents on 
     * all nodes 
     */
    if (local->inode->ctx && dict_get (local->inode->ctx, this->name)) {
      list = data_to_ptr (dict_get (local->inode->ctx, this->name));
      if (list) {
	local->list = list;
	for (index = 0; list[index] != -1; index++)
	  local->call_count++;
	/* send getdents() namespace after finishing storage nodes */
	local->call_count--; 

	if (!local->failed) {
	  if (local->call_count) {
	    /* Used as the offset index. This list keeps track of offset
	     * sent to each node during STACK_WIND.
	     */
	    local->offset_list = calloc (priv->child_count, sizeof (off_t));

	    /* Send getdents on all the fds */
	    for (index = 0; list[index] != -1; index++) {
	      char need_break = (list[index+1] == -1);
	      if (priv->xl_array[list[index]] != NS(this)) {
		_STACK_WIND (frame,
			     unify_sh_getdents_cbk,
			     (void *)(long)list[index],
			     priv->xl_array[list[index]],
			     priv->xl_array[list[index]]->fops->getdents,
			     local->fd,
			     UNIFY_SELF_HEAL_GETDENTS_SIZE,
			     0, /* In this call, do send '0' as offset */
			     GF_GET_ALL);
	      }
	      if (need_break)
		break;
	    }
	    /* did a stack wind, so no need to unwind here */
	    return 0;
	  }
	} else {
	  /* Opendir failed on one node, now send closedir to those nodes, 
	   * where it succeeded. 
	   */
	  if (local->call_count) {
	    call_frame_t *bg_frame = copy_frame (frame);
	    unify_local_t *bg_local = NULL;
	    
	    INIT_LOCAL (bg_frame, bg_local);
	    bg_local->call_count = local->call_count;
	    
	    for (index = 0; list[index] != -1; index++) {
	      char need_break = (list[index+1] == -1);
	      STACK_WIND (bg_frame,
			  unify_background_cbk,
			  priv->xl_array[list[index]],
			  priv->xl_array[list[index]]->fops->closedir,
			  local->fd);
	      if (need_break)
		break;
	    }
	  }
	}
      } else {
	/* no list */
	gf_log (this->name, GF_LOG_CRITICAL,
		"'list' not present in the inode ctx");
      }
    } else {
      /* No context at all */
      gf_log (this->name, GF_LOG_CRITICAL,
	      "no context for the inode at this translator");
    }

    /* no inode, or everything is fine, just do STACK_UNWIND */
    if (local->fd)
      fd_destroy (local->fd);
    freee (local->path);
    local->op_ret = 0;

    /* This is lookup_cbk ()'s UNWIND. */
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->inode,
		  &local->stbuf,
		  local->dict);
  }
  return 0;
}

/**
 * gf_unify_self_heal - 
 * 
 * @frame: frame used in lookup. get a copy of it, and use that copy.
 * @this: pointer to unify xlator.
 * @inode: pointer to inode, for which the consistency check is required.
 *
 */
int32_t 
unify_sh_checksum_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       uint8_t *checksum)
{
  unify_local_t *local = frame->local;
  unify_private_t *priv = this->private;
  int16_t *list = NULL;
  int16_t index = 0;
  int32_t callcnt = 0;

  LOCK (&frame->lock);
  {
    callcnt = --local->call_count;
    if (op_ret >= 0) {
      if (NS(this) == (xlator_t *)cookie) {
	memcpy (local->ns_checksum, checksum, 4096);
      } else {
	if (local->entry_count == 0) {
	  /* Initialize the dir_checksum to be used for comparision 
	   * with other storage nodes. Should be done for the first 
	   * successful call *only*.
	   */
	  local->entry_count = 1;
	  memcpy (local->dir_checksum, dir_checksum, 4096);
	}

	/* Reply from the storage nodes */
	for (index = 0; index < 4096; index++) {
	/* Files should be present in only one node */
	  local->file_checksum[index] ^= file_checksum[index];
	  
	  /* directory structure should be same accross */
	  if (local->dir_checksum[index] != dir_checksum[index])
	    local->failed = 1;
	}
      }
    }
  }
  UNLOCK (&frame->lock);

  if (!callcnt) {
    for (index = 0; index < 4096 ; index++) {
      if (local->checksum[index] != local->ns_checksum[index]) {
	local->failed = 1;
	break;
      }
    }
	
    if (local->failed) {
      /* Any self heal will be done at the directory level */
      local->call_count = 0;
      local->op_ret = -1;
      local->failed = 0;
      
      local->fd = fd_create (local->inode);
      list = data_to_ptr (dict_get (local->inode->ctx, this->name));
      for (index = 0; list[index] != -1; index++)
	local->call_count++;
      
      for (index = 0; list[index] != -1; index++) {
	char need_break = (list[index+1] == -1);
	loc_t tmp_loc = {
	  .inode = local->inode,
	  .path = local->path,
	};
	_STACK_WIND (frame,
		     unify_sh_opendir_cbk,
		     priv->xl_array[list[index]]->name,
		     priv->xl_array[list[index]],
		     priv->xl_array[list[index]]->fops->opendir,
		     &tmp_loc,
		     local->fd);
	if (need_break)
	  break;
      }
    } else {
      /* no mismatch */
      freee (local->path);
      
      /* This is lookup_cbk ()'s UNWIND. */
      STACK_UNWIND (frame,
		    local->op_ret,
		    local->op_errno,
		    local->inode,
		    &local->stbuf,
		    local->dict);
    }
  }

  return 0;
}

/**
 * gf_unify_self_heal - 
 * 
 * @frame: frame used in lookup. get a copy of it, and use that copy.
 * @this: pointer to unify xlator.
 * @inode: pointer to inode, for which the consistency check is required.
 *
 */
int32_t 
gf_unify_self_heal (call_frame_t *frame,
		    xlator_t *this,
		    unify_local_t *local)
{
  unify_private_t *priv = this->private;
  inode_t *loc_inode = local->inode;
  int16_t index = 0;
  
  if (local->inode->generation < priv->inode_generation) {
    /* Any self heal will be done at the directory level */
    local->call_count = 0;
    local->op_ret = 0;
    local->failed = 0;

    local->call_count = priv->child_count + 1;

    for (index = 0; index < (priv->child_count + 1); index++) {
      loc_t tmp_loc = {
	.inode = local->inode,
	.path = local->path,
      };
      _STACK_WIND (frame,
		   unify_sh_checksum_cbk,
		   priv->xl_array[index],
		   priv->xl_array[index],
		   priv->xl_array[index]->mops->checksum,
		   &tmp_loc,
		   0);
    }
  } else {
    /* no inode, or everything is fine, just do STACK_UNWIND */
    freee (local->path);
    
    /* This is lookup_cbk ()'s UNWIND. */
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->inode,
		  &local->stbuf,
		  local->dict);
  }

  /* Update the inode's generation to the current generation value. */
  loc_inode->generation = priv->inode_generation;

  return 0;
}

