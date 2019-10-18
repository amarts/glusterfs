#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "metadisp-fops.h"
#include "metadisp.h"
#include <glusterfs/xlator.h>

/* BEGIN GENERATED CODE - DO NOT MODIFY */

int32_t
metadisp_writev(call_frame_t *frame, xlator_t *this, fd_t *fd,
                struct iovec *vector, int32_t count, off_t off, uint32_t flags,
                struct iobref *iobref, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_writev_cbk, child, child->fops->writev, fd,
               vector, count, off, flags, iobref, xdata);
    return 0;
}

int32_t
metadisp_readv(call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
               off_t offset, uint32_t flags, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_readv_cbk, child, child->fops->readv, fd, size,
               offset, flags, xdata);
    return 0;
}

int32_t
metadisp_ftruncate(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                   dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_ftruncate_cbk, child, child->fops->ftruncate, fd,
               offset, xdata);
    return 0;
}

int32_t
metadisp_zerofill(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                  off_t len, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_zerofill_cbk, child, child->fops->zerofill, fd,
               offset, len, xdata);
    return 0;
}

int32_t
metadisp_discard(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
                 size_t len, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_discard_cbk, child, child->fops->discard, fd,
               offset, len, xdata);
    return 0;
}

int32_t
metadisp_seek(call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
              gf_seek_what_t what, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_seek_cbk, child, child->fops->seek, fd, offset,
               what, xdata);
    return 0;
}

int32_t
metadisp_fstat(call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_fstat_cbk, child, child->fops->fstat, fd, xdata);
    return 0;
}

int32_t
metadisp_truncate(call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
                  dict_t *xdata)
{
    xlator_t *child = NULL;
    child = DATA_CHILD(this);
    STACK_WIND(frame, default_truncate_cbk, child, child->fops->truncate, loc,
               offset, xdata);
    return 0;
}

int32_t
metadisp_mkdir(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               mode_t umask, dict_t *xdata)
{
    METADISP_TRACE("mkdir metadata");
    STACK_WIND(frame, default_mkdir_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->mkdir, loc, mode, umask, xdata);
    return 0;
}

int32_t
metadisp_symlink(call_frame_t *frame, xlator_t *this, const char *linkpath,
                 loc_t *loc, mode_t umask, dict_t *xdata)
{
    METADISP_TRACE("symlink metadata");
    STACK_WIND(frame, default_symlink_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->symlink, linkpath, loc, umask,
               xdata);
    return 0;
}

int32_t
metadisp_link(call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc,
              dict_t *xdata)
{
    METADISP_TRACE("link metadata");
    STACK_WIND(frame, default_link_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->link, oldloc, newloc, xdata);
    return 0;
}

int32_t
metadisp_rename(call_frame_t *frame, xlator_t *this, loc_t *oldloc,
                loc_t *newloc, dict_t *xdata)
{
    METADISP_TRACE("rename metadata");
    STACK_WIND(frame, default_rename_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->rename, oldloc, newloc, xdata);
    return 0;
}

int32_t
metadisp_mknod(call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
               dev_t rdev, mode_t umask, dict_t *xdata)
{
    METADISP_TRACE("mknod metadata");
    STACK_WIND(frame, default_mknod_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->mknod, loc, mode, rdev, umask,
               xdata);
    return 0;
}

int32_t
metadisp_opendir(call_frame_t *frame, xlator_t *this, loc_t *loc, fd_t *fd,
                 dict_t *xdata)
{
    METADISP_TRACE("opendir metadata");
    STACK_WIND(frame, default_opendir_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->opendir, loc, fd, xdata);
    return 0;
}

int32_t
metadisp_fsyncdir(call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t flags,
                  dict_t *xdata)
{
    METADISP_TRACE("fsyncdir metadata");
    STACK_WIND(frame, default_fsyncdir_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->fsyncdir, fd, flags, xdata);
    return 0;
}

int32_t
metadisp_setattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
                 struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
    METADISP_TRACE("setattr metadata");
    STACK_WIND(frame, default_setattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->setattr, loc, stbuf, valid, xdata);
    return 0;
}

int32_t
metadisp_readlink(call_frame_t *frame, xlator_t *this, loc_t *loc, size_t size,
                  dict_t *xdata)
{
    METADISP_TRACE("readlink metadata");
    STACK_WIND(frame, default_readlink_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->readlink, loc, size, xdata);
    return 0;
}

int32_t
metadisp_fentrylk(call_frame_t *frame, xlator_t *this, const char *volume,
                  fd_t *fd, const char *basename, entrylk_cmd cmd,
                  entrylk_type type, dict_t *xdata)
{
    METADISP_TRACE("fentrylk metadata");
    STACK_WIND(frame, default_fentrylk_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->fentrylk, volume, fd, basename, cmd,
               type, xdata);
    return 0;
}

int32_t
metadisp_access(call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t mask,
                dict_t *xdata)
{
    METADISP_TRACE("access metadata");
    STACK_WIND(frame, default_access_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->access, loc, mask, xdata);
    return 0;
}

int32_t
metadisp_xattrop(call_frame_t *frame, xlator_t *this, loc_t *loc,
                 gf_xattrop_flags_t flags, dict_t *dict, dict_t *xdata)
{
    METADISP_TRACE("xattrop metadata");
    STACK_WIND(frame, default_xattrop_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->xattrop, loc, flags, dict, xdata);
    return 0;
}

int32_t
metadisp_setxattr(call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *dict,
                  int32_t flags, dict_t *xdata)
{
    METADISP_TRACE("setxattr metadata");
    STACK_WIND(frame, default_setxattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->setxattr, loc, dict, flags, xdata);
    return 0;
}

int32_t
metadisp_getxattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
                  const char *name, dict_t *xdata)
{
    METADISP_TRACE("getxattr metadata");
    STACK_WIND(frame, default_getxattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->getxattr, loc, name, xdata);
    return 0;
}

int32_t
metadisp_removexattr(call_frame_t *frame, xlator_t *this, loc_t *loc,
                     const char *name, dict_t *xdata)
{
    METADISP_TRACE("removexattr metadata");
    STACK_WIND(frame, default_removexattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->removexattr, loc, name, xdata);
    return 0;
}

int32_t
metadisp_fgetxattr(call_frame_t *frame, xlator_t *this, fd_t *fd,
                   const char *name, dict_t *xdata)
{
    METADISP_TRACE("fgetxattr metadata");
    STACK_WIND(frame, default_fgetxattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->fgetxattr, fd, name, xdata);
    return 0;
}

int32_t
metadisp_fsetxattr(call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *dict,
                   int32_t flags, dict_t *xdata)
{
    METADISP_TRACE("fsetxattr metadata");
    STACK_WIND(frame, default_fsetxattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->fsetxattr, fd, dict, flags, xdata);
    return 0;
}

int32_t
metadisp_fremovexattr(call_frame_t *frame, xlator_t *this, fd_t *fd,
                      const char *name, dict_t *xdata)
{
    METADISP_TRACE("fremovexattr metadata");
    STACK_WIND(frame, default_fremovexattr_cbk, METADATA_CHILD(this),
               METADATA_CHILD(this)->fops->fremovexattr, fd, name, xdata);
    return 0;
}

struct xlator_fops fops = {
    .readdir = metadisp_readdir,
    .readdirp = metadisp_readdirp,
    .lookup = metadisp_lookup,
    .fsync = metadisp_fsync,
    .stat = metadisp_stat,
    .open = metadisp_open,
    .create = metadisp_create,
    .unlink = metadisp_unlink,
    .writev = metadisp_writev,
    .readv = metadisp_readv,
    .ftruncate = metadisp_ftruncate,
    .zerofill = metadisp_zerofill,
    .discard = metadisp_discard,
    .seek = metadisp_seek,
    .fstat = metadisp_fstat,
    .truncate = metadisp_truncate,
    .mkdir = metadisp_mkdir,
    .symlink = metadisp_symlink,
    .link = metadisp_link,
    .rename = metadisp_rename,
    .mknod = metadisp_mknod,
    .opendir = metadisp_opendir,
    .fsyncdir = metadisp_fsyncdir,
    .setattr = metadisp_setattr,
    .readlink = metadisp_readlink,
    .fentrylk = metadisp_fentrylk,
    .access = metadisp_access,
    .xattrop = metadisp_xattrop,
    .setxattr = metadisp_setxattr,
    .getxattr = metadisp_getxattr,
    .removexattr = metadisp_removexattr,
    .fgetxattr = metadisp_fgetxattr,
    .fsetxattr = metadisp_fsetxattr,
    .fremovexattr = metadisp_fremovexattr,
};
/* END GENERATED CODE */
