/*
 * Copyright (c) 2019 Kadalu Software. <http://kadalu.io>
 * This file is part of GlusterFS.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 3 or any later version (LGPLv3 or
 * later), or the GNU General Public License, version 2 (GPLv2), in all
 * cases as published by the Free Software Foundation.
 *
 * xlators/features/dsmds:
 *    This translator separates fops into meta-data and data classification,
 *    and sends the operations to relevant subvolume
 */

#include <sys/types.h>

#include <glusterfs/defaults.h>
#include <glusterfs/hashfn.h>
#include <glusterfs/logging.h>
#include "dsmds.h"

/* helper functions */
static void
dsmds_local_wipe(xlator_t *this, dsmds_local_t *local)
{
  if (!local)
    goto out;

  dict_unref(local->xdata_req);
  dict_unref(local->xdata_rsp);
  loc_wipe(&local->orig_loc);
  loc_wipe(&local->ds_loc);
  if(local->inode)
    inode_unref(local->inode);
  GF_FREE(local);

 out:
  return;
}

static void
build_ds_loc(inode_table_t *itable, uuid_t gfid, dsmds_local_t *l)
{
  loc_t *loc = &l->ds_loc;
  loc->name = uuid_utoa_r(gfid, &l->name);
  gf_uuid_copy(loc->gfid, gfid);
  loc->parent = inode_ref(itable->root);
}

/* ----------------------- */
int32_t
dsmds_entrylk(call_frame_t *frame, xlator_t *this, const char *volume,
              loc_t *loc, const char *basename, entrylk_cmd cmd,
              entrylk_type type, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_entrylk_cbk, priv->mds, priv->mds->fops->entrylk,
               volume, loc, basename, cmd, type, xdata);
    return 0;
}

int32_t
dsmds_fentrylk(call_frame_t *frame, xlator_t *this, const char *volume,
               fd_t *fd, const char *basename, entrylk_cmd cmd,
               entrylk_type type, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fentrylk_cbk, priv->mds,
               priv->mds->fops->fentrylk, volume, fd, basename, cmd, type,
               xdata);
    return 0;
}

int32_t
dsmds_rmdir(call_frame_t *frame, xlator_t *this, loc_t *loc, int xflags,
            dict_t *xdata)
{
    dsmds_private_t *priv = this->private;

    STACK_WIND(frame, default_rmdir_cbk, priv->mds, priv->mds->fops->rmdir, loc,
               xflags, xdata);
    return 0;
}

int32_t
dsmds_rename(call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc,
             dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_rename_cbk, priv->mds, priv->mds->fops->rename,
               oldloc, newloc, xdata);
    return 0;
}

int32_t
dsmds_link(call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc,
           dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_link_cbk, priv->mds, priv->mds->fops->link,
               oldloc, newloc, xdata);
    return 0;
}

int32_t
dsmds_mkdir(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
            mode_t umask, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_mkdir_cbk, priv->mds, priv->mds->fops->mkdir, loc,
               mode, umask, xdata);
    return 0;
}

int32_t
dsmds_symlink(call_frame_t *frame, xlator_t *this, const char *linkname,
              loc_t *loc, mode_t umask, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_symlink_cbk, priv->mds, priv->mds->fops->symlink,
               linkname, loc, umask, xdata);
    return 0;
}

int32_t
dsmds_mknod(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
            dev_t dev, mode_t umask, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;

    STACK_WIND(frame, default_mknod_cbk, priv->mds, priv->mds->fops->mknod, loc,
               mode, dev, umask, xdata);
    return 0;
}

int32_t
dsmds_readlink(call_frame_t *frame, xlator_t *this, loc_t *loc, size_t size,
               dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_readlink_cbk, priv->mds,
               priv->mds->fops->readlink, loc, size, xdata);
    return 0;
}

int32_t
dsmds_access(call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t mask,
             dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_access_cbk, priv->mds, priv->mds->fops->access,
               loc, mask, xdata);
    return 0;
}

int32_t
dsmds_opendir(call_frame_t *frame, xlator_t *this, loc_t *loc, fd_t *fd,
              dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_opendir_cbk, priv->mds, priv->mds->fops->opendir,
               loc, fd, xdata);
    return 0;
}

int32_t
dsmds_fsyncdir(call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t datasync,
               dict_t *xdata)

{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fsyncdir_cbk, priv->mds,
               priv->mds->fops->fsyncdir, fd, datasync, xdata);
    return 0;
}

int32_t
dsmds_readdir(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
              off_t offset, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_readdir_cbk, priv->mds, priv->mds->fops->readdir,
               fd, size, offset, xdata);

    return 0;
}

int32_t
dsmds_readdirp(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
               off_t offset, dict_t *dict)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_readdirp_cbk, priv->mds,
               priv->mds->fops->readdirp, fd, size, offset, dict);
    return 0;
}

int32_t
dsmds_fsetattr(call_frame_t *frame, xlator_t *this, fd_t *fd,
               struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fsetattr_cbk, priv->mds,
               priv->mds->fops->fsetattr, fd, stbuf, valid, xdata);
    return 0;
}

int32_t
dsmds_setattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
              struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_setattr_cbk, priv->mds,
               priv->mds->fops->setattr, loc, stbuf, valid, xdata);
    return 0;
}


int32_t
dsmds_fremovexattr(call_frame_t *frame, xlator_t *this, fd_t *fd,
                   const char *name, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fremovexattr_cbk, priv->mds,
               priv->mds->fops->fremovexattr, fd, name, xdata);
    return 0;
}

int32_t
dsmds_removexattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
                  const char *name, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_removexattr_cbk, priv->mds,
               priv->mds->fops->removexattr, loc, name, xdata);
    return 0;
}

int32_t
dsmds_setxattr(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *dict,
               int32_t flags, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_setxattr_cbk, priv->mds,
               priv->mds->fops->setxattr, loc, dict, flags, xdata);
    return 0;
}

int32_t
dsmds_fsetxattr(call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *dict,
                int32_t flags, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fsetxattr_cbk, priv->mds,
               priv->mds->fops->fsetxattr, fd, dict, flags, xdata);
    return 0;
}


int32_t
dsmds_fgetxattr(call_frame_t *frame, xlator_t *this, fd_t *fd, const char *name,
                dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fgetxattr_cbk, priv->mds,
               priv->mds->fops->fgetxattr, fd, name, xdata);
    return 0;
}

int32_t
dsmds_getxattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
               const char *name, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_getxattr_cbk, priv->mds,
               priv->mds->fops->getxattr, loc, name, xdata);
    return 0;
}

int32_t
dsmds_xattrop(call_frame_t *frame, xlator_t *this, loc_t *loc,
              gf_xattrop_flags_t flags, dict_t *dict, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_xattrop_cbk, priv->mds,
               priv->mds->fops->xattrop, loc, flags, dict, xdata);

    return 0;
}

int32_t
dsmds_fxattrop(call_frame_t *frame, xlator_t *this, fd_t *fd,
               gf_xattrop_flags_t flags, dict_t *dict, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fxattrop_cbk, priv->mds,
               priv->mds->fops->fxattrop, fd, flags, dict, xdata);

    return 0;
}

int32_t
dsmds_getspec(call_frame_t *frame, xlator_t *this, const char *key,
              int32_t flag)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_getspec_cbk, priv->mds,
               priv->mds->fops->getspec, key, flag);
    return 0;
}

/* ---------------- */

/* Both */
int
dsmds_statfs_cbk(call_frame_t *frame, void *cookie, xlator_t *this, int op_ret,
                 int op_errno, struct statvfs *statvfs, dict_t *xdata)
{
    int callcnt = 0;
    dsmds_local_t *local = frame->local;
    if (!local)
      goto unwind;
    /* TODO: do more stuff here */
    LOCK(&frame->lock);
    {
      callcnt = --local->callcnt;
    }
    UNLOCK(&frame->lock);

    if (!callcnt) {
      DSMDS_STACK_UNWIND(statfs, frame, op_ret, op_errno, statvfs, xdata);
    }

    return 0;
 unwind:
    DSMDS_STACK_UNWIND(statfs, frame, op_ret, op_errno, statvfs, xdata);

    return 0;
}

int32_t
dsmds_statfs(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    dsmds_local_t *local = GF_CALLOC(1, sizeof(*local), 0);
    /* Add log */
    if (!local)
      goto out;

    frame->local = local;
    local->callcnt = 2; /* DS + MDS */

    STACK_WIND(frame, dsmds_statfs_cbk, priv->mds, priv->mds->fops->statfs, loc,
               xdata);
 out:
    /* Assumption is df is asked on root. Anything else, it may need to
       changed for sending on DataPath */
    STACK_WIND(frame, dsmds_statfs_cbk, priv->ds, priv->ds->fops->statfs, loc,
               xdata);
    return 0;
}

int
mds_create_cbk()
{
}

int32_t
dsmds_create(call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
             mode_t mode, mode_t umask, fd_t *fd, dict_t *xdata)
{
    /* TODO: upon successful create, go ahead and create data file */
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, mds_create_cbk, priv->mds, priv->mds->fops->create,
               loc, flags, mode, umask, fd, xdata);
    return 0;
}


int32_t
dsmds_open(call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
           fd_t *fd, dict_t *xdata)
{
    STACK_WIND(frame, default_open_cbk, FIRST_CHILD(this),
               FIRST_CHILD(this)->fops->open, loc, flags, fd, xdata);
    return 0;
}

int
dsmds_ds_lookup_cbk(call_frame_t *frame, void *cookie, xlator_t *this, int op_ret,
               int op_errno, inode_t *inode, struct iatt *stbuf, dict_t *xattr,
               struct iatt *postparent)
{
  dsmds_private_t *priv = this->private;
  dsmds_local_t *local = frame->local;

  DSMDS_UNWIND_STRICT(lookup, frame, op_ret, op_errno, &local->inode, &local->mds_stbuf,
		      local->xdata_rsp,
		      postparent);
  return 0;
}

int
dsmds_mds_lookup_cbk(call_frame_t *frame, void *cookie, xlator_t *this, int op_ret,
               int op_errno, inode_t *inode, struct iatt *stbuf, dict_t *xattr,
               struct iatt *postparent)
{
  dsmds_private_t *priv = this->private;
  dsmds_local_t *local = frame->local;

  if (!local)
    goto out;
  
  if ((op_ret == 0) && IA_ISREG(stbuf)) {
    /* TODO: get the loc helper to fill the new ds loc */
    local->mds_stbuf = *stbuf;
    local->xdata_rsp = dict_ref(xattr);
    local->inode = inode_ref(inode);
    build_ds_loc(inode->itable, stbuf->ia_gfid, local);
    STACK_WIND(frame, dsmds_ds_lookup_cbk, priv->ds, priv->ds->fops->lookup,
               &local->ds_loc, local->xdata_req);
    
    return 0;
  }

 out:
  DSMDS_UNWIND_STRICT(lookup, frame, op_ret, op_errno, inode, stbuf, xattr,
		      postparent);
  
  return 0;
}

int32_t
dsmds_lookup(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
  /* TODO: if revalidate, send it on both if inode->ia_type is REG */
    dsmds_private_t *priv = this->private;
    dsmds_local_t *local = GF_CALLOC(1, sizeof(dsmds_local_t), 0);
    /* Add a log */
    if (!local)
        goto out;
    frame->local = local;
    local->xdata_req = dict_ref(xdata);
    local->orig_loc = *loc;
    STACK_WIND(frame, dsmds_mds_lookup_cbk, priv->mds, priv->mds->fops->lookup,
               loc, xdata);
    return 0;
}

int32_t
dsmds_unlink(call_frame_t *frame, xlator_t *this, loc_t *loc, int xflags,
             dict_t *xdata)
{
    /* TODO: also remove the data file from backend */
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_unlink_cbk, priv->mds, priv->mds->fops->unlink,
               loc, xflags, xdata);
    return 0;
}

int32_t
dsmds_stat(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_stat_cbk, priv->mds,
               priv->mds->fops->stat, loc, xdata);
    return 0;
}

int32_t
dsmds_fstat(call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fstat_cbk, priv->mds,
               priv->mds->fops->fstat, fd, xdata);
    return 0;
}


/* ---------------- */

/* Only Data Path */
int32_t
dsmds_truncate(call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
               dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_truncate_cbk, priv->ds,
               priv->ds->fops->truncate, loc, offset, xdata);
    return 0;
}

int32_t
dsmds_ftruncate(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_ftruncate_cbk, priv->ds,
               priv->ds->fops->ftruncate, fd, offset, xdata);
    return 0;
}

int32_t
dsmds_lk(call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t cmd,
         struct gf_flock *flock, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_lk_cbk, priv->ds,
               priv->ds->fops->lk, fd, cmd, flock, xdata);
    return 0;
}

int32_t
dsmds_writev(call_frame_t *frame, xlator_t *this, fd_t *fd,
             struct iovec *vector, int32_t count, off_t offset, uint32_t flags,
             struct iobref *iobref, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_writev_cbk, priv->ds,
               priv->ds->fops->writev, fd, vector, count, offset,
               flags, iobref, xdata);
    return 0;
}

int32_t
dsmds_readv(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
            off_t offset, uint32_t flags, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_readv_cbk, priv->ds,
               priv->ds->fops->readv, fd, size, offset, flags, xdata);
    return 0;
}

int32_t
dsmds_flush(call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_flush_cbk, priv->ds,
               priv->ds->fops->flush, fd, xdata);
    return 0;
}

int32_t
dsmds_fsync(call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t datasync,
            dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fsync_cbk, priv->ds,
               priv->ds->fops->fsync, fd, datasync, xdata);
    return 0;
}
int32_t
dsmds_rchecksum(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                int32_t len, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_rchecksum_cbk, priv->ds,
               priv->ds->fops->rchecksum, fd, offset, len, xdata);
    return 0;
}

int32_t
dsmds_inodelk(call_frame_t *frame, xlator_t *this, const char *volume,
              loc_t *loc, int32_t cmd, struct gf_flock *flock, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_inodelk_cbk, priv->ds,
               priv->ds->fops->inodelk, volume, loc, cmd, flock,
               xdata);
    return 0;
}

int32_t
dsmds_finodelk(call_frame_t *frame, xlator_t *this, const char *volume,
               fd_t *fd, int32_t cmd, struct gf_flock *flock, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_finodelk_cbk, priv->ds,
               priv->ds->fops->finodelk, volume, fd, cmd, flock,
               xdata);
    return 0;
}

int32_t
dsmds_fallocate(call_frame_t *frame, xlator_t *this, fd_t *fd,
                int32_t keep_size, off_t offset, size_t len, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_fallocate_cbk, priv->ds,
               priv->ds->fops->fallocate, fd, keep_size, offset, len,
               xdata);
    return 0;
}

int32_t
dsmds_discard(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
              size_t len, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_discard_cbk, priv->ds,
               priv->ds->fops->discard, fd, offset, len, xdata);
    return 0;
}

int32_t
dsmds_zerofill(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
               off_t len, dict_t *xdata)
{
    dsmds_private_t *priv = this->private;
    STACK_WIND(frame, default_zerofill_cbk, priv->ds,
               priv->ds->fops->zerofill, fd, offset, len, xdata);
    return 0;
}

int
dsmds_forget(xlator_t *this, inode_t *inode)
{
    return 0;
}

/* required only in case of file (create/open) */
int32_t
dsmds_release(xlator_t *this, fd_t *fd)
{
    return 0;
}

int32_t
init(xlator_t *this)
{
    int32_t ret = -1;
    dsmds_private_t *priv = NULL;

    if (!this->children || !this->children->next ||
        this->children->next->next) {
        gf_log(this->name, GF_LOG_ERROR, "translator needs two subvolumes.");
        goto out;
    }

    if (!this->parents) {
        gf_log(this->name, GF_LOG_ERROR,
               "dangling volume. please check volfile.");
        goto out;
    }

    priv = GF_CALLOC(1, sizeof(dsmds_private_t), 0);
    if (!priv) {
        gf_log(this->name, GF_LOG_ERROR,
               "Can't allocate dsmds_priv structure.");
        goto out;
    }

    priv->mds = FIRST_CHILD(this);
    priv->ds = SECOND_CHILD(this);

    this->private = priv;
    ret = 0;

out:
    if (ret) {
        GF_FREE(priv);
    }

    return ret;
}

void
fini(xlator_t *this)
{
    GF_FREE(this->private);
    this->private = NULL;
}

int
reconfigure(xlator_t *this, dict_t *options)
{
    int ret = -1;

    ret = 0;
    return ret;
}

struct xlator_fops fops = {
    .lookup = dsmds_lookup,
    .stat = dsmds_stat,
    .fstat = dsmds_fstat,
    .truncate = dsmds_truncate,
    .ftruncate = dsmds_ftruncate,
    .access = dsmds_access,
    .readlink = dsmds_readlink,
    .mknod = dsmds_mknod,
    .mkdir = dsmds_mkdir,
    .unlink = dsmds_unlink,
    .rmdir = dsmds_rmdir,
    .symlink = dsmds_symlink,
    .rename = dsmds_rename,
    .link = dsmds_link,
    .create = dsmds_create,
    .open = dsmds_open,
    .readv = dsmds_readv,
    .writev = dsmds_writev,
    .flush = dsmds_flush,
    .fsync = dsmds_fsync,
    .opendir = dsmds_opendir,
    .readdir = dsmds_readdir,
    .readdirp = dsmds_readdirp,
    .fsyncdir = dsmds_fsyncdir,
    .statfs = dsmds_statfs,
    .setxattr = dsmds_setxattr,
    .getxattr = dsmds_getxattr,
    .fsetxattr = dsmds_fsetxattr,
    .fgetxattr = dsmds_fgetxattr,
    .removexattr = dsmds_removexattr,
    .fremovexattr = dsmds_fremovexattr,
    .lk = dsmds_lk,
    .inodelk = dsmds_inodelk,
    .finodelk = dsmds_finodelk,
    .entrylk = dsmds_entrylk,
    .fentrylk = dsmds_fentrylk,
    .rchecksum = dsmds_rchecksum,
    .xattrop = dsmds_xattrop,
    .fxattrop = dsmds_fxattrop,
    .setattr = dsmds_setattr,
    .fsetattr = dsmds_fsetattr,
    .getspec = dsmds_getspec,
    .fallocate = dsmds_fallocate,
    .discard = dsmds_discard,
    .zerofill = dsmds_zerofill,
    /* pending */
    //.ipc
    //.lease
    //.put
    //.seek
    //.copy_file_range

};

struct xlator_cbks cbks = {
    .forget = dsmds_forget,
    .release = dsmds_release,
};

struct xlator_dumpops dumpops;

struct volume_options options[] = {
    {.key = {NULL}},
};

xlator_api_t xlator_api = {
    .init = init,
    .fini = fini,
    .reconfigure = reconfigure,
    .op_version = {GD_OP_VERSION_8_0},
    .dumpops = &dumpops,
    .fops = &fops,
    .cbks = &cbks,
    .options = options,
    .identifier = "dsmds",
    .category = GF_TECH_PREVIEW,
};
