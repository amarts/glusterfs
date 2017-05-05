/*Copyright (c) 2008-2010 Gluster, Inc. <http://www.gluster.com>
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

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "dict.h"
#include "xlator.h"
#include "defaults.h"
#include "libxlator.h"
#include "common-utils.h"
#include "byte-order.h"
#include "marker-quota.h"
#include "marker-quota-helper.h"

int32_t
loc_fill_from_name (xlator_t *this, loc_t *newloc, loc_t *oldloc, uint64_t ino, char *name)
{
        int32_t   ret  = 0;
        int32_t   len  = 0;
        char     *path = NULL;


        newloc->ino = ino;

        newloc->inode = inode_new (oldloc->inode->table);

        if (!newloc->inode) {
                ret = -1;
                goto out;
        }

        newloc->parent = inode_ref (oldloc->inode);

        len = strlen (oldloc->path);

        if (oldloc->path [len - 1] == '/')
                ret = gf_asprintf ((char **) &path, "%s%s",
                                   oldloc->path, name);
        else
                ret = gf_asprintf ((char **) &path, "%s/%s",
                                   oldloc->path, name);

        if (ret < 0)
                goto out;

        newloc->path = path;

        newloc->name = strrchr (newloc->path, '/');

        if (newloc->name)
                newloc->name++;

        gf_log (this->name, GF_LOG_INFO, "path = %s name =%s",newloc->path, newloc->name);
out:
        return ret;
}

int32_t
dirty_inode_updation_done (call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno)
{
        quota_local_t *local = NULL;

        local = frame->local;

        if (!local->err)
                QUOTA_SAFE_DECREMENT (&local->lock, local->ref);
        else
                frame->local = NULL;

        QUOTA_STACK_DESTROY (frame, this);

        return 0;
}

int32_t
release_lock_on_dirty_inode (call_frame_t *frame, void *cookie, xlator_t *this,
                             int32_t op_ret, int32_t op_errno)
{
        struct gf_flock   lock;
        quota_local_t    *local = NULL;

        local = frame->local;

        if (op_ret == -1) {
                local->err = -1;

                dirty_inode_updation_done (frame, NULL, this, 0, 0);

                return 0;
        }

        if (op_ret == 0)
                local->ctx->dirty = 0;

        lock.l_type   = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start  = 0;
        lock.l_len    = 0;
        lock.l_pid    = 0;

        STACK_WIND (frame,
                    dirty_inode_updation_done,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->loc, F_SETLKW, &lock);

        return 0;
}

int32_t
mark_inode_undirty (call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        int32_t        ret       = -1;
        int64_t       *size      = NULL;
        dict_t        *newdict   = NULL;
        quota_local_t *local     = NULL;
        marker_conf_t  *priv      = NULL;

        local = (quota_local_t *) frame->local;

        if (op_ret == -1)
                goto err;

        priv = (marker_conf_t *) this->private;

        if (!dict)
                goto wind;

        ret = dict_get_bin (dict, QUOTA_SIZE_KEY, (void **) &size);
        if (ret)
                goto wind;

        local->ctx->size = ntoh64 (*size);

wind:
        newdict = dict_new ();
        if (!newdict)
                goto err;

        ret = dict_set_int8 (newdict, QUOTA_DIRTY_KEY, 0);
        if (ret)
                goto err;

        STACK_WIND (frame, release_lock_on_dirty_inode,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr,
                    &local->loc, newdict, 0);
        ret = 0;

err:
        if (op_ret == -1 || ret == -1) {
                local->err = -1;

                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);
        }

        if (newdict)
                dict_unref (newdict);

        return 0;
}

int32_t
update_size_xattr (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, inode_t *inode,
                   struct iatt *buf, dict_t *dict, struct iatt *postparent)
{
        int32_t          ret      = -1;
        dict_t          *new_dict = NULL;
        int64_t         *size     = NULL;
        int64_t         *delta    = NULL;
        quota_local_t   *local    = NULL;
        marker_conf_t   *priv     = NULL;

        local = frame->local;

        if (op_ret == -1)
                goto err;

        priv = this->private;

        if (!dict)
                goto err;

        ret = dict_get_bin (dict, QUOTA_SIZE_KEY, (void **) &size);
        if (!size)
                goto err;

        QUOTA_ALLOC_OR_GOTO (delta, int64_t, ret, err);

        *delta = hton64 (ntoh64 (*size) - local->sum);

        gf_log (this->name, GF_LOG_DEBUG, "calculated size = %"PRId64", "
                "original size = %"PRIu64
                " path = %s diff = %"PRIu64, local->sum, ntoh64 (*size),
                local->loc.path, ntoh64 (*delta));

        new_dict = dict_new ();
        if (!new_dict);

        ret = dict_set_bin (new_dict, QUOTA_SIZE_KEY, delta, 8);
        if (ret)
                goto err;

        STACK_WIND (frame, mark_inode_undirty, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->xattrop, &local->loc,
                    GF_XATTROP_ADD_ARRAY64, new_dict);

        ret = 0;

err:
        if (op_ret == -1 || ret == -1) {
                local->err = -1;

                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);
        }

        if (new_dict)
                dict_unref (new_dict);

        return 0;
}

int32_t
get_dirty_inode_size (call_frame_t *frame, xlator_t *this)
{
        int32_t          ret            = -1;
        dict_t          *dict           = NULL;
        quota_local_t   *local          = NULL;
        marker_conf_t    *priv           = NULL;

        local = (quota_local_t *) frame->local;

        priv = (marker_conf_t *) this->private;

        dict = dict_new ();
        if (!dict) {
                ret = -1;
                goto err;
        }

        ret = dict_set_int64 (dict, QUOTA_SIZE_KEY, 0);
        if (ret)
                goto err;

        STACK_WIND (frame, update_size_xattr, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->lookup, &local->loc, dict);
        ret =0;

err:
        if (ret) {
                local->err = -1;

                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);
        }

        return 0;
}

int32_t
get_child_contribution (call_frame_t *frame,
                        void *cookie,
                        xlator_t *this,
                        int32_t op_ret,
                        int32_t op_errno,
                        inode_t *inode,
                        struct iatt *buf,
                        dict_t *dict,
                        struct iatt *postparent)
{
        int32_t          ret            = -1;
	int32_t		 val	 	= 0;
        char             contri_key [512] = {0, };
        int64_t         *contri         = NULL;
        quota_local_t   *local          = NULL;

        local = frame->local;

        frame->local = NULL;

        QUOTA_STACK_DESTROY (frame, this);

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s", strerror (op_errno));

                local->err = -2;

                release_lock_on_dirty_inode (local->frame, NULL, this, 0, 0);

                goto out;
        }

        if (local->err)
                goto out;

        GET_CONTRI_KEY (contri_key, local->loc.inode->gfid, ret);
        if (ret < 0)
                goto out;

        if (!dict)
                goto out;

        if (dict_get_bin (dict, contri_key, (void **) &contri) == 0)
                local->sum += ntoh64 (*contri);

out:
        LOCK (&local->lock);
        {
                val = local->dentry_child_count--;
        }
        UNLOCK (&local->lock);

        if (val== 0) {
                if (local->err) {
                        QUOTA_SAFE_DECREMENT (&local->lock, local->ref);

                        quota_local_unref (this, local);
                } else
                        quota_dirty_inode_readdir (local->frame, NULL, this,
                                                   0, 0, NULL);
        }

        return 0;
}

int32_t
quota_readdir_cbk (call_frame_t *frame,
                   void *cookie,
                   xlator_t *this,
                   int32_t op_ret,
                   int32_t op_errno,
                   gf_dirent_t *entries)
{
        char             contri_key [512] = {0, };
        loc_t            loc;
        int32_t          ret            = 0;
        off_t            offset         = 0;
        int32_t          count          = 0;
        dict_t          *dict           = NULL;
        quota_local_t   *local          = NULL;
        gf_dirent_t     *entry          = NULL;
        call_frame_t    *newframe      = NULL;

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_DEBUG,
                        "readdir failed %s", strerror (op_errno));
                local->err = -1;

                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);

                return 0;
        } else if (op_ret == 0) {
                get_dirty_inode_size (frame, this);

                return 0;
        }

        local->dentry_child_count =  0;

        list_for_each_entry (entry, (&entries->list), list) {
              gf_log (this->name, GF_LOG_INFO, "entry  = %s", entry->d_name);

              if ((!strcmp (entry->d_name, ".")) || (!strcmp (entry->d_name, ".."))) {
                      gf_log (this->name, GF_LOG_INFO, "entry  = %s", entry->d_name);
                      continue;
              }
              count++;
        }

        local->frame = frame;

        list_for_each_entry (entry, (&entries->list), list) {
                gf_log (this->name, GF_LOG_INFO, "entry  = %s", entry->d_name);

                if ((!strcmp (entry->d_name, ".")) || (!strcmp (entry->d_name, ".."))) {
                        gf_log (this->name, GF_LOG_INFO, "entry  = %s", entry->d_name);
                        offset = entry->d_off;
                        continue;
                }

                ret = loc_fill_from_name (this, &loc, &local->loc,
                                          entry->d_ino, entry->d_name);
                if (ret < 0)
                        goto out;

                newframe = copy_frame (frame);
                if (!newframe) {
                        ret = -1;
                        goto out;
                }

                newframe->local = local;

                dict = dict_new ();
                if (!dict) {
                        ret = -1;
                        goto out;
                }

                GET_CONTRI_KEY (contri_key, local->loc.inode->gfid, ret);
                if (ret < 0)
                        goto out;

                ret = dict_set_int64 (dict, contri_key, 0);
                if (ret)
                        goto out;

                QUOTA_SAFE_INCREMENT (&local->lock, local->dentry_child_count);

                STACK_WIND (newframe,
                            get_child_contribution,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->lookup,
                            &loc, dict);

                offset = entry->d_off;

                loc_wipe (&loc);

        out:
                if (dict) {
                        dict_unref (dict);
                        dict = NULL;
                }

                if (ret) {
                        LOCK (&local->lock);
                        {
                                if (local->dentry_child_count == 0)
                                        local->err = -1;
                                else
                                        local->err = -2;
                        }
                        UNLOCK (&local->lock);

                        if (newframe) {
                                newframe->local = NULL;

                                QUOTA_STACK_DESTROY (newframe, this);
                        }

                        break;
                }
        }
        gf_log (this->name, GF_LOG_INFO, "offset before =%"PRIu64,
                local->d_off);
        local->d_off +=offset;
        gf_log (this->name, GF_LOG_INFO, "offset after = %"PRIu64,
                local->d_off);

        if (ret)
                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);

        else if (count == 0 )
                get_dirty_inode_size (frame, this);

        return 0;
}

int32_t
quota_dirty_inode_readdir (call_frame_t *frame,
                           void *cookie,
                           xlator_t *this,
                           int32_t op_ret,
                           int32_t op_errno,
                           fd_t *fd)
{
        quota_local_t *local = NULL;

        local = frame->local;

        if (op_ret == -1) {
                local->err = -1;
                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);
                return 0;
        }

        if (local->fd == NULL)
                local->fd = fd_ref (fd);

        STACK_WIND (frame,
                    quota_readdir_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readdir,
                    local->fd, READDIR_BUF, local->d_off);

        return 0;
}

int32_t
check_if_still_dirty (call_frame_t *frame,
                      void *cookie,
                      xlator_t *this,
                      int32_t op_ret,
                      int32_t op_errno,
                      inode_t *inode,
                      struct iatt *buf,
                      dict_t *dict,
                      struct iatt *postparent)
{
        int8_t           dirty          = -1;
        int32_t          ret            = -1;
        fd_t            *fd             = NULL;
        quota_local_t   *local          = NULL;
        marker_conf_t   *priv           = NULL;

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "failed to get "
                        "the dirty xattr for %s", local->loc.path);
                goto err;
        }

        priv = this->private;

        if (!dict) {
              ret = -1;
              goto err;
        }

        ret = dict_get_int8 (dict, QUOTA_DIRTY_KEY, &dirty);
        if (ret)
                goto err;

        //the inode is not dirty anymore
        if (dirty == 0) {
                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);

                return 0;
        }

        fd = fd_create (local->loc.inode, frame->root->pid);

        local->d_off = 0;

        STACK_WIND(frame,
                   quota_dirty_inode_readdir,
                   FIRST_CHILD(this),
                   FIRST_CHILD(this)->fops->opendir,
                   &local->loc, fd);

        ret = 0;

err:
        if (op_ret == -1 || ret == -1) {
                local->err = -1;
                release_lock_on_dirty_inode (frame, NULL, this, 0, 0);
        }

        return 0;
}

int32_t
get_dirty_xattr (call_frame_t *frame, void *cookie,
                 xlator_t *this, int32_t op_ret, int32_t op_errno)
{
        int32_t         ret             = -1;
        dict_t          *xattr_req      = NULL;
        quota_local_t   *local          = NULL;
        marker_conf_t   *priv           = NULL;

        if (op_ret == -1) {
                dirty_inode_updation_done (frame, NULL, this, 0, 0);
                return 0;
        }

        priv = (marker_conf_t *) this->private;

        local = frame->local;

        xattr_req = dict_new ();
        if (xattr_req == NULL) {
                ret = -1;
                goto err;
        }

        ret = dict_set_int8 (xattr_req, QUOTA_DIRTY_KEY, 0);
        if (ret)
                goto err;

        STACK_WIND (frame,
                    check_if_still_dirty,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->lookup,
                    &local->loc,
                    xattr_req);
        ret = 0;

err:
        if (ret) {
                local->err = -1;
                release_lock_on_dirty_inode(frame, NULL, this, 0, 0);
        }

        if (xattr_req)
                dict_unref (xattr_req);

        return 0;
}

int32_t
update_dirty_inode (xlator_t *this,
                    loc_t *loc,
                    quota_inode_ctx_t *ctx,
                    inode_contribution_t *contribution)
{
        int32_t          ret    = -1;
        quota_local_t   *local  = NULL;
        struct gf_flock  lock;
        call_frame_t    *frame  = NULL;

        frame = create_frame (this, this->ctx->pool);
        if (frame == NULL) {
                ret = -1;
                goto out;
        }

        local = quota_local_new ();
        if (local == NULL)
                goto fr_destroy;

        frame->local = local;

        ret = loc_copy (&local->loc, loc);
        if (ret < 0)
                goto fr_destroy;

        local->ctx = ctx;

        local->contri = contribution;

        frame->root->lk_owner = cn++;

        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0;
        lock.l_len = 0;

        STACK_WIND (frame,
                    get_dirty_xattr,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->loc, F_SETLKW, &lock);
        return 0;

fr_destroy:
         QUOTA_STACK_DESTROY (frame, this);
out:

        return 0;
}


int32_t
quota_inode_creation_done (call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno)
{
        quota_local_t *local = NULL;

        if (frame == NULL)
                return 0;

        local = frame->local;

        QUOTA_STACK_DESTROY (frame, this);

        return 0;
}

int32_t
create_dirty_xattr (call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        int32_t          ret       = -1;
        dict_t          *newdict   = NULL;
        quota_local_t   *local     = NULL;
        marker_conf_t   *priv      = NULL;

        if (op_ret == -1 && op_errno == ENOTCONN)
                goto err;

        local = frame->local;

        priv = (marker_conf_t *) this->private;

        if (local->loc.inode->ia_type == IA_IFDIR) {
                newdict = dict_new ();
                if (!newdict)
                        goto err;

                ret = dict_set_int8 (newdict, QUOTA_DIRTY_KEY, 0);
                if (ret == -1)
                        goto err;

                STACK_WIND (frame, quota_inode_creation_done,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->setxattr,
                            &local->loc, newdict, 0);
        } else
                quota_inode_creation_done (frame, NULL, this, 0, 0);

        ret = 0;

err:
        if (ret == -1)
                quota_inode_creation_done (frame, NULL, this, -1, 0);

        if (newdict)
                dict_unref (newdict);

        return 0;
}


int32_t
quota_set_inode_xattr (xlator_t *this, loc_t *loc)
{
        int32_t               ret       = 0;
        int64_t              *value     = NULL;
        int64_t              *size      = NULL;
        dict_t               *dict      = NULL;
        char                  key[512]  = {0, };
        call_frame_t         *frame     = NULL;
        quota_local_t        *local     = NULL;
        marker_conf_t        *priv      = NULL;
        quota_inode_ctx_t    *ctx       = NULL;
        inode_contribution_t *contri    = NULL;

        if (loc == NULL || this == NULL)
                return 0;

        priv = (marker_conf_t *) this->private;

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret < 0) {
                ctx = quota_inode_ctx_new (loc->inode, this);
                if (ctx == NULL) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "quota_inode_ctx_new failed");
                        ret = -1;
                        goto out;
                }
        }

        dict = dict_new ();
        if (!dict)
                goto out;

        if (loc->inode->ia_type == IA_IFDIR) {
                QUOTA_ALLOC_OR_GOTO (size, int64_t, ret, err);
                ret = dict_set_bin (dict, QUOTA_SIZE_KEY, size, 8);
                if (ret < 0)
                        goto free_size;
        }

        //if '/' then dont set contribution xattr
        if (strcmp (loc->path, "/") == 0)
                goto wind;

        contri = add_new_contribution_node (this, ctx, loc);
        if (contri == NULL)
                goto err;

        QUOTA_ALLOC_OR_GOTO (value, int64_t, ret, err);
        GET_CONTRI_KEY (key, loc->parent->gfid, ret);

        ret = dict_set_bin (dict, key, value, 8);
        if (ret < 0)
                goto free_value;

wind:
        frame = create_frame (this, this->ctx->pool);
        if (!frame) {
                ret = -1;
                goto err;
        }

        local = quota_local_new ();
        if (local == NULL)
                goto free_size;

        local->ctx = ctx;

        local->contri = contri;

        ret = loc_copy (&local->loc, loc);
        if (ret < 0)
                quota_local_unref (this, local);

        frame->local = local;

        STACK_WIND (frame, create_dirty_xattr, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->xattrop, &local->loc,
                    GF_XATTROP_ADD_ARRAY64, dict);
        ret = 0;

free_size:
        if (ret < 0)
                GF_FREE (size);

free_value:
        if (ret < 0)
                GF_FREE (value);

err:
        dict_unref (dict);

out:
        if (ret < 0)
                quota_inode_creation_done (NULL, NULL, this, -1, 0);

        return 0;
}


int32_t
get_parent_inode_local (xlator_t *this, quota_local_t *local)
{
        int32_t            ret;
        quota_inode_ctx_t *ctx = NULL;

        loc_wipe (&local->loc);

        loc_copy (&local->loc, &local->parent_loc);

        loc_wipe (&local->parent_loc);

        quota_inode_loc_fill (NULL, local->loc.parent, &local->parent_loc);

        ret = quota_inode_ctx_get (local->loc.inode, this, &ctx);
        if (ret < 0)
                return -1;

        local->ctx = ctx;

        local->contri = (inode_contribution_t *) ctx->contribution_head.next;

        return 0;
}


int32_t
xattr_updation_done (call_frame_t *frame,
                     void *cookie,
                     xlator_t *this,
                     int32_t op_ret,
                     int32_t op_errno,
                     dict_t *dict)
{
        QUOTA_STACK_DESTROY (frame, this);
        return 0;
}


int32_t
quota_inodelk_cbk (call_frame_t *frame, void *cookie,
                   xlator_t *this, int32_t op_ret, int32_t op_errno)
{
        int32_t         ret  = 0;
        quota_local_t  *local = NULL;

        local = frame->local;

        if (op_ret == -1 || local->err) {
                gf_log (this->name, GF_LOG_INFO, "lock setting failed");
                xattr_updation_done (frame, NULL, this, 0, 0, NULL);

                return 0;
        }

        gf_log (this->name, GF_LOG_DEBUG,
                "inodelk released on %s", local->parent_loc.path);

        if (strcmp (local->parent_loc.path, "/") == 0) {
                xattr_updation_done (frame, NULL, this, 0, 0, NULL);
        } else {
                ret = get_parent_inode_local (this, local);
                if (ret < 0) {
                        xattr_updation_done (frame, NULL, this, 0, 0, NULL);
                        goto out;
                }

                get_lock_on_parent (frame, this);
        }
out:
        return 0;
}


//now release lock on the parent inode
int32_t
quota_release_parent_lock (call_frame_t *frame, void *cookie,
                           xlator_t *this, int32_t op_ret,
                           int32_t op_errno)
{
        int32_t            ret      = 0;
        struct gf_flock    lock;
        quota_local_t     *local    = NULL;
        quota_inode_ctx_t *ctx      = NULL;

        trap ();

        local = frame->local;

        ret = quota_inode_ctx_get (local->parent_loc.inode, this, &ctx);
        if (ret < 0)
                goto wind;

        LOCK (&ctx->lock);
        {
                ctx->dirty = 0;
        }
        UNLOCK (&ctx->lock);

wind:
        lock.l_type   = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start  = 0;
        lock.l_len    = 0;
        lock.l_pid    = 0;

        STACK_WIND (frame,
                    quota_inodelk_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->parent_loc,
                    F_SETLKW, &lock);

        return 0;
}


int32_t
quota_mark_undirty (call_frame_t *frame,
                    void *cookie,
                    xlator_t *this,
                    int32_t op_ret,
                    int32_t op_errno,
                    dict_t *dict)
{
        int32_t            ret          = -1;
        int64_t           *size         = NULL;
        dict_t            *newdict      = NULL;
        quota_local_t     *local        = NULL;
        quota_inode_ctx_t *ctx          = NULL;
        marker_conf_t     *priv         = NULL;

        trap ();

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING, "%s occurred while"
                        " updating the size of %s", strerror (op_errno),
                        local->parent_loc.path);

                goto err;
        }

        priv = this->private;

       //update the size of the parent inode
        if (dict != NULL) {
                ret = quota_inode_ctx_get (local->parent_loc.inode, this, &ctx);
                if (ret < 0)
                        goto err;

                ret = dict_get_bin (dict, QUOTA_SIZE_KEY, (void **) &size);
                if (ret < 0)
                        goto err;

                LOCK (&ctx->lock);
                {
                        if (size)
                                ctx->size = ntoh64 (*size);
                        gf_log (this->name, GF_LOG_DEBUG, "%s %"PRId64,
                                local->parent_loc.path, ctx->size);
                }
                UNLOCK (&ctx->lock);
        }

        newdict = dict_new ();

        if (!newdict)
                goto err;

        ret = dict_set_int8 (newdict, QUOTA_DIRTY_KEY, 0);

        if (ret == -1)
                goto err;

        STACK_WIND (frame, quota_release_parent_lock,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr,
                    &local->parent_loc, newdict, 0);

        ret = 0;
err:
        if (op_ret == -1 || ret == -1) {
                local->err = 1;

                quota_release_parent_lock (frame, NULL, this, 0, 0);
        }

        if (newdict)
                dict_unref (newdict);

        return 0;
}


int32_t
quota_update_parent_size (call_frame_t *frame,
                          void *cookie,
                          xlator_t *this,
                          int32_t op_ret,
                          int32_t op_errno,
                          dict_t *dict)
{
        int64_t             *size       = NULL;
        int32_t              ret        = -1;
        dict_t              *newdict    = NULL;
        marker_conf_t       *priv       = NULL;
        quota_local_t       *local      = NULL;
        quota_inode_ctx_t   *ctx        = NULL;

        trap ();

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "%s", strerror (op_errno));

                goto err;
        }

        local->contri->contribution += local->delta;

        gf_log (this->name, GF_LOG_DEBUG, "%s %"PRId64 "%"PRId64,
                local->loc.path, local->ctx->size,
                local->contri->contribution);

        priv = this->private;

        if (dict == NULL)
                goto err;

        ret = quota_inode_ctx_get (local->parent_loc.inode, this, &ctx);
        if (ret < 0)
                goto err;

        newdict = dict_new ();
        if (!newdict) {
                ret = -1;
                goto err;
        }

        QUOTA_ALLOC_OR_GOTO (size, int64_t, ret, err);

        *size = ntoh64 (local->delta);

        ret = dict_set_bin (newdict, QUOTA_SIZE_KEY, size, 8);
        if (ret < 0)
                goto err;

        STACK_WIND (frame,
                    quota_mark_undirty,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->xattrop,
                    &local->parent_loc,
                    GF_XATTROP_ADD_ARRAY64,
                    newdict);
        ret = 0;
err:
        if (op_ret == -1 || ret < 0) {
                local->err = 1;
                quota_release_parent_lock (frame, NULL, this, 0, 0);
        }

        if (dict)
                dict_unref (newdict);

        return 0;
}

int32_t
quota_update_inode_contribution (call_frame_t *frame, void *cookie,
                                 xlator_t *this, int32_t op_ret,
                                 int32_t op_errno, inode_t *inode,
                                 struct iatt *buf, dict_t *dict,
                                 struct iatt *postparent)
{
        int32_t               ret             = -1;
        int64_t              *size            = NULL;
        int64_t              *contri          = NULL;
        int64_t              *delta           = NULL;
        char                  contri_key [512] = {0, };
        dict_t               *newdict         = NULL;
        quota_local_t        *local           = NULL;
        quota_inode_ctx_t    *ctx             = NULL;
        marker_conf_t        *priv            = NULL;
        inode_contribution_t *contribution    = NULL;

        trap ();

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR, "failed to get size and "
                        "contribution with %s error", strerror (op_errno));
                goto err;
        }

        priv = this->private;

        ctx = local->ctx;
        contribution = local->contri;

        //prepare to update size & contribution of the inode
        GET_CONTRI_KEY (contri_key, contribution->gfid, ret);
        if (ret == -1)
                goto err;

        LOCK (&ctx->lock);
        {
                if (local->loc.inode->ia_type == IA_IFDIR ) {
                        ret = dict_get_bin (dict, QUOTA_SIZE_KEY,
                                            (void **) &size);
                        if (ret < 0)
                                goto unlock;

                        ctx->size = ntoh64 (*size);
                } else
                        ctx->size = buf->ia_size;

                ret = dict_get_bin (dict, contri_key, (void **) &contri);
                if (ret < 0)
                        contribution->contribution = 0;
                else
                        contribution->contribution = ntoh64 (*contri);

                ret = 0;
        }
unlock:
        UNLOCK (&ctx->lock);

        if (ret < 0)
                goto err;

        gf_log (this->name, GF_LOG_DEBUG, "%s %"PRId64 "%"PRId64,
                local->loc.path, ctx->size, contribution->contribution);
        newdict = dict_new ();
        if (newdict == NULL) {
                ret = -1;
                goto err;
        }

        local->delta = ctx->size - contribution->contribution;

        QUOTA_ALLOC_OR_GOTO (delta, int64_t, ret, err);

        *delta = hton64 (local->delta);

        ret = dict_set_bin (newdict, contri_key, delta, 8);
        if (ret < 0) {
                ret = -1;
                goto err;
        }

        STACK_WIND (frame,
                    quota_update_parent_size,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->xattrop,
                    &local->loc,
                    GF_XATTROP_ADD_ARRAY64,
                    newdict);
        ret = 0;

err:
        if (op_ret == -1 || ret < 0) {
                local->err = 1;

                quota_release_parent_lock (frame, NULL, this, 0, 0);
        }

        if (newdict)
                dict_unref (newdict);

        return 0;
}

int32_t
quota_fetch_child_size_and_contri (call_frame_t *frame, void *cookie,
                                   xlator_t *this, int32_t op_ret,
                                   int32_t op_errno)
{
        int32_t            ret        = -1;
        char               contri_key [512] = {0, };
        dict_t            *newdict    = NULL;
        quota_local_t     *local      = NULL;
        marker_conf_t     *priv       = NULL;
        quota_inode_ctx_t *ctx        = NULL;

        trap ();

        local = frame->local;

        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "%s couldnt mark dirty", local->parent_loc.path);
                goto err;
        }

        VALIDATE_OR_GOTO (local->ctx, err);
        VALIDATE_OR_GOTO (local->contri, err);

        gf_log (this->name, GF_LOG_DEBUG, "%s marked dirty", local->parent_loc.path);

        priv = this->private;

        //update parent ctx
        ret = quota_inode_ctx_get (local->parent_loc.inode, this, &ctx);
        if (ret == -1)
                goto err;

        LOCK (&ctx->lock);
        {
                ctx->dirty = 1;
        }
        UNLOCK (&ctx->lock);

        newdict = dict_new ();
        if (newdict == NULL)
                goto err;

        if (local->loc.inode->ia_type == IA_IFDIR) {
                ret = dict_set_int64 (newdict, QUOTA_SIZE_KEY, 0);
        }

        GET_CONTRI_KEY (contri_key, local->contri->gfid, ret);
        if (ret < 0)
                goto err;

        ret = dict_set_int64 (newdict, contri_key, 0);

        STACK_WIND (frame, quota_update_inode_contribution, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->lookup, &local->loc, newdict);

        ret = 0;

err:
        if (op_ret == -1 || ret == -1) {
                local->err = 1;

                quota_release_parent_lock (frame, NULL, this, 0, 0);
        }

        if (newdict)
                dict_unref (newdict);

        return 0;
}

int32_t
quota_markdirty (call_frame_t *frame, void *cookie,
                 xlator_t *this, int32_t op_ret, int32_t op_errno)
{
        int32_t        ret      = -1;
        dict_t        *dict     = NULL;
        quota_local_t *local    = NULL;
        marker_conf_t *priv     = NULL;

        local = frame->local;

        if (op_ret == -1){
                gf_log (this->name, GF_LOG_ERROR,
                        "lock setting failed on %s",
                        local->parent_loc.path);

                local->err = 1;

                quota_inodelk_cbk (frame, NULL, this, 0, 0);

                return 0;
        }

        gf_log (this->name, GF_LOG_TRACE,
                "inodelk succeeded on  %s", local->parent_loc.path);

        priv = this->private;

        dict = dict_new ();
        if (!dict) {
                ret = -1;
                goto err;
        }

        ret = dict_set_int8 (dict, QUOTA_DIRTY_KEY, 1);
        if (ret == -1)
                goto err;

        STACK_WIND (frame, quota_fetch_child_size_and_contri,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setxattr,
                    &local->parent_loc, dict, 0);

        ret = 0;
err:
        if (ret == -1) {
                local->err = 1;

                quota_release_parent_lock (frame, NULL, this, 0, 0);
        }

        if (dict)
                dict_unref (dict);

        return 0;
}


int32_t
get_lock_on_parent (call_frame_t *frame, xlator_t *this)
{
        struct gf_flock  lock;
        quota_local_t   *local = NULL;

        GF_VALIDATE_OR_GOTO ("marker", frame, fr_destroy);

        local = frame->local;
        gf_log (this->name, GF_LOG_DEBUG, "taking lock on %s",
                local->parent_loc.path);

        lock.l_len    = 0;
        lock.l_start  = 0;
        lock.l_type   = F_WRLCK;
        lock.l_whence = SEEK_SET;

        STACK_WIND (frame,
                    quota_markdirty,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->parent_loc, F_SETLKW, &lock);

        return 0;

fr_destroy:
        QUOTA_STACK_DESTROY (frame, this);

        return 0;
}


int
start_quota_txn (xlator_t *this, loc_t *loc,
                 quota_inode_ctx_t *ctx,
                 inode_contribution_t *contri)
{
        int32_t          ret    = -1;
        call_frame_t    *frame  = NULL;
        quota_local_t   *local  = NULL;

        frame = create_frame (this, this->ctx->pool);
        if (frame == NULL)
                goto err;

        frame->root->lk_owner = cn++;

        local = quota_local_new ();
        if (local == NULL)
                goto fr_destroy;

        frame->local = local;

        ret = loc_copy (&local->loc, loc);
        if (ret < 0)
                goto fr_destroy;

        ret = quota_inode_loc_fill (NULL, local->loc.parent,
                                    &local->parent_loc);
        if (ret < 0)
                goto fr_destroy;

        local->ctx = ctx;
        local->contri = contri;

        get_lock_on_parent (frame, this);

        return 0;

fr_destroy:
        QUOTA_STACK_DESTROY (frame, this);

err:
        return -1;
}


int
initiate_quota_txn (xlator_t *this, loc_t *loc)
{
        int32_t            ret    = -1;
        quota_inode_ctx_t *ctx    = NULL;
        inode_contribution_t *contribution = NULL;

        trap ();

        VALIDATE_OR_GOTO (loc, out);

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "inode ctx get failed, aborting quota txn");
                ret = -1;
                goto out;
        }

        contribution = get_contribution_node (loc->parent, ctx);
        if (contribution == NULL)
                goto out;

        start_quota_txn (this, loc, ctx, contribution);
out:
        return 0;
}


int32_t
validate_inode_size_contribution (xlator_t *this,
                                  loc_t *loc,
                                  quota_inode_ctx_t *ctx,
                                  inode_contribution_t *contribution)
{
        if (ctx->size != contribution->contribution)
                initiate_quota_txn (this, loc);

        return 0;
}


int32_t
inspect_directory_xattr (xlator_t *this,
                        loc_t *loc,
                        dict_t *dict,
                        struct iatt buf)
{
        int32_t                  ret            = 0;
        int8_t                   dirty          = -1;
        int64_t                 *size           = NULL;
        int64_t                 *contri         = NULL;
        char                     contri_key [512] = {0, };
        marker_conf_t           *priv           = NULL;
        quota_inode_ctx_t       *ctx            = NULL;
        inode_contribution_t    *contribution   = NULL;

        priv = this->private;

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret < 0) {
                ctx = quota_inode_ctx_new (loc->inode, this);
                if (ctx == NULL) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "quota_inode_ctx_new failed");
                        ret = -1;
                        goto out;
                }
        }

        ret = dict_get_bin (dict, QUOTA_SIZE_KEY, (void **) &size);
        if (ret < 0)
                goto out;

        ret = dict_get_int8 (dict, QUOTA_DIRTY_KEY, &dirty);
        if (ret < 0)
                goto out;

        if (strcmp (loc->path, "/") != 0) {
                contribution = add_new_contribution_node (this, ctx, loc);
                if (contribution == NULL) {
                        gf_log (this->name, GF_LOG_DEBUG,
                                "cannot add a new contributio node");
                        goto out;
                }

                GET_CONTRI_KEY (contri_key, contribution->gfid, ret);
                if (ret < 0)
                        goto out;

                ret = dict_get_bin (dict, contri_key, (void **) &contri);
                if (ret < 0)
                        goto out;

                contribution->contribution = ntoh64 (*contri);
        }

        ctx->size = ntoh64 (*size);

        gf_log (this->name, GF_LOG_INFO, "size=%"PRId64
                "contri=%"PRId64, ctx->size,
                contribution?contribution->contribution:0);

        ctx->dirty = dirty;
        if (ctx->dirty == 1)
                update_dirty_inode (this, loc, ctx, contribution);

        ret = 0;
out:
        if (ret)
                quota_set_inode_xattr (this, loc);

        return 0;
}

int32_t
inspect_file_xattr (xlator_t *this,
                    loc_t *loc,
                    dict_t *dict,
                    struct iatt buf)
{
        int32_t               ret          = -1;
        uint64_t              contri_int   = 0;
        int64_t              *contri_ptr   = NULL;
        char                  contri_key [512] = {0, };
        marker_conf_t        *priv         = NULL;
        quota_inode_ctx_t    *ctx          = NULL;
        inode_contribution_t *contribution = NULL;

        priv = this->private;

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret < 0) {
                ctx = quota_inode_ctx_new (loc->inode, this);
                if (ctx == NULL) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "quota_inode_ctx_new failed");
                        ret = -1;
                        goto out;
                }
        }

        contribution = add_new_contribution_node (this, ctx, loc);
        if (contribution == NULL)
                goto out;

        LOCK (&ctx->lock);
        {
                ctx->size = buf.ia_size;
        }
        UNLOCK (&ctx->lock);

        list_for_each_entry (contribution, &ctx->contribution_head, contri_list) {
                GET_CONTRI_KEY (contri_key, contribution->gfid, ret);
                if (ret < 0)
                        continue;

                ret = dict_get_bin (dict, contri_key, (void **) &contri_int);
                if (ret == 0) {
                        contri_ptr = (int64_t *)(unsigned long)contri_int;

                        contribution->contribution = ntoh64 (*contri_ptr);

                        gf_log (this->name, GF_LOG_DEBUG,
                                "size=%"PRId64 " contri=%"PRId64, ctx->size,
                                contribution->contribution);

                        ret = validate_inode_size_contribution
                                (this, loc, ctx, contribution);
                } else
                      initiate_quota_txn (this, loc);
        }

out:
        return ret;
}

int32_t
quota_xattr_state (xlator_t *this,
                   loc_t *loc,
                   dict_t *dict,
                   struct iatt buf)
{
        if (buf.ia_type == IA_IFREG ||
            buf.ia_type == IA_IFLNK) {
                k ++;
                inspect_file_xattr (this, loc, dict, buf);
        } else if (buf.ia_type == IA_IFDIR)
                inspect_directory_xattr (this, loc, dict, buf);

        return 0;
}

int32_t
quota_req_xattr (xlator_t *this,
                 loc_t *loc,
                 dict_t *dict)
{
        int32_t               ret       = -1;
        marker_conf_t        *priv      = NULL;

        GF_VALIDATE_OR_GOTO ("marker", this, out);
        GF_VALIDATE_OR_GOTO ("marker", loc, out);
        GF_VALIDATE_OR_GOTO ("marker", dict, out);

        priv = this->private;

        //if not "/" then request contribution
        if (strcmp (loc->path, "/") == 0)
                goto set_size;

        ret = dict_set_contribution (this, dict, loc);
        if (ret == -1)
                goto out;

set_size:
        ret = dict_set_uint64 (dict, QUOTA_SIZE_KEY, 0);
        if (ret < 0) {
                ret = -1;
                goto out;
        }

        ret = dict_set_int8 (dict, QUOTA_DIRTY_KEY, 0);
        if (ret < 0) {
                ret = -1;
                goto out;
        }

        ret = 0;

out:
        return ret;
}


int32_t
quota_removexattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                       int32_t op_ret, int32_t op_errno)
{
        QUOTA_STACK_DESTROY (frame, this);

        return 0;
}

int32_t
quota_inode_remove_done (call_frame_t *frame, void *cookie, xlator_t *this,
                         int32_t op_ret, int32_t op_errno)
{
        int32_t          ret        = 0;
        char             contri_key [512] = {0, };
        quota_local_t   *local      = NULL;

        local = (quota_local_t *) frame->local;

        if (op_ret == -1 || local->err == -1) {
                quota_removexattr_cbk (frame, NULL, this, -1, 0);
                return 0;
        }

        if (local->hl_count > 1) {
                GET_CONTRI_KEY (contri_key, local->contri->gfid, ret);

                STACK_WIND (frame, quota_removexattr_cbk, FIRST_CHILD(this),
                           FIRST_CHILD(this)->fops->removexattr,
                           &local->loc, contri_key);
                ret = 0;
        }

        if (strcmp (local->parent_loc.path, "/") != 0) {
                get_parent_inode_local (this, local);

                start_quota_txn (this, &local->loc, local->ctx, local->contri);
        }

        quota_local_unref (this, local);

        return 0;
}

int32_t
mq_inode_remove_done (call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        int32_t ret;
        struct gf_flock lock;
        quota_inode_ctx_t *ctx;
        quota_local_t  *local = NULL;

        local = frame->local;
        if (op_ret == -1)
                local->err = -1;

        ret = quota_inode_ctx_get (local->parent_loc.inode, this, &ctx);
        if (ret == 0)
                ctx->size -= local->contri->contribution;

        local->contri->contribution = 0;

        lock.l_type   = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start  = 0;
        lock.l_len    = 0;
        lock.l_pid    = 0;

        STACK_WIND (frame,
                    quota_inode_remove_done,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->parent_loc,
                    F_SETLKW, &lock);
        return 0;
}

static int32_t
mq_reduce_parent_size_xattr (call_frame_t *frame, void *cookie,
                             xlator_t *this, int32_t op_ret, int32_t op_errno)
{
        int32_t                  ret               = -1;
        int64_t                 *size              = NULL;
        dict_t                  *dict              = NULL;
        marker_conf_t           *priv              = NULL;
        quota_local_t           *local             = NULL;
        inode_contribution_t    *contribution      = NULL;

        local = frame->local;
        if (op_ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "inodelk set failed on %s", local->parent_loc.path);
                QUOTA_STACK_DESTROY (frame, this);
                return 0;
        }

        VALIDATE_OR_GOTO (local->contri, err);

        priv = this->private;

        contribution = local->contri;

        dict = dict_new ();
        if (dict == NULL) {
                ret = -1;
                goto err;
        }

        QUOTA_ALLOC_OR_GOTO (size, int64_t, ret, err);

        *size = hton64 (-contribution->contribution);


        ret = dict_set_bin (dict, QUOTA_SIZE_KEY, size, 8);
        if (ret < 0)
                goto err;


        STACK_WIND (frame, mq_inode_remove_done, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->xattrop, &local->parent_loc,
                    GF_XATTROP_ADD_ARRAY64, dict);
        return 0;

err:
        local->err = 1;
        mq_inode_remove_done (frame, NULL, this, -1, 0, NULL);
        return 0;
}

int32_t
reduce_parent_size (xlator_t *this, loc_t *loc)
{
        int32_t                  ret           = -1;
        struct gf_flock          lock          = {0,};
        call_frame_t            *frame         = NULL;
        marker_conf_t           *priv          = NULL;
        quota_local_t           *local         = NULL;
        quota_inode_ctx_t       *ctx           = NULL;
        inode_contribution_t    *contribution  = NULL;

        GF_VALIDATE_OR_GOTO ("marker", this, out);
        GF_VALIDATE_OR_GOTO ("marker", loc, out);

        priv = this->private;

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret < 0)
                goto out;

        contribution = get_contribution_node (loc->parent, ctx);
        if (contribution == NULL)
                goto out;

        local = quota_local_new ();
        if (local == NULL) {
                ret = -1;
                goto out;
        }

        ret = loc_copy (&local->loc, loc);
        if (ret < 0)
                goto out;

        local->ctx = ctx;
        local->contri = contribution;

        ret = quota_inode_loc_fill (NULL, loc->parent, &local->parent_loc);
        if (ret < 0)
                goto out;

        frame = create_frame (this, this->ctx->pool);
        if (!frame) {
                ret = -1;
                goto out;
        }

        frame->root->lk_owner = cn++;
        frame->local = local;

        lock.l_len    = 0;
        lock.l_start  = 0;
        lock.l_type   = F_WRLCK;
        lock.l_whence = SEEK_SET;

        STACK_WIND (frame,
                    mq_reduce_parent_size_xattr,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->inodelk,
                    this->name, &local->parent_loc, F_SETLKW, &lock);
        ret = 0;

out:
        return ret;
}


int32_t
init_quota_priv (xlator_t *this)
{
        strcpy (volname, "quota");

        return 0;
}


int32_t
quota_rename_update_newpath (xlator_t *this, loc_t *loc, inode_t *inode)
{
        int32_t               ret          = -1;
        quota_inode_ctx_t    *ctx          = NULL;
        inode_contribution_t *contribution = NULL;

        GF_VALIDATE_OR_GOTO ("marker", this, out);
        GF_VALIDATE_OR_GOTO ("marker", loc, out);
        GF_VALIDATE_OR_GOTO ("marker", inode, out);

        if (loc->inode == NULL)
                loc->inode = inode_ref (inode);

        ret = quota_inode_ctx_get (loc->inode, this, &ctx);
        if (ret < 0)
                goto out;

        contribution = add_new_contribution_node (this, ctx, loc);
        if (contribution == NULL) {
                ret = -1;
                goto out;
        }

        initiate_quota_txn (this, loc);
out:
        return ret;
}

int32_t
quota_forget (xlator_t *this, quota_inode_ctx_t *ctx)
{
        inode_contribution_t *contri = NULL;
        inode_contribution_t *next   = NULL;

        GF_VALIDATE_OR_GOTO ("marker", this, out);
        GF_VALIDATE_OR_GOTO ("marker", ctx, out);

        list_for_each_entry_safe (contri, next, &ctx->contribution_head,
                                  contri_list) {
                list_del (&contri->contri_list);
                GF_FREE (contri);
        }

        LOCK_DESTROY (&ctx->lock);
        GF_FREE (ctx);
out:
        return 0;
}
