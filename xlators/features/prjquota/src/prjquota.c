/*
   Copyright (c) 2019 Kadalu.IO <https://kadalu.io>
   This file is part of GlusterFS.

   This file is licensed to you under your choice of the GNU Lesser
   General Public License, version 3 or any later version (LGPLv3 or
   later), or the GNU General Public License, version 2 (GPLv2), in all
   cases as published by the Free Software Foundation.
*/

#include <glusterfs/atomic.h>
#include <glusterfs/xlator.h>
#include <glusterfs/dict.h>
#include <glusterfs/list.h>
#include <glusterfs/mem-types.h>

#include "prjquota.h"

uint64_t prjq_blk_size = 0;

static inline prjq_ctx_t *
prjq_ctx_ref(prjq_ctx_t *pctx)
{
    GF_ATOMIC_INC(pctx->ref);
    return pctx;
}

static inline prjq_ctx_t *
prjq_ctx_unref(prjq_ctx_t *pctx)
{
    prjq_ctx_t *npctx = pctx;
    uint64_t val = GF_ATOMIC_DEC(pctx->ref);

    if (!val) {
        prjq_priv_t *priv = THIS->private;
        LOCK(&priv->lock);
        {
            list_del(&pctx->list);
        }
        UNLOCK(&priv->lock);

        GF_FREE(pctx->path);
        GF_FREE(pctx);
        npctx = NULL;
    }

    return npctx;
}

static inline void
prjq_ctx_inc_size(prjq_ctx_t *pctx, uint64_t size)
{
    GF_ATOMIC_ADD(pctx->used, size);
}

static inline void
prjq_ctx_dec_size(prjq_ctx_t *pctx, uint64_t size)
{
    GF_ATOMIC_SUB(pctx->used, size);
}

static inline prjq_ctx_t *
prjq_ctx_new(xlator_t *this, uuid_t gfid, const char *path, uint64_t limit,
             uint64_t used)
{
    prjq_priv_t *priv = this->private;
    prjq_ctx_t *p = GF_CALLOC(1, sizeof(prjq_ctx_t), prjq_mt_ctx_t);
    if (!p)
        return NULL;

    GF_ATOMIC_INIT(p->used, used);
    p->limit = limit;
    p->path = gf_strdup(path);
    gf_uuid_copy(p->gfid, gfid);

    INIT_LIST_HEAD(&p->list);
    LOCK(&priv->lock);
    {
        list_add_tail(&priv->qlist, &p->list);
    }
    UNLOCK(&priv->lock);

    return p;
}

static inline prjq_ctx_t *
prjq_inode_set(xlator_t *this, inode_t *inode, void *value)
{
    prjq_ctx_t *pc = (prjq_ctx_t *)value;
    prjq_ctx_ref(pc);
    inode_ctx_put(inode, this, (unsigned long)pc);
    return pc;
}

int
prjquota_mkdir_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, inode_t *inode,
                   struct iatt *stbuf, struct iatt *preparent,
                   struct iatt *postparent, dict_t *xdata)
{
    if ((op_ret >= 0) && frame->local) {
        /* If successful, then set inode ctx to same as its parent. */
        prjq_ctx_t *pc = prjq_inode_set(this, inode, frame->local);
        prjq_ctx_inc_size(pc, 1 * prjq_blk_size);
    }
    frame->local = NULL;

    STACK_UNWIND_STRICT(mkdir, frame, op_ret, op_errno, inode, stbuf, preparent,
                        postparent, xdata);
    return 0;
}

int
prjquota_mkdir(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               mode_t umask, dict_t *xdata)
{
    uint64_t value;
    int ret = inode_ctx_get(loc->parent, this, &value);
    if (!ret) {
        frame->local = (void *)(unsigned long)value;
    }

    STACK_WIND(frame, prjquota_mkdir_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->mkdir, loc, mode, umask, xdata);

    return 0;
}

int
prjquota_rmdir_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                   struct iatt *postparent, dict_t *xdata)
{
    if (op_ret >= 0) {
        inode_t *inode = cookie;
        uint64_t value;
        int ret = inode_ctx_get(inode, this, &value);
        if (!ret) {
            prjq_ctx_dec_size((prjq_ctx_t *)(unsigned long)value,
                              1 * prjq_blk_size);
        }
    }
    STACK_UNWIND_STRICT(rmdir, frame, op_ret, op_errno, preparent, postparent,
                        xdata);
    return 0;
}

int
prjquota_rmdir(call_frame_t *frame, xlator_t *this, loc_t *loc, int flags,
               dict_t *xdata)
{
    STACK_WIND_COOKIE(frame, prjquota_rmdir_cbk, loc->inode, FIRST_CHILD(this),
                      FIRST_CHILD(this)->fops->rmdir, loc, flags, xdata);

    return 0;
}

int
prjquota_create_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, fd_t *fd, inode_t *inode,
                    struct iatt *stbuf, struct iatt *preparent,
                    struct iatt *postparent, dict_t *xdata)
{
    if ((op_ret >= 0) && frame->local) {
        /* If successful, then set inode ctx to same as its parent. */
        prjq_ctx_t *pc = prjq_inode_set(this, inode, frame->local);
        prjq_ctx_inc_size(pc, 1 * prjq_blk_size);
    }
    frame->local = NULL;

    STACK_UNWIND_STRICT(create, frame, op_ret, op_errno, fd, inode, stbuf,
                        preparent, postparent, xdata);
    return 0;
}

int
prjquota_create(call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
                mode_t mode, mode_t umask, fd_t *fd, dict_t *xdata)
{
    uint64_t value;
    int ret = inode_ctx_get(loc->parent, this, &value);
    if (!ret) {
        frame->local = (void *)(unsigned long)value;
    }
    STACK_WIND(frame, prjquota_create_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->create, loc, flags, mode, umask, fd,
               xdata);

    return 0;
}

int
prjquota_unlink_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                    struct iatt *postparent, dict_t *xdata)
{
    if (op_ret >= 0) {
        inode_t *inode = cookie;
        uint64_t value;
        int ret = inode_ctx_get(inode, this, &value);
        if (!ret) {
            /* TODO: get the exact size */
            prjq_ctx_dec_size((prjq_ctx_t *)(unsigned long)value,
                              1 * prjq_blk_size);
        }
    }

    STACK_UNWIND_STRICT(unlink, frame, op_ret, op_errno, preparent, postparent,
                        xdata);
    return 0;
}

int
prjquota_unlink(call_frame_t *frame, xlator_t *this, loc_t *loc, int flags,
                dict_t *xdata)
{
    /* Do we have size saved somewhere? */
    STACK_WIND_COOKIE(frame, prjquota_unlink_cbk, loc->inode, FIRST_CHILD(this),
                      FIRST_CHILD(this)->fops->unlink, loc, flags, xdata);

    return 0;
}

int
prjquota_symlink_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno, inode_t *inode,
                     struct iatt *stbuf, struct iatt *preparent,
                     struct iatt *postparent, dict_t *xdata)
{
    /* set parent ctx in inode */
    if ((op_ret >= 0) && frame->local) {
        /* If successful, then set inode ctx to same as its parent. */
        prjq_ctx_t *pc = prjq_inode_set(this, inode, frame->local);
        prjq_ctx_inc_size(pc, 1 * prjq_blk_size);
    }
    frame->local = NULL;

    STACK_UNWIND_STRICT(symlink, frame, op_ret, op_errno, inode, stbuf,
                        preparent, postparent, xdata);
    return 0;
}

int
prjquota_symlink(call_frame_t *frame, xlator_t *this, const char *linkname,
                 loc_t *loc, mode_t umask, dict_t *xdata)
{
    uint64_t value;
    int ret = inode_ctx_get(loc->parent, this, &value);
    if (!ret) {
        frame->local = (void *)(unsigned long)value;
    }
    STACK_WIND(frame, prjquota_symlink_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->symlink, linkname, loc, umask, xdata);

    return 0;
}

int
prjquota_link_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, inode_t *inode,
                  struct iatt *stbuf, struct iatt *preparent,
                  struct iatt *postparent, dict_t *xdata)
{
    STACK_UNWIND_STRICT(link, frame, op_ret, op_errno, inode, stbuf, preparent,
                        postparent, xdata);
    return 0;
}

int
prjquota_link(call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc,
              dict_t *xdata)
{
    /* TODO: don't allow linking of entries belonging to different prjquota
     * setup */
    STACK_WIND(frame, prjquota_link_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->link, oldloc, newloc, xdata);

    return 0;
}

int
prjquota_mknod_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, inode_t *inode,
                   struct iatt *stbuf, struct iatt *preparent,
                   struct iatt *postparent, dict_t *xdata)
{
    if ((op_ret >= 0) && frame->local) {
        /* If successful, then set inode ctx to same as its parent. */
        prjq_ctx_t *pc = prjq_inode_set(this, inode, frame->local);
        prjq_ctx_inc_size(pc, 1 * prjq_blk_size);
    }
    frame->local = NULL;

    STACK_UNWIND_STRICT(mknod, frame, op_ret, op_errno, inode, stbuf, preparent,
                        postparent, xdata);
    return 0;
}

int
prjquota_mknod(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               dev_t rdev, mode_t umask, dict_t *xdata)
{
    uint64_t value;
    int ret = inode_ctx_get(loc->parent, this, &value);
    if (!ret) {
        frame->local = (void *)(unsigned long)value;
    }
    STACK_WIND(frame, prjquota_mknod_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->mknod, loc, mode, rdev, umask, xdata);

    return 0;
}

int
prjquota_rename_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *stbuf,
                    struct iatt *preoldparent, struct iatt *postoldparent,
                    struct iatt *prenewparent, struct iatt *postnewparent,
                    dict_t *xdata)
{
    STACK_UNWIND_STRICT(rename, frame, op_ret, op_errno, stbuf, preoldparent,
                        postoldparent, prenewparent, postnewparent, xdata);

    return 0;
}

int
prjquota_rename(call_frame_t *frame, xlator_t *this, loc_t *oldloc,
                loc_t *newloc, dict_t *xdata)
{
    /* TODO: don't allow linking of entries belonging to different prjquota
     * setup */
    STACK_WIND(frame, prjquota_rename_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->rename, oldloc, newloc, xdata);
    return 0;
}

int
prjquota_lookup_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, inode_t *inode,
                    struct iatt *stbuf, dict_t *xdata, struct iatt *postparent)
{
    prjq_ctx_t *pc = NULL;
    uint64_t limit = 0;
    int ret;
    char *path = cookie;

    /* This is where we need to have proper xattr check and setting in ctx, and
     * other things */
    /* TODO: most works gets here */
    if (op_ret == 0) {
        if (!prjq_blk_size) {
            /* Only 1 time for the run time of process, set quota on root */
            prjq_blk_size = stbuf->ia_blksize;
            pc = prjq_ctx_new(this, stbuf->ia_gfid, path, 0, 0);
            prjq_inode_set(this, inode, (void *)pc);
        }

        uint64_t value = 0;
        inode_ctx_get(inode, this, &value);
        ret = dict_get_uint64(xdata, PRJQ_LIMIT_XATTR_KEY, &limit);
        if ((ret || !limit) && frame->local && !value) {
            /* If successful, then set inode ctx to same as its parent. */
            pc = prjq_inode_set(this, inode, frame->local);
            prjq_ctx_inc_size(pc, stbuf->ia_blocks * 512);
            goto unwind;
        }
        if (limit) {
            /* inode may already have an entry, check if it is for same inode,
             * and if yes, then update the limit*/
            if (!value) {
                pc = prjq_ctx_new(this, stbuf->ia_gfid, path, limit, 0);
                prjq_inode_set(this, inode, pc);
                goto unwind;
            }
            pc = (prjq_ctx_t *)(unsigned long)value;
            /* If current context is for different gfid */
            if (gf_uuid_compare(pc->gfid, stbuf->ia_gfid)) {
                /* set new context, and unref current */
                pc = prjq_ctx_new(this, stbuf->ia_gfid, path, limit, 0);
                prjq_inode_set(this, inode, pc);
                prjq_ctx_unref((prjq_ctx_t *)(unsigned long)value);
                goto unwind;
            }
            if (pc->limit != limit) {
                pc->limit = limit;
            }
        }
    }

unwind:
    frame->local = NULL;

    STACK_UNWIND_STRICT(lookup, frame, op_ret, op_errno, inode, stbuf, xdata,
                        postparent);
    return 0;
}

int
prjquota_lookup(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
    uint64_t value = 0;
    if (!loc->parent) {
        /* this is root lookup */
        goto wind;
    }

    inode_ctx_get(loc->parent, this, &value);
    if (value) {
        frame->local = (void *)(unsigned long)value;
    }

wind:
    gf_log("", GF_LOG_INFO, "%s: %p %lu", loc->path, frame->local, value);

    /* TODO: handle no memory */
    if (!xdata)
        xdata = dict_new();
    else
        dict_ref(xdata);

    int ret = dict_set_uint64(xdata, PRJQ_LIMIT_XATTR_KEY, 0);
    if (ret)
        gf_log(this->name, GF_LOG_WARNING, "%s: failed to set limit key",
               loc->path);

    STACK_WIND_COOKIE(frame, prjquota_lookup_cbk, loc->path, FIRST_CHILD(this),
                      FIRST_CHILD(this)->fops->lookup, loc, xdata);

    dict_unref(xdata);
    return 0;
}

int32_t
prjquota_readdirp_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                      int32_t op_ret, int32_t op_errno, gf_dirent_t *entries,
                      dict_t *xdata)
{
    /* TODO: do we need to bother about setting prjquota ctx ? If not, then we
     * can skip this fop itself */
    STACK_UNWIND_STRICT(readdirp, frame, op_ret, op_errno, entries, xdata);
    return 0;
}

int32_t
prjquota_readdirp(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
                  off_t off, dict_t *xdata)
{
    STACK_WIND(frame, prjquota_readdirp_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->readdirp, fd, size, off, xdata);
    return 0;
}

static int
prjquota_setxattr_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                      int op_ret, int op_errno, dict_t *xdata)
{
    uint64_t limit = (uint64_t)(unsigned long)frame->local;
    frame->local = NULL;

    /* Upon success, set the inode ctx */
    if (!op_ret && limit) {
        inode_t *inode = cookie;
        uint64_t value = 0;
        inode_ctx_get(inode, this, &value);
        prjq_ctx_t *pc = (prjq_ctx_t *)(unsigned long)value;
        if (value && !gf_uuid_compare(pc->gfid, inode->gfid)) {
            gf_log(this->name, GF_LOG_INFO,
                   "Updating Quota for GFID(%s) from %" PRIu64 " -> %" PRIu64,
                   uuid_utoa(pc->gfid), pc->limit, limit);
            pc->limit = limit;
            goto unwind;
        }
        /* TODO: figure out an way to get path here */
        pc = prjq_ctx_new(this, inode->gfid, "path", limit, 0);
        prjq_ctx_ref(pc);
        prjq_inode_set(this, inode, pc);
    }
unwind:
    STACK_UNWIND_STRICT(setxattr, frame, op_ret, op_errno, xdata);
    return 0;
}

static int
prjquota_setxattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
                  dict_t *xattr, int flags, dict_t *xdata)
{
    /* TODO:
       1. provide a virtual key for deleting quota.
     */
    uint64_t limit = 0;
    int op_errno = 0;
    int ret = dict_get_uint64(xattr, PRJQ_LIMIT_VIRT_XATTR_KEY, &limit);
    if (ret)
        goto wind;

    if (!IA_ISDIR(loc->inode->ia_type) || (limit == 0)) {
        op_errno = ENOTSUP;
        goto err;
    }

    frame->local = (void *)(unsigned long)limit;

    dict_del(xattr, PRJQ_LIMIT_VIRT_XATTR_KEY);
    ret = dict_set_uint64(xattr, PRJQ_LIMIT_XATTR_KEY, limit);
    if (ret) {
        gf_log(this->name, GF_LOG_WARNING, "%s: Failed to set the xattr key",
               loc->path);
        op_errno = EINVAL;
        goto err;
    }
wind:
    STACK_WIND_COOKIE(frame, prjquota_setxattr_cbk, loc->inode,
                      FIRST_CHILD(this), FIRST_CHILD(this)->fops->setxattr, loc,
                      xattr, flags, xdata);
    return 0;
err:
    STACK_UNWIND_STRICT(setxattr, frame, -1, op_errno, xdata);
    return 0;
}

int32_t
prjquota_statfs_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct statvfs *buf,
                    dict_t *xdata)
{
    inode_t *inode = cookie;

    /* This fop will fail mostly in case of client disconnect,
     * which is already logged. Hence, not logging here */
    if (op_ret == -1)
        goto unwind;

    uint64_t value = 0;
    inode_ctx_get(inode, this, &value);
    prjq_ctx_t *pc = (prjq_ctx_t *)(unsigned long)value;

    if (pc && pc->limit) {
        /* statfs is adjusted in this code block */
        uint64_t blocks = pc->limit / buf->f_bsize;
        uint64_t usage = GF_ATOMIC_GET(pc->used) / buf->f_bsize;
        uint64_t avail = blocks - usage;

        buf->f_blocks = blocks;
        buf->f_bfree = max(avail, 0);

        /*
         * We have to assume that the total assigned quota
         * won't cause us to dip into the reserved space,
         * because dealing with the overcommitted cases is
         * just too hairy (especially when different bricks
         * might be using different reserved percentages and
         * such).
         */
        buf->f_bavail = buf->f_bfree;
    }

unwind:
    STACK_UNWIND(statfs, frame, op_ret, op_errno, buf, xdata);

    return 0;
}

int32_t
prjquota_statfs(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
    STACK_WIND_COOKIE(frame, prjquota_statfs_cbk, loc->inode, FIRST_CHILD(this),
                      FIRST_CHILD(this)->fops->statfs, loc, xdata);
    return 0;
}

static int32_t
prjquota_writev_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int op_ret, int op_errno, struct iatt *prebuf,
                    struct iatt *postbuf, dict_t *xdata)
{
    inode_t *inode = cookie;
    uint64_t value = 0;
    inode_ctx_get(inode, this, &value);
    prjq_ctx_t *pc = (prjq_ctx_t *)(unsigned long)value;
    if (pc) {
        prjq_ctx_inc_size(pc, (postbuf->ia_blocks - prebuf->ia_blocks) * 512);
    }

    STACK_UNWIND_STRICT(writev, frame, op_ret, op_errno, prebuf, postbuf,
                        xdata);

    return 0;
}

int32_t
prjquota_writev(call_frame_t *frame, xlator_t *this, fd_t *fd,
                struct iovec *vector, int32_t count, off_t off, uint32_t flags,
                struct iobref *iobref, dict_t *xdata)
{
    STACK_WIND_COOKIE(frame, prjquota_writev_cbk, fd->inode, FIRST_CHILD(this),
                      FIRST_CHILD(this)->fops->writev, fd, vector, count, off,
                      flags, iobref, xdata);
    return 0;
}

static int32_t
prjquota_truncate_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                      int op_ret, int op_errno, struct iatt *prebuf,
                      struct iatt *postbuf, dict_t *xdata)
{
    inode_t *inode = cookie;
    uint64_t value = 0;
    if (op_ret == 0) {
        inode_ctx_get(inode, this, &value);
        prjq_ctx_t *pc = (prjq_ctx_t *)(unsigned long)value;
        uint64_t size = (prebuf->ia_blocks - postbuf->ia_blocks) * 512;
        if (pc)
            prjq_ctx_dec_size(pc, size);
        else
            gf_log("", GF_LOG_INFO, "%p %" PRIu64, inode, size);
    }

    STACK_UNWIND_STRICT(truncate, frame, op_ret, op_errno, prebuf, postbuf,
                        xdata);

    return 0;
}

static int32_t
prjquota_truncate(call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
                  dict_t *xdata)
{
    STACK_WIND_COOKIE(frame, prjquota_truncate_cbk, loc->inode,
                      FIRST_CHILD(this), FIRST_CHILD(this)->fops->truncate, loc,
                      offset, xdata);

    return 0;
}

static int32_t
prjquota_ftruncate(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                   dict_t *xdata)
{
    STACK_WIND_COOKIE(frame, prjquota_truncate_cbk, fd->inode,
                      FIRST_CHILD(this), FIRST_CHILD(this)->fops->ftruncate, fd,
                      offset, xdata);

    return 0;
}

int32_t
prjquota_forget(xlator_t *this, inode_t *inode)
{
    uint64_t value;
    int ret = inode_ctx_get(inode, this, &value);
    if (!ret) {
        prjq_ctx_unref((prjq_ctx_t *)(unsigned long)value);
    }
    return 0;
}

int32_t
mem_acct_init(xlator_t *this)
{
    int ret = -1;

    ret = xlator_mem_acct_init(this, prjq_mt_end + 1);

    return ret;
}

int
init(xlator_t *this)
{
    int ret = -1;

    if (!this->children || this->children->next) {
        gf_log(this->name, GF_LOG_ERROR,
               "'project quota' not configured with exactly one child");
        goto out;
    }

    if (!this->parents) {
        gf_log(this->name, GF_LOG_WARNING, "dangling volume. check volfile ");
    }

    GF_OPTION_INIT("pass-through", this->pass_through, bool, out);

    prjq_priv_t *priv = GF_CALLOC(1, sizeof(prjq_priv_t), prjq_mt_priv_t);
    if (!priv)
        gf_log(this->name, GF_LOG_ERROR, "failed to allocate memory");

    LOCK_INIT(&priv->lock);
    INIT_LIST_HEAD(&priv->qlist);

    this->private = priv;
    ret = 0;

out:
    return ret;
}

int
reconfigure(xlator_t *this, dict_t *options)
{
    int ret = -1;

    GF_OPTION_RECONF("pass-through", this->pass_through, options, bool, out);

    ret = 0;
out:
    return ret;
}

void
fini(xlator_t *this)
{
    prjq_priv_t *priv = this->private;
    this->private = NULL;
    /* sync the config file */
    GF_FREE(priv);
    return;
}

struct xlator_fops fops = {
    .mkdir = prjquota_mkdir,
    .rmdir = prjquota_rmdir,
    .create = prjquota_create,
    .unlink = prjquota_unlink,
    .symlink = prjquota_symlink,
    .link = prjquota_link,
    .mknod = prjquota_mknod,
    .rename = prjquota_rename,
    .lookup = prjquota_lookup,
    .setxattr = prjquota_setxattr,
    .readdirp = prjquota_readdirp,
    .statfs = prjquota_statfs,
    .writev = prjquota_writev,
    .truncate = prjquota_truncate,
    .ftruncate = prjquota_ftruncate,

    /* TODO: handle below: */
    //.fallocate
    //.zerofill
};

struct xlator_cbks cbks = {
    .forget = prjquota_forget,
};

struct volume_options options[] = {
    {.key = {"pass-through"},
     .type = GF_OPTION_TYPE_BOOL,
     .default_value = "false",
     .op_version = {GD_OP_VERSION_7_0},
     .flags = OPT_FLAG_SETTABLE | OPT_FLAG_DOC,
     .tags = {"prjquota"},
     .description = "Enable/Disable project quota functionality"},
    {.key = {"config-file"},
     .type = GF_OPTION_TYPE_PATH,
     .default_value = "/kadalu/quota",
     .op_version = {GD_OP_VERSION_7_0},
     .flags = OPT_FLAG_SETTABLE | OPT_FLAG_DOC,
     .tags = {"prjquota"},
     .description = "config-file to store the quota setting etc"},
    {.key = {NULL}},
};

xlator_api_t xlator_api = {
    .init = init,
    .fini = fini,
    .mem_acct_init = mem_acct_init,
    .reconfigure = reconfigure,
    .op_version = {GD_OP_VERSION_7_0},
    .fops = &fops,
    .cbks = &cbks,
    .options = options,
    .identifier = "project quota",
    .category = GF_TECH_PREVIEW,
};
