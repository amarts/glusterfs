/*
  Copyright (c) 2007-2010 Gluster, Inc. <http://www.gluster.com>
  This file is part of GlusterFS.

  GlusterFS is free software; you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation; either version 3 of the License,
  or (at your option) any later version.

  GlusterFS is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program.  If not, see
  <http://www.gnu.org/licenses/>.
*/

#include "dict.h"
#include "byte-order.h"
#include "common-utils.h"

#include "afr.h"
#include "afr-transaction.h"

#include <signal.h>


#define LOCKED_NO       0x0        /* no lock held */
#define LOCKED_YES      0x1        /* for DATA, METADATA, ENTRY and higher_path
                                      of RENAME */
#define LOCKED_LOWER    0x2        /* for lower_path of RENAME */


afr_fd_ctx_t *
afr_fd_ctx_get (fd_t *fd, xlator_t *this)
{
        uint64_t       ctx = 0;
        afr_fd_ctx_t  *fd_ctx = NULL;
        int            ret = 0;

        ret = fd_ctx_get (fd, this, &ctx);

        if (ret < 0)
                goto out;

        fd_ctx = (afr_fd_ctx_t *)(long) ctx;

out:
        return fd_ctx;
}


static void
afr_pid_save (call_frame_t *frame)
{
        afr_local_t * local = NULL;

        local = frame->local;

        local->saved_pid = frame->root->pid;
}


static void
afr_pid_restore (call_frame_t *frame)
{
        afr_local_t * local = NULL;

        local = frame->local;

        frame->root->pid = local->saved_pid;
}


static void
__mark_all_pending (int32_t *pending[], int child_count,
                    afr_transaction_type type)
{
	int i;
        int j;

        for (i = 0; i < child_count; i++) {
                j = afr_index_for_transaction_type (type);
		pending[i][j] = hton32 (1);
        }
}


static void
__mark_child_dead (int32_t *pending[], int child_count, int child,
                   afr_transaction_type type)
{
        int j;

        j = afr_index_for_transaction_type (type);

	pending[child][j] = 0;
}


static void
__mark_pre_op_done_on_fd (call_frame_t *frame, xlator_t *this, int child_index)
{
        afr_local_t   *local = NULL;
        afr_fd_ctx_t  *fd_ctx = NULL;

        local = frame->local;

        if (!local->fd)
                return;

        fd_ctx = afr_fd_ctx_get (local->fd, this);

        if (!fd_ctx)
                goto out;

        LOCK (&local->fd->lock);
        {
                if (local->transaction.type == AFR_DATA_TRANSACTION)
                        fd_ctx->pre_op_done[child_index]++;
        }
        UNLOCK (&local->fd->lock);
out:
        return;
}


static void
__mark_pre_op_undone_on_fd (call_frame_t *frame, xlator_t *this, int child_index)
{
        afr_local_t   *local = NULL;
        afr_fd_ctx_t  *fd_ctx = NULL;

        local = frame->local;

        if (!local->fd)
                return;

        fd_ctx = afr_fd_ctx_get (local->fd, this);

        if (!fd_ctx)
                goto out;

        LOCK (&local->fd->lock);
        {
                if (local->transaction.type == AFR_DATA_TRANSACTION)
                        fd_ctx->pre_op_done[child_index]--;
        }
        UNLOCK (&local->fd->lock);
out:
        return;
}


static void
__mark_down_children (int32_t *pending[], int child_count,
                      unsigned char *child_up, afr_transaction_type type)
{
	int i;
	int j;

	for (i = 0; i < child_count; i++) {
                j = afr_index_for_transaction_type (type);

		if (!child_up[i])
			pending[i][j] = 0;
        }
}


static void
__mark_all_success (int32_t *pending[], int child_count,
                    afr_transaction_type type)
{
	int i;
        int j;

	for (i = 0; i < child_count; i++) {
                j = afr_index_for_transaction_type (type);
		pending[i][j] = hton32 (-1);
        }
}


static int
__changelog_enabled (afr_private_t *priv, afr_transaction_type type)
{
	int ret = 0;

	switch (type) {
	case AFR_DATA_TRANSACTION:
		if (priv->data_change_log)
			ret = 1;

		break;

	case AFR_METADATA_TRANSACTION:
		if (priv->metadata_change_log)
			ret = 1;

		break;

	case AFR_ENTRY_TRANSACTION:
	case AFR_ENTRY_RENAME_TRANSACTION:
		if (priv->entry_change_log)
			ret = 1;

		break;
	}

	return ret;
}


static int
__changelog_needed_pre_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv  = NULL;
	afr_local_t   * local = NULL;

	int op_ret   = 0;

	priv  = this->private;
	local = frame->local;

	if (__changelog_enabled (priv, local->transaction.type)) {
		switch (local->op) {

		case GF_FOP_WRITE:
		case GF_FOP_FTRUNCATE:
                        op_ret = 1;
			break;

		case GF_FOP_FLUSH:
			op_ret = 0;
			break;

		default:
			op_ret = 1;
		}
	}

	return op_ret;
}


static int
__changelog_needed_post_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv  = NULL;
	afr_local_t   * local = NULL;

	int op_ret = 0;
	afr_transaction_type type = -1;

	priv  = this->private;
	local = frame->local;
	type  = local->transaction.type;

	if (__changelog_enabled (priv, type)) {
                switch (local->op) {

                case GF_FOP_WRITE:
                case GF_FOP_FTRUNCATE:
                        op_ret = 1;
                        break;

                case GF_FOP_FLUSH:
                        op_ret = 0;
                        break;

                default:
                        op_ret = 1;
                }
        }

	return op_ret;
}


static int
afr_set_pending_dict (afr_private_t *priv, dict_t *xattr, int32_t **pending)
{
        int i;
        int ret = 0;

        for (i = 0; i < priv->child_count; i++) {
                ret = dict_set_static_bin (xattr, priv->pending_key[i],
                                           pending[i], 3 * sizeof (int32_t));
                /* 3 = data+metadata+entry */

                if (ret < 0)
                        goto out;
        }

out:
        return ret;
}


static int
afr_set_piggyback_dict (afr_private_t *priv, dict_t *xattr, int32_t **pending,
                        afr_transaction_type type)
{
        int i;
        int ret = 0;
        int *arr = NULL;
        int index = 0;

        index = afr_index_for_transaction_type (type);

        for (i = 0; i < priv->child_count; i++) {
                arr = GF_CALLOC (3 * sizeof (int32_t), priv->child_count,
                                 gf_afr_mt_char);
                if (!arr) {
                        ret = -1;
                        goto out;
                }

                memcpy (arr, pending[i], 3 * sizeof (int32_t));

                arr[index]++;

                ret = dict_set_bin (xattr, priv->pending_key[i],
                                    arr, 3 * sizeof (int32_t));
                /* 3 = data+metadata+entry */

                if (ret < 0)
                        goto out;
        }

out:
        return ret;
}


int
afr_lock_server_count (afr_private_t *priv, afr_transaction_type type)
{
	int ret = 0;

	switch (type) {
	case AFR_DATA_TRANSACTION:
		ret = priv->child_count;
		break;

	case AFR_METADATA_TRANSACTION:
		ret = priv->child_count;
		break;

	case AFR_ENTRY_TRANSACTION:
	case AFR_ENTRY_RENAME_TRANSACTION:
		ret = priv->child_count;
		break;
	}

	return ret;
}

/* {{{ pending */

int32_t
afr_changelog_post_op_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
			   int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
        afr_internal_lock_t *int_lock = NULL;
	afr_private_t       *priv     = NULL;
	afr_local_t         *local    = NULL;
        int                  child_index = 0;

	int call_count = -1;

	priv     = this->private;
	local    = frame->local;
        int_lock = &local->internal_lock;

        child_index = (long) cookie;

        if (op_ret == 1) {
        }

        if (op_ret == 0) {
                __mark_pre_op_undone_on_fd (frame, this, child_index);
        }

	LOCK (&frame->lock);
	{
		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0) {
                if (afr_lock_server_count (priv, local->transaction.type) == 0) {
                        local->transaction.done (frame, this);
                } else {
                        int_lock->lock_cbk = local->transaction.done;
                        afr_unlock (frame, this);
                }
	}

	return 0;
}


void
afr_update_read_child (call_frame_t *frame, xlator_t *this, inode_t *inode,
                       afr_transaction_type type)
{
        int           curr_read_child = -1;
        int           new_read_child = -1;
        afr_private_t   *priv = NULL;
        afr_local_t  *local = NULL;
        int         **pending = NULL;
        int           idx = 0;

        idx = afr_index_for_transaction_type (type);

        priv = this->private;
        local = frame->local;
        curr_read_child = afr_read_child (this, inode);
        pending = local->pending;

        if (pending[curr_read_child][idx] != 0)
                return;

        /* need to set new read_child */
        for (new_read_child = 0; new_read_child < priv->child_count;
             new_read_child++) {

                if (!priv->child_up[new_read_child])
                        /* child is down */
                        continue;

                if (pending[new_read_child][idx] == 0)
                        /* op just failed */
                        continue;

                break;
        }

        if (new_read_child == priv->child_count)
                /* all children uneligible. leave as-is */
                return;

        afr_set_read_child (this, inode, new_read_child);
}


int 
afr_changelog_post_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv = this->private;
        afr_internal_lock_t *int_lock = NULL;
	int ret        = 0;
	int i          = 0;
	int call_count = 0;

	afr_local_t *  local = NULL;
        afr_fd_ctx_t  *fdctx = NULL;
	dict_t        **xattr = NULL;
        int            piggyback = 0;
        int            index = 0;
        int            nothing_failed = 1;

	local    = frame->local;
        int_lock = &local->internal_lock;

	__mark_down_children (local->pending, priv->child_count,
                              local->child_up, local->transaction.type);

        if (local->fd)
                afr_update_read_child (frame, this, local->fd->inode,
                                       local->transaction.type);

        xattr = alloca (priv->child_count * sizeof (*xattr));
        memset (xattr, 0, (priv->child_count * sizeof (*xattr)));
	for (i = 0; i < priv->child_count; i++) {
                xattr[i] = get_new_dict ();
                dict_ref (xattr[i]);
        }

        call_count = afr_up_children_count (priv->child_count, local->child_up);

        if (local->transaction.type == AFR_ENTRY_RENAME_TRANSACTION) {
                call_count *= 2;
        }

	local->call_count = call_count;

        if (local->fd)
                fdctx = afr_fd_ctx_get (local->fd, this);

	if (call_count == 0) {
		/* no child is up */
                for (i = 0; i < priv->child_count; i++) {
                        dict_unref (xattr[i]);
                }

                int_lock->lock_cbk = local->transaction.done;
		afr_unlock (frame, this);
		return 0;
	}

        /* check if something has failed, to handle piggybacking */
        nothing_failed = 1;
        index = afr_index_for_transaction_type (local->transaction.type);
        for (i = 0; i < priv->child_count; i++) {
                if (local->pending[i][index] == 0) {
                        nothing_failed = 0;
                        break;
                }
        }

        index = afr_index_for_transaction_type (local->transaction.type);
        if (local->optimistic_change_log &&
            local->transaction.type != AFR_DATA_TRANSACTION) {
                /* if nothing_failed, then local->pending[..] == {0 .. 0} */
                for (i = 0; i < priv->child_count; i++)
                        local->pending[i][index]++;
        }

	for (i = 0; i < priv->child_count; i++) {
		if (!local->child_up[i])
                        continue;

                ret = afr_set_pending_dict (priv, xattr[i],
                                            local->pending);

                if (ret < 0)
                        gf_log (this->name, GF_LOG_DEBUG,
                                "failed to set pending entry");


                switch (local->transaction.type) {
                case AFR_DATA_TRANSACTION:
                {
                        if (!fdctx) {
                                STACK_WIND (frame, afr_changelog_post_op_cbk,
                                            priv->children[i],
                                            priv->children[i]->fops->xattrop,
                                            &local->loc,
                                            GF_XATTROP_ADD_ARRAY, xattr[i]);
                                break;
                        }

                        LOCK (&local->fd->lock);
                        {
                                piggyback = 0;
                                if (fdctx->pre_op_piggyback[i]) {
                                        fdctx->pre_op_piggyback[i]--;
                                        piggyback = 1;
                                }
                        }
                        UNLOCK (&local->fd->lock);

                        if (piggyback && !nothing_failed)
                                ret = afr_set_piggyback_dict (priv, xattr[i],
                                                              local->pending,
                                                              local->transaction.type);

                        if (nothing_failed && piggyback) {
                                afr_changelog_post_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                        } else {
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_post_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->fxattrop,
                                                   local->fd,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                        }
                }
                break;
                case AFR_METADATA_TRANSACTION:
                {
                        if (nothing_failed) {
                                afr_changelog_post_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                                break;
                        }

                        if (local->fd)
                                STACK_WIND (frame, afr_changelog_post_op_cbk,
                                            priv->children[i],
                                            priv->children[i]->fops->fxattrop,
                                            local->fd,
                                            GF_XATTROP_ADD_ARRAY, xattr[i]);
                        else
                                STACK_WIND (frame, afr_changelog_post_op_cbk,
                                            priv->children[i],
                                            priv->children[i]->fops->xattrop,
                                            &local->loc,
                                            GF_XATTROP_ADD_ARRAY, xattr[i]);
                }
                break;

                case AFR_ENTRY_RENAME_TRANSACTION:
                {
                        if (nothing_failed) {
                                afr_changelog_post_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                        } else {
                                STACK_WIND_COOKIE (frame, afr_changelog_post_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->xattrop,
                                                   &local->transaction.new_parent_loc,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                        }
                        call_count--;
                }

                /*
                  set it again because previous stack_wind
                  might have already returned (think of case
                  where subvolume is posix) and would have
                  used the dict as placeholder for return
                  value
                */

                ret = afr_set_pending_dict (priv, xattr[i],
                                            local->pending);

                if (ret < 0)
                        gf_log (this->name, GF_LOG_DEBUG,
                                "failed to set pending entry");

                /* fall through */

                case AFR_ENTRY_TRANSACTION:
                {
                        if (nothing_failed) {
                                afr_changelog_post_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                                break;
                        }

                        if (local->fd)
                                STACK_WIND (frame, afr_changelog_post_op_cbk,
                                            priv->children[i],
                                            priv->children[i]->fops->fxattrop,
                                            local->fd,
                                            GF_XATTROP_ADD_ARRAY, xattr[i]);
                        else
                                STACK_WIND (frame, afr_changelog_post_op_cbk,
                                            priv->children[i],
                                            priv->children[i]->fops->xattrop,
                                            &local->transaction.parent_loc,
                                            GF_XATTROP_ADD_ARRAY, xattr[i]);
                }
                break;
                }

                if (!--call_count)
                        break;
	}

        for (i = 0; i < priv->child_count; i++) {
                dict_unref (xattr[i]);
        }

	return 0;
}


int32_t
afr_changelog_pre_op_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                          int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = this->private;
	loc_t       *   loc   = NULL;

	int call_count  = -1;
	int child_index = (long) cookie;

	local = frame->local;
	loc   = &local->loc;

	LOCK (&frame->lock);
	{
                if (op_ret == 1) {
                        /* special op_ret for piggyback */
                }

                if (op_ret == 0) {
                        __mark_pre_op_done_on_fd (frame, this, child_index);
                }

		if (op_ret == -1) {
			local->child_up[child_index] = 0;

			if (op_errno == ENOTSUP) {
				gf_log (this->name, GF_LOG_ERROR,
					"xattrop not supported by %s",
					priv->children[child_index]->name);
				local->op_ret = -1;

			} else if (!child_went_down (op_ret, op_errno)) {
				gf_log (this->name, GF_LOG_ERROR,
					"xattrop failed on child %s: %s",
					priv->children[child_index]->name,
					strerror (op_errno));
			}
			local->op_errno = op_errno;
		}

		call_count = --local->call_count;
	}
	UNLOCK (&frame->lock);

	if (call_count == 0) {
		if ((local->op_ret == -1) &&
		    (local->op_errno == ENOTSUP)) {
			local->transaction.resume (frame, this);
		} else {
                        __mark_all_success (local->pending, priv->child_count,
                                            local->transaction.type);

                        afr_pid_restore (frame);

			local->transaction.fop (frame, this);
		}
	}

	return 0;
}


int
afr_changelog_pre_op (call_frame_t *frame, xlator_t *this)
{
	afr_private_t * priv = this->private;
	int i = 0;
	int ret = 0;
	int call_count = 0;
	dict_t **xattr = NULL;
        afr_fd_ctx_t *fdctx = NULL;

	afr_local_t *local = NULL;
        int          piggyback = 0;

	local = frame->local;

        xattr = alloca (priv->child_count * sizeof (*xattr));
        memset (xattr, 0, (priv->child_count * sizeof (*xattr)));

	for (i = 0; i < priv->child_count; i++) {
                xattr[i] = get_new_dict ();
                dict_ref (xattr[i]);
        }

	call_count = afr_up_children_count (priv->child_count,
					    local->child_up);

	if (local->transaction.type == AFR_ENTRY_RENAME_TRANSACTION) {
		call_count *= 2;
	}

	if (call_count == 0) {
		/* no child is up */
                for (i = 0; i < priv->child_count; i++) {
                        dict_unref (xattr[i]);
                }

                local->internal_lock.lock_cbk =
                        local->transaction.done;
		afr_unlock (frame, this);
		return 0;
	}

	local->call_count = call_count;

	__mark_all_pending (local->pending, priv->child_count,
                            local->transaction.type);

        if (local->fd)
                fdctx = afr_fd_ctx_get (local->fd, this);

	for (i = 0; i < priv->child_count; i++) {
                if (!local->child_up[i])
                        continue;
                ret = afr_set_pending_dict (priv, xattr[i],
                                            local->pending);

                if (ret < 0)
                        gf_log (this->name, GF_LOG_DEBUG,
                                "failed to set pending entry");


                switch (local->transaction.type) {
                case AFR_DATA_TRANSACTION:
                {
                        if (!fdctx) {
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->xattrop,
                                                   &(local->loc),
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                                break;
                        }

                        LOCK (&local->fd->lock);
                        {
                                piggyback = 0;
                                if (fdctx->pre_op_done[i]) {
                                        fdctx->pre_op_piggyback[i]++;
                                        piggyback = 1;
                                        fdctx->hit++;
                                } else {
                                        fdctx->miss++;
                                }
                        }
                        UNLOCK (&local->fd->lock);

                        if (piggyback)
                                afr_changelog_pre_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                        else
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->fxattrop,
                                                   local->fd,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                }
                break;
                case AFR_METADATA_TRANSACTION:
                {
                        if (local->optimistic_change_log) {
                                afr_changelog_pre_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                                break;
                        }

                        if (local->fd)
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->fxattrop,
                                                   local->fd,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                        else
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->xattrop,
                                                   &(local->loc),
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                }
                break;

                case AFR_ENTRY_RENAME_TRANSACTION:
                {
                        if (local->optimistic_change_log) {
                                afr_changelog_pre_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                        } else {
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->xattrop,
                                                   &local->transaction.new_parent_loc,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                        }

                        call_count--;
                }


                /*
                  set it again because previous stack_wind
                  might have already returned (think of case
                  where subvolume is posix) and would have
                  used the dict as placeholder for return
                  value
                */

                ret = afr_set_pending_dict (priv, xattr[i],
                                            local->pending);

                if (ret < 0)
                        gf_log (this->name, GF_LOG_DEBUG,
                                "failed to set pending entry");

                /* fall through */

                case AFR_ENTRY_TRANSACTION:
                {
                        if (local->optimistic_change_log) {
                                afr_changelog_pre_op_cbk (frame, (void *)(long)i,
                                                          this, 1, 0, xattr[i]);
                                break;
                        }

                        if (local->fd)
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->fxattrop,
                                                   local->fd,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                        else
                                STACK_WIND_COOKIE (frame,
                                                   afr_changelog_pre_op_cbk,
                                                   (void *) (long) i,
                                                   priv->children[i],
                                                   priv->children[i]->fops->xattrop,
                                                   &local->transaction.parent_loc,
                                                   GF_XATTROP_ADD_ARRAY, xattr[i]);
                }
                break;
                }

                if (!--call_count)
                        break;
	}

        for (i = 0; i < priv->child_count; i++) {
                dict_unref (xattr[i]);
        }

	return 0;
}


int
afr_post_blocking_inodelk_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        if (int_lock->lock_op_ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking inodelks failed.");
		local->transaction.done (frame, this);
        } else {

                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking inodelks done. Proceeding to FOP");
                afr_internal_lock_finish (frame, this);
        }

        return 0;
}


int
afr_post_nonblocking_inodelk_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        /* Initiate blocking locks if non-blocking has failed */
        if (int_lock->lock_op_ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "Non blocking inodelks failed. Proceeding to blocking");
                int_lock->lock_cbk = afr_post_blocking_inodelk_cbk;
                afr_blocking_lock (frame, this);
        } else {

                gf_log (this->name, GF_LOG_DEBUG,
                        "Non blocking inodelks done. Proceeding to FOP");
                afr_internal_lock_finish (frame, this);
        }

        return 0;
}


int
afr_post_blocking_entrylk_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        if (int_lock->lock_op_ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking entrylks failed.");
		local->transaction.done (frame, this);
        } else {

                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking entrylks done. Proceeding to FOP");
                afr_internal_lock_finish (frame, this);
        }

        return 0;
}


int
afr_post_nonblocking_entrylk_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local = frame->local;
        int_lock = &local->internal_lock;

        /* Initiate blocking locks if non-blocking has failed */
        if (int_lock->lock_op_ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "Non blocking entrylks failed. Proceeding to blocking");
                int_lock->lock_cbk = afr_post_blocking_entrylk_cbk;
                afr_blocking_lock (frame, this);
        } else {

                gf_log (this->name, GF_LOG_DEBUG,
                        "Non blocking entrylks done. Proceeding to FOP");
                afr_internal_lock_finish (frame, this);
        }

        return 0;
}


int
afr_post_blocking_rename_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        if (int_lock->lock_op_ret < 0) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking entrylks failed.");
		local->transaction.done (frame, this);
        } else {

                gf_log (this->name, GF_LOG_DEBUG,
                        "Blocking entrylks done. Proceeding to FOP");
                afr_internal_lock_finish (frame, this);
        }
        return 0;
}


int
afr_post_lower_unlock_cbk (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        GF_ASSERT (!int_lock->higher_locked);

        int_lock->lock_cbk = afr_post_blocking_rename_cbk;
        afr_blocking_lock (frame, this);

        return 0;
}


int
afr_set_transaction_flock (afr_local_t *local)
{
        afr_internal_lock_t *int_lock = NULL;

        int_lock = &local->internal_lock;

        int_lock->lk_flock.l_len   = local->transaction.len;
        int_lock->lk_flock.l_start = local->transaction.start;
        int_lock->lk_flock.l_type  = F_WRLCK;

        return 0;
}

int
afr_lock_rec (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
        afr_local_t         *local    = NULL;

        local    = frame->local;
        int_lock = &local->internal_lock;

        int_lock->transaction_lk_type = AFR_TRANSACTION_LK;

	switch (local->transaction.type) {
	case AFR_DATA_TRANSACTION:
	case AFR_METADATA_TRANSACTION:
                afr_set_transaction_flock (local);

                int_lock->lock_cbk = afr_post_nonblocking_inodelk_cbk;

                afr_nonblocking_inodelk (frame, this);
		break;

        case AFR_ENTRY_RENAME_TRANSACTION:

                int_lock->lock_cbk = afr_post_blocking_rename_cbk;
                afr_blocking_lock (frame, this);
                break;

	case AFR_ENTRY_TRANSACTION:
                int_lock->lk_basename = local->transaction.basename;
                if (&local->transaction.parent_loc)
                        int_lock->lk_loc = &local->transaction.parent_loc;
                else
                        GF_ASSERT (local->fd);

                int_lock->lock_cbk = afr_post_nonblocking_entrylk_cbk;
                afr_nonblocking_entrylk (frame, this);
                break;
        }

        return 0;
}


int
afr_lock (call_frame_t *frame, xlator_t *this)
{
        afr_pid_save (frame);

        frame->root->pid = (long) frame->root;

        afr_set_lk_owner (frame, this);

        afr_set_lock_number (frame, this);

	return afr_lock_rec (frame, this);
}


/* }}} */

int
afr_internal_lock_finish (call_frame_t *frame, xlator_t *this)
{
        afr_local_t   *local = NULL;
        afr_private_t *priv  = NULL;

        priv  = this->private;
        local = frame->local;

        if (__changelog_needed_pre_op (frame, this)) {
                afr_changelog_pre_op (frame, this);
        } else {
                __mark_all_success (local->pending, priv->child_count,
                                    local->transaction.type);

                afr_pid_restore (frame);

                local->transaction.fop (frame, this);
        }

        return 0;
}


int
afr_transaction_resume (call_frame_t *frame, xlator_t *this)
{
        afr_internal_lock_t *int_lock = NULL;
	afr_local_t         *local    = NULL;
	afr_private_t       *priv     = NULL;

	local    = frame->local;
        int_lock = &local->internal_lock;
	priv     = this->private;

	if (__changelog_needed_post_op (frame, this)) {
		afr_changelog_post_op (frame, this);
	} else {
		if (afr_lock_server_count (priv, local->transaction.type) == 0) {
			local->transaction.done (frame, this);
		} else {
                        int_lock->lock_cbk = local->transaction.done;
			afr_unlock (frame, this);
		}
	}

	return 0;
}


/**
 * afr_transaction_fop_failed - inform that an fop failed
 */

void
afr_transaction_fop_failed (call_frame_t *frame, xlator_t *this, int child_index)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	local = frame->local;
	priv  = this->private;

        __mark_child_dead (local->pending, priv->child_count,
                           child_index, local->transaction.type);
}


int
afr_transaction (call_frame_t *frame, xlator_t *this, afr_transaction_type type)
{
	afr_local_t *   local = NULL;
	afr_private_t * priv  = NULL;

	local = frame->local;
	priv  = this->private;

	afr_transaction_local_init (local, priv);

	local->transaction.resume = afr_transaction_resume;
	local->transaction.type   = type;

	if (afr_lock_server_count (priv, local->transaction.type) == 0) {
                afr_internal_lock_finish (frame, this);
	} else {
		afr_lock (frame, this);
	}

	return 0;
}
