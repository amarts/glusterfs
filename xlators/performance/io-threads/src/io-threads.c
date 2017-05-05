/*
  Copyright (c) 2006-2009 Z RESEARCH, Inc. <http://www.zresearch.com>
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

#include "call-stub.h"
#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"
#include "io-threads.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

typedef void *(*iot_worker_fn)(void*);

void
iot_stop_worker (iot_worker_t *worker);

void
iot_stop_workers (iot_worker_t **workers, int start_idx, int count);

void
_iot_queue (iot_worker_t *worker, iot_request_t *req);

iot_request_t *
iot_init_request (call_stub_t *stub);

int
iot_startup_workers (iot_worker_t **workers, int start_idx, int count,
                     iot_worker_fn workerfunc);

void *
iot_worker_unordered (void *arg);

void *
iot_worker_ordered (void *arg);

int
iot_startup_worker (iot_worker_t *worker, iot_worker_fn workerfunc);

void
iot_destroy_request (iot_request_t * req);


/* I know this function modularizes things a bit too much,
 * but it is easier on the eyes to read this than see all that locking,
 * queueing, and thread firing in the same curly block, as was the
 * case before this function.
 */
int
iot_request_queue_and_thread_fire (iot_worker_t *worker,
                                   iot_worker_fn workerfunc, iot_request_t *req)
{
        int     ret = -1; 
        pthread_mutex_lock (&worker->qlock);
        {
                if (iot_worker_active (worker)) {
                        _iot_queue (worker, req);
                        ret = 0;
                }else {
                        ret = iot_startup_worker (worker, workerfunc);
                        if (ret < 0) {
                                goto unlock;
                        }
                        _iot_queue (worker, req);
                }
        }
unlock:
        pthread_mutex_unlock (&worker->qlock);

        return ret;
}


int
iot_unordered_request_balancer (iot_conf_t *conf)
{
        long int        rand = 0;
        int             idx = 0;

        /* Decide which thread will service the request.
         * FIXME: This should change into some form of load-balancing.
         * */
        rand = random ();

        /* If scaling is on, we can choose from any thread
        * that has been allocated upto, max_o_threads, but
        * with scaling off, we'll never have threads more
        * than min_o_threads.
        */
        if (iot_unordered_scaling_on (conf))
                idx = (rand % conf->max_u_threads);
        else
                idx = (rand % conf->min_u_threads);

        return idx;
}


int
iot_schedule_unordered (iot_conf_t *conf, inode_t *inode, call_stub_t *stub)
{
        int32_t          idx = 0;
        iot_worker_t    *selected_worker = NULL;
        iot_request_t   *req = NULL;
        int             ret = -1;

        idx = iot_unordered_request_balancer (conf);
        selected_worker = conf->uworkers[idx];

        req = iot_init_request (stub);
        if (req == NULL) {
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_request_queue_and_thread_fire (selected_worker,
                                                 iot_worker_unordered, req);
        if (ret < 0) {
                iot_destroy_request (req);
        }
out:
        return ret;
}


/* Only to be used with ordered requests.
 */
uint64_t
iot_create_inode_worker_assoc (iot_conf_t * conf, inode_t * inode)
{
        long int        rand = 0;
        uint64_t        idx = 0;

        rand = random ();
        /* If scaling is on, we can choose from any thread
        * that has been allocated upto, max_o_threads, but
        * with scaling off, we'll never have threads more
        * than min_o_threads.
        */
        if (iot_ordered_scaling_on (conf))
                idx = (rand % conf->max_o_threads);
        else
                idx = (rand % conf->min_o_threads);

        __inode_ctx_put (inode, conf->this, idx);

        return idx;
}


/* Assumes inode lock is held. */
int32_t
iot_ordered_request_balancer (iot_conf_t *conf, inode_t *inode, uint64_t *idx)
{
        int ret = -1;

        if (__inode_ctx_get (inode, conf->this, idx) < 0)
                *idx = iot_create_inode_worker_assoc (conf, inode);
        else {
                /* Sanity check to ensure the idx received from the inode
                * context is within bounds. We're a bit optimistic in
                * assuming that if an index is within bounds, it is
                * not corrupted. idx is uint so we dont check for less
                * than 0.
                */
                if ((*idx >= (uint64_t)conf->max_o_threads)) {
                        gf_log (conf->this->name, GF_LOG_DEBUG,
                                "inode context returned insane thread index %"
                                PRIu64, *idx);
                        ret = -EINVAL;
                        goto out;
                }
        }
        ret = 0;
out:
        return ret;
}


int
iot_schedule_ordered (iot_conf_t *conf, inode_t *inode, call_stub_t *stub)
{
        uint64_t         idx = 0;
        iot_worker_t    *selected_worker = NULL;
        iot_request_t   *req = NULL;
        int              balstatus = 0, ret = -1;

        if (inode == NULL) {
                gf_log (conf->this->name, GF_LOG_DEBUG,
                        "Got NULL inode for ordered request");
                ret = -EINVAL;
                goto out;
        }

        req = iot_init_request (stub);
        if (req == NULL) {
                gf_log (conf->this->name, GF_LOG_ERROR,
                        "out of memory");
                ret = -ENOMEM;
                goto out;
        }

        LOCK (&inode->lock);
        {
                balstatus = iot_ordered_request_balancer (conf, inode, &idx);
                if (balstatus < 0) {
                        gf_log (conf->this->name, GF_LOG_DEBUG,
                                "Insane worker index. Unwinding stack");
                        ret = -ECANCELED;
                        goto unlock_out;
                }
                /* inode lock once acquired, cannot be left here
                 * because other gluster main threads might be
                 * contending on it to append a request for this file.
                 * So we'll also leave the lock only after we've
                 * added the request to the worker queue.
                 */
                selected_worker = conf->oworkers[idx];
                ret = iot_request_queue_and_thread_fire (selected_worker,
                                                         iot_worker_ordered,
                                                         req);
        }
unlock_out:
        UNLOCK (&inode->lock);

out:
        if (ret < 0) {
                if (req != NULL) {
                        iot_destroy_request (req);
                }
        }
        return ret;
}


int
iot_lookup_cbk (call_frame_t *frame, void * cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno,
                inode_t *inode, struct stat *buf, dict_t *xattr)
{
        STACK_UNWIND (frame, op_ret, op_errno, inode, buf, xattr);
        return 0;
}


int
iot_lookup_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                    dict_t *xattr_req)
{
        STACK_WIND (frame, iot_lookup_cbk,
                    FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->lookup,
                    loc, xattr_req);
        return 0;
}


int
iot_lookup (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xattr_req)
{
        call_stub_t     *stub = NULL;
        int              ret = -1;

        stub = fop_lookup_stub (frame, iot_lookup_wrapper, loc, xattr_req);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR,
                        "cannot create lookup stub (out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
                STACK_UNWIND (frame, -1, -ret, NULL, NULL, NULL);
        }

        return 0;
}


int
iot_chmod_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_chmod_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                   mode_t mode)
{
        STACK_WIND (frame, iot_chmod_cbk,
                    FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->chmod,
                    loc, mode);
        return 0;
}


int
iot_chmod (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int             ret = -1;

        stub = fop_chmod_stub (frame, iot_chmod_wrapper, loc, mode);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create chmod stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

	fd = fd_lookup (loc->inode, frame->root->pid);
        if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                        loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
                        
                STACK_UNWIND (frame, -1, -ret, NULL);
        }

        return 0;
}


int
iot_fchmod_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_fchmod_wrapper (call_frame_t *frame, xlator_t *this,
                    fd_t *fd, mode_t mode)
{
        STACK_WIND (frame, iot_fchmod_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fchmod, fd, mode);
        return 0;
}


int
iot_fchmod (call_frame_t *frame, xlator_t *this, fd_t *fd, mode_t mode)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fchmod_stub (frame, iot_fchmod_wrapper, fd, mode);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fchmod stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_chown_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_chown_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc, uid_t uid,
                   gid_t gid)
{
        STACK_WIND (frame, iot_chown_cbk,
                    FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->chown,
                    loc, uid, gid);
        return 0;
}


int
iot_chown (call_frame_t *frame, xlator_t *this, loc_t *loc, uid_t uid,
           gid_t gid)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int             ret = -1;

        stub = fop_chown_stub (frame, iot_chown_wrapper, loc, uid, gid);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create chown stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                        loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_fchown_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_fchown_wrapper (call_frame_t *frame, xlator_t *this,
                    fd_t *fd, uid_t uid, gid_t gid)
{
        STACK_WIND (frame, iot_fchown_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fchown, fd, uid, gid);
        return 0;
}


int
iot_fchown (call_frame_t *frame, xlator_t *this, fd_t *fd, uid_t uid, gid_t gid)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fchown_stub (frame, iot_fchown_wrapper, fd, uid, gid);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fchown stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_access_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_access_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                    int32_t mask)
{
        STACK_WIND (frame, iot_access_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->access, loc, mask);
        return 0;
}


int
iot_access (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t mask)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_access_stub (frame, iot_access_wrapper, loc, mask);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create access stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_readlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, const char *path)
{
        STACK_UNWIND (frame, op_ret, op_errno, path);
        return 0;
}


int
iot_readlink_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                      size_t size)
{
        STACK_WIND (frame, iot_readlink_cbk,
                    FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->readlink,
                    loc, size);
        return 0;
}


int
iot_readlink (call_frame_t *frame, xlator_t *this, loc_t *loc, size_t size)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_readlink_stub (frame, iot_readlink_wrapper, loc, size);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create readlink stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }

        return 0;
}


int
iot_mknod_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, inode_t *inode,
               struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
        return 0;
}


int
iot_mknod_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
                   dev_t rdev)
{
        STACK_WIND (frame, iot_mknod_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->mknod, loc, mode, rdev);
        return 0;
}


int
iot_mknod (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode,
           dev_t rdev)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_mknod_stub (frame, iot_mknod_wrapper, loc, mode, rdev);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create mknod stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_mkdir_cbk (call_frame_t *frame, void * cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, inode_t *inode,
               struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
        return 0;
}


int
iot_mkdir_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode)
{
        STACK_WIND (frame, iot_mkdir_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->mkdir, loc, mode);
        return 0;
}


int
iot_mkdir (call_frame_t *frame, xlator_t *this, loc_t *loc, mode_t mode)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_mkdir_stub (frame, iot_mkdir_wrapper, loc, mode);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create mkdir stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_rmdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_rmdir_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
        STACK_WIND (frame, iot_rmdir_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->rmdir, loc);
        return 0;
}


int
iot_rmdir (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_rmdir_stub (frame, iot_rmdir_wrapper, loc);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create rmdir stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_symlink_cbk (call_frame_t *frame, void * cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, inode_t *inode,
                 struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
        return 0;
}


int
iot_symlink_wrapper (call_frame_t *frame, xlator_t *this, const char *linkname,
                     loc_t *loc)
{
        STACK_WIND (frame, iot_symlink_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->symlink, linkname, loc);
        return 0;
}


int
iot_symlink (call_frame_t *frame, xlator_t *this, const char *linkname,
             loc_t *loc)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_symlink_stub (frame, iot_symlink_wrapper, linkname, loc);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create symlink stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }

        return 0;
}


int
iot_rename_cbk (call_frame_t *frame, void * cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_rename_wrapper (call_frame_t *frame, xlator_t *this, loc_t *oldloc,
                    loc_t *newloc)
{
        STACK_WIND (frame, iot_rename_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->rename, oldloc, newloc);
        return 0;
}


int
iot_rename (call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_rename_stub (frame, iot_rename_wrapper, oldloc, newloc);
        if (!stub) {
                gf_log (this->name, GF_LOG_DEBUG, "cannot create rename stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                      oldloc->inode, stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
                
        return 0;
}


int
iot_open_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
              int32_t op_errno, fd_t *fd)
{
	STACK_UNWIND (frame, op_ret, op_errno, fd);
	return 0;
}


int
iot_open_wrapper (call_frame_t * frame, xlator_t * this, loc_t *loc,
                  int32_t flags, fd_t * fd)
{
	STACK_WIND (frame, iot_open_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->open, loc, flags, fd);
	return 0;
}


int
iot_open (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
          fd_t *fd)
{
        call_stub_t	*stub = NULL;
        int             ret = -1;

        stub = fop_open_stub (frame, iot_open_wrapper, loc, flags, fd);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR,
                        "cannot create open call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

	ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, 0);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }                

	return 0;
}


int
iot_create_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, fd_t *fd, inode_t *inode,
                struct stat *stbuf)
{
	STACK_UNWIND (frame, op_ret, op_errno, fd, inode, stbuf);
	return 0;
}


int
iot_create_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                    int32_t flags, mode_t mode, fd_t *fd)
{
	STACK_WIND (frame, iot_create_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->create,
		    loc, flags, mode, fd);
	return 0;
}


int
iot_create (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
            mode_t mode, fd_t *fd)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_create_stub (frame, iot_create_wrapper, loc, flags, mode,
                                fd);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR,
                        "cannot create \"create\" call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, 0);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }

        return 0;
}


int
iot_readv_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct iovec *vector,
               int32_t count, struct stat *stbuf, struct iobref *iobref)
{
	STACK_UNWIND (frame, op_ret, op_errno, vector, count, stbuf, iobref);

	return 0;
}


int
iot_readv_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
                   off_t offset)
{
	STACK_WIND (frame, iot_readv_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->readv,
		    fd, size, offset);
	return 0;
}


int
iot_readv (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
           off_t offset)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_readv_stub (frame, iot_readv_wrapper, fd, size, offset);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR, 
			"cannot create readv call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL, 0);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_flush_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno)
{
	STACK_UNWIND (frame, op_ret, op_errno);
	return 0;
}


int
iot_flush_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd)
{
	STACK_WIND (frame, iot_flush_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->flush,
		    fd);
	return 0;
}


int
iot_flush (call_frame_t *frame, xlator_t *this, fd_t *fd)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_flush_stub (frame, iot_flush_wrapper, fd);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create flush_cbk call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_fsync_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno)
{
	STACK_UNWIND (frame, op_ret, op_errno);
	return 0;
}


int
iot_fsync_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                   int32_t datasync)
{
	STACK_WIND (frame, iot_fsync_cbk,
		    FIRST_CHILD (this),
		    FIRST_CHILD (this)->fops->fsync,
		    fd, datasync);
	return 0;
}


int
iot_fsync (call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t datasync)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_fsync_stub (frame, iot_fsync_wrapper, fd, datasync);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fsync_cbk call stub"
                        "(out of memory)");
                ret = -1;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct stat *stbuf)
{
	STACK_UNWIND (frame, op_ret, op_errno, stbuf);
	return 0;
}


int
iot_writev_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                    struct iovec *vector, int32_t count,
                    off_t offset, struct iobref *iobref)
{
	STACK_WIND (frame, iot_writev_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->writev,
		    fd, vector, count, offset, iobref);
	return 0;
}


int
iot_writev (call_frame_t *frame, xlator_t *this, fd_t *fd,
            struct iovec *vector, int32_t count, off_t offset,
            struct iobref *iobref)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_writev_stub (frame, iot_writev_wrapper,
				fd, vector, count, offset, iobref);

	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create writev call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }

	return 0;
}


int32_t
iot_lk_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
            int32_t op_ret, int32_t op_errno, struct flock *flock)
{
	STACK_UNWIND (frame, op_ret, op_errno, flock);
	return 0;
}


int
iot_lk_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                int32_t cmd, struct flock *flock)
{
	STACK_WIND (frame, iot_lk_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->lk,
		    fd, cmd, flock);
	return 0;
}


int
iot_lk (call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t cmd,
	struct flock *flock)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_lk_stub (frame, iot_lk_wrapper, fd, cmd, flock);

	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_lk call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_stat_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
              int32_t op_ret, int32_t op_errno, struct stat *buf)
{
	STACK_UNWIND (frame, op_ret, op_errno, buf);
	return 0;
}


int
iot_stat_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
	STACK_WIND (frame, iot_stat_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->stat,
		    loc);
	return 0;
}


int
iot_stat (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
	call_stub_t *stub = NULL;
	fd_t        *fd = NULL;
        int         ret = -1;

        stub = fop_stat_stub (frame, iot_stat_wrapper, loc);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_stat call stub"
                        "(out of memory)");
                ret = -1;
                goto out;
	}

	fd = fd_lookup (loc->inode, frame->root->pid);
        /* File is not open, so we can send it through unordered pool.
         */
	if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
	        fd_unref (fd);
        }

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_fstat_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct stat *buf)
{
	STACK_UNWIND (frame, op_ret, op_errno, buf);
	return 0;
}


int
iot_fstat_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd)
{
	STACK_WIND (frame, iot_fstat_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->fstat,
		    fd);
	return 0;
}


int
iot_fstat (call_frame_t *frame, xlator_t *this, fd_t *fd)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_fstat_stub (frame, iot_fstat_wrapper, fd);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_fstat call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, struct stat *buf)
{
	STACK_UNWIND (frame, op_ret, op_errno, buf);
	return 0;
}


int
iot_truncate_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                      off_t offset)
{
	STACK_WIND (frame, iot_truncate_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->truncate,
		    loc, offset);
	return 0;
}


int
iot_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset)
{
	call_stub_t *stub;
	fd_t        *fd = NULL;
        int         ret = -1;

        stub = fop_truncate_stub (frame, iot_truncate_wrapper, loc, offset);

	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_stat call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

	fd = fd_lookup (loc->inode, frame->root->pid);
	if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
	        fd_unref (fd);
        }

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }

	return 0;
}


int
iot_ftruncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, struct stat *buf)
{
	STACK_UNWIND (frame, op_ret, op_errno, buf);
	return 0;
}


int
iot_ftruncate_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                       off_t offset)
{
	STACK_WIND (frame, iot_ftruncate_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->ftruncate,
		    fd, offset);
	return 0;
}


int
iot_ftruncate (call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_ftruncate_stub (frame, iot_ftruncate_wrapper, fd, offset);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_ftruncate call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}
        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_utimens_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno,
                 struct stat *buf)
{
	STACK_UNWIND (frame, op_ret, op_errno, buf);
	return 0;
}


int
iot_utimens_wrapper (call_frame_t *frame, xlator_t *this,
                     loc_t *loc, struct timespec tv[2])
{
	STACK_WIND (frame, iot_utimens_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->utimens,
		    loc, tv);
  	return 0;
}


int
iot_utimens (call_frame_t *frame, xlator_t *this, loc_t *loc,
             struct timespec tv[2])
{
	call_stub_t *stub;
	fd_t        *fd = NULL;
        int         ret = -1;

	stub = fop_utimens_stub (frame, iot_utimens_wrapper, loc, tv);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_utimens call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
	        fd_unref (fd);
        }

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_checksum_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
		  int32_t op_ret, int32_t op_errno, uint8_t *file_checksum,
                  uint8_t *dir_checksum)
{
	STACK_UNWIND (frame, op_ret, op_errno, file_checksum, dir_checksum);
	return 0;
}


int
iot_checksum_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                      int32_t flags)
{
	STACK_WIND (frame, iot_checksum_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->checksum,
		    loc, flags);
  
	return 0;
}


int
iot_checksum (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags)
{
	call_stub_t *stub = NULL;
        int         ret = -1;

	stub = fop_checksum_stub (frame, iot_checksum_wrapper, loc, flags);

	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_checksum call stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
	}
        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);
out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret, NULL, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
	return 0;
}


int
iot_unlink_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
		int32_t op_ret, int32_t op_errno)
{
	STACK_UNWIND (frame, op_ret, op_errno);
	return 0;
}


int
iot_unlink_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
	STACK_WIND (frame, iot_unlink_cbk,
		    FIRST_CHILD(this),
		    FIRST_CHILD(this)->fops->unlink,
		    loc);
  
	return 0;
}


int
iot_unlink (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
	call_stub_t *stub = NULL;
        int         ret = -1;
 
	stub = fop_unlink_stub (frame, iot_unlink_wrapper, loc);
	if (!stub) {
		gf_log (this->name, GF_LOG_ERROR,
                        "cannot create fop_unlink call stub"
                        "(out of memory)");
                ret = -1;
                goto out;
	}

        ret = iot_schedule_unordered((iot_conf_t *)this->private, loc->inode,
                                     stub);

out:
        if (ret < 0) {
		STACK_UNWIND (frame, -1, -ret);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        } 

	return 0;
}


int
iot_link_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
              int32_t op_ret, int32_t op_errno, inode_t *inode,
              struct stat *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, inode, buf);
        return 0;
}


int
iot_link_wrapper (call_frame_t *frame, xlator_t *this, loc_t *old, loc_t *new)
{
        STACK_WIND (frame, iot_link_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->link, old, new);

        return 0;
}


int
iot_link (call_frame_t *frame, xlator_t *this, loc_t *oldloc, loc_t *newloc)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_link_stub (frame, iot_link_wrapper, oldloc, newloc);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create link stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                      oldloc->inode, stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_opendir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, fd_t *fd)
{
        STACK_UNWIND (frame, op_ret, op_errno, fd);
        return 0;
}


int
iot_opendir_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc, fd_t *fd)
{
        STACK_WIND (frame, iot_opendir_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->opendir, loc, fd);
        return 0;
}


int
iot_opendir (call_frame_t *frame, xlator_t *this, loc_t *loc, fd_t *fd)
{
        call_stub_t     *stub  = NULL;
        int             ret = -1;

        stub = fop_opendir_stub (frame, iot_opendir_wrapper, loc, fd);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create opendir stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_fsyncdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_fsyncdir_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                      int datasync)
{
        STACK_WIND (frame, iot_fsyncdir_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fsyncdir, fd, datasync);
        return 0;
}


int
iot_fsyncdir (call_frame_t *frame, xlator_t *this, fd_t *fd, int datasync)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fsyncdir_stub (frame, iot_fsyncdir_wrapper, fd, datasync);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fsyncdir stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_statfs_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct statvfs *buf)
{
        STACK_UNWIND (frame, op_ret, op_errno, buf);
        return 0;
}


int
iot_statfs_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
        STACK_WIND (frame, iot_statfs_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->statfs, loc);
        return 0;
}


int
iot_statfs (call_frame_t *frame, xlator_t *this, loc_t *loc)
{
        call_stub_t     *stub = NULL;
        int              ret = -1;

        stub = fop_statfs_stub (frame, iot_statfs_wrapper, loc);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create statfs stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_unordered ((iot_conf_t *)this->private, loc->inode,
                                      stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_setxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_setxattr_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                      dict_t *dict, int32_t flags)
{
        STACK_WIND (frame, iot_setxattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->setxattr, loc, dict, flags);
        return 0;
}


int
iot_setxattr (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *dict,
              int32_t flags)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int              ret = -1;

        stub = fop_setxattr_stub (frame, iot_setxattr_wrapper, loc, dict,
                                  flags);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create setxattr stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (fd == NULL)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_getxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        STACK_UNWIND (frame, op_ret, op_errno, dict);
        return 0;
}


int
iot_getxattr_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                      const char *name)
{
        STACK_WIND (frame, iot_getxattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->getxattr, loc, name);
        return 0;
}


int
iot_getxattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
              const char *name)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int             ret = -1;

        stub = fop_getxattr_stub (frame, iot_getxattr_wrapper, loc, name);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create getxattr stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (!fd)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_fgetxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno, dict_t *dict)
{
        STACK_UNWIND (frame, op_ret, op_errno, dict);
        return 0;
}


int
iot_fgetxattr_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                       const char *name)
{
        STACK_WIND (frame, iot_fgetxattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fgetxattr, fd, name);
        return 0;
}


int
iot_fgetxattr (call_frame_t *frame, xlator_t *this, fd_t *fd,
               const char *name)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fgetxattr_stub (frame, iot_fgetxattr_wrapper, fd, name);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fgetxattr stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_fsetxattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                   int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_fsetxattr_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                       dict_t *dict, int32_t flags)
{
        STACK_WIND (frame, iot_fsetxattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fsetxattr, fd, dict, flags);
        return 0;
}


int
iot_fsetxattr (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *dict,
               int32_t flags)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fsetxattr_stub (frame, iot_fsetxattr_wrapper, fd, dict,
                                        flags);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fsetxattr stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_removexattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                     int32_t op_ret, int32_t op_errno)
{
        STACK_UNWIND (frame, op_ret, op_errno);
        return 0;
}


int
iot_removexattr_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                         const char *name)
{
        STACK_WIND (frame, iot_removexattr_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->removexattr, loc, name);
        return 0;
}


int
iot_removexattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
                 const char *name)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int             ret = -1;

        stub = fop_removexattr_stub (frame, iot_removexattr_wrapper, loc,
                                     name);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR,"cannot get removexattr fop"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (!fd)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret);
                
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}

int
iot_readdir_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, gf_dirent_t *entries)
{
        STACK_UNWIND (frame, op_ret, op_errno, entries);
        return 0;
}


int
iot_readdir_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                     size_t size, off_t offset)
{
        STACK_WIND (frame, iot_readdir_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->readdir, fd, size, offset);
        return 0;
}


int
iot_readdir (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
             off_t offset)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_readdir_stub (frame, iot_readdir_wrapper, fd, size, offset);
        if (!stub) {
                gf_log (this->private, GF_LOG_ERROR,"cannot get readdir stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_xattrop_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
        STACK_UNWIND (frame, op_ret, op_errno, xattr);
        return 0;
}


int
iot_xattrop_wrapper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                     gf_xattrop_flags_t optype, dict_t *xattr)
{
        STACK_WIND (frame, iot_xattrop_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->xattrop, loc, optype, xattr);
        return 0;
}


int
iot_xattrop (call_frame_t *frame, xlator_t *this, loc_t *loc,
             gf_xattrop_flags_t optype, dict_t *xattr)
{
        call_stub_t     *stub = NULL;
        fd_t            *fd = NULL;
        int             ret = -1;

        stub = fop_xattrop_stub (frame, iot_xattrop_wrapper, loc, optype,
                                        xattr);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create xattrop stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        fd = fd_lookup (loc->inode, frame->root->pid);
        if (!fd)
                ret = iot_schedule_unordered ((iot_conf_t *)this->private,
                                              loc->inode, stub);
        else {
                ret = iot_schedule_ordered ((iot_conf_t *)this->private,
                                            loc->inode, stub);
                fd_unref (fd);
        }

out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);

                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


int
iot_fxattrop_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, dict_t *xattr)
{
        STACK_UNWIND (frame, op_ret, op_errno, xattr);
        return 0;
}

int
iot_fxattrop_wrapper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                      gf_xattrop_flags_t optype, dict_t *xattr)
{
        STACK_WIND (frame, iot_fxattrop_cbk, FIRST_CHILD (this),
                    FIRST_CHILD (this)->fops->fxattrop, fd, optype, xattr);
        return 0;
}

int
iot_fxattrop (call_frame_t *frame, xlator_t *this, fd_t *fd,
              gf_xattrop_flags_t optype, dict_t *xattr)
{
        call_stub_t     *stub = NULL;
        int             ret = -1;

        stub = fop_fxattrop_stub (frame, iot_fxattrop_wrapper, fd, optype,
                                        xattr);
        if (!stub) {
                gf_log (this->name, GF_LOG_ERROR, "cannot create fxattrop stub"
                        "(out of memory)");
                ret = -ENOMEM;
                goto out;
        }

        ret = iot_schedule_ordered ((iot_conf_t *)this->private, fd->inode,
                                    stub);
out:
        if (ret < 0) {
                STACK_UNWIND (frame, -1, -ret, NULL);
                if (stub != NULL) {
                        call_stub_destroy (stub);
                }
        }
        return 0;
}


/* Must be called with worker lock held */
void
_iot_queue (iot_worker_t *worker, iot_request_t *req)
{
        list_add_tail (&req->list, &worker->rqlist);

        /* dq_cond */
        worker->queue_size++;
        pthread_cond_broadcast (&worker->dq_cond);
}


iot_request_t *
iot_init_request (call_stub_t *stub)
{
	iot_request_t   *req = NULL;

        req = CALLOC (1, sizeof (iot_request_t));
        if (req == NULL) {
                goto out;
        }

        req->stub = stub;
out:
        return req;
}


void
iot_destroy_request (iot_request_t * req)
{
        if (req == NULL)
                return;

        FREE (req);
}


/* Must be called with worker lock held. */
gf_boolean_t
iot_can_ordered_exit (iot_worker_t * worker)
{
        gf_boolean_t     allow_exit = _gf_false;
        iot_conf_t      *conf = NULL;

        conf = worker->conf;
        /* We dont want this thread to exit if its index is
         * below the min thread count.
         */
        if (worker->thread_idx >= conf->min_o_threads)
                allow_exit = _gf_true;

        return allow_exit;
}

/* Must be called with worker lock held. */
gf_boolean_t
iot_ordered_exit (int cond_waitres, iot_worker_t *worker)
{
        gf_boolean_t     allow_exit = _gf_false;

        if (worker->state == IOT_STATE_EXIT_REQUEST) {
                allow_exit = 1;
        } else if (cond_waitres == ETIMEDOUT) {
                allow_exit = iot_can_ordered_exit (worker);
        }

        if (allow_exit) {
                worker->state = IOT_STATE_DEAD;
                worker->thread = 0;
        }

        return allow_exit;
}


int
iot_ordered_request_wait (iot_worker_t * worker)
{
        struct timeval  tv;
        struct timespec ts;
        int             waitres = 0;
        int             retstat = 0;

        if (worker->state == IOT_STATE_EXIT_REQUEST) {
                retstat = -1;
                goto out;
        }

        gettimeofday (&tv, NULL);
        ts.tv_sec = tv.tv_sec + worker->conf->o_idle_time;
        /* Slightly skew the idle time for threads so that, we dont
         * have all of them rushing to exit at the same time, if
         * they've been idle.
         */
        ts.tv_nsec = skew_usec_idle_time (tv.tv_usec) * 1000;
        waitres = pthread_cond_timedwait (&worker->dq_cond, &worker->qlock,
                                          &ts);
        if (iot_ordered_exit (waitres, worker)) {
                retstat = -1;
        }

out:
        return retstat;
}


call_stub_t *
iot_dequeue_ordered (iot_worker_t *worker)
{
	call_stub_t     *stub = NULL;
	iot_request_t   *req = NULL;
        int              waitstat = 0;

	pthread_mutex_lock (&worker->qlock);
        {
                while (!worker->queue_size) {
                        waitstat = 0;
                        waitstat = iot_ordered_request_wait (worker);
                        /* We must've timed out and are now required to
                         * exit.
                         */
                        if (waitstat == -1)
                                goto out;
                }

                list_for_each_entry (req, &worker->rqlist, list)
                        break;
                list_del (&req->list);
                stub = req->stub;

                worker->queue_size--;
        }
out:
	pthread_mutex_unlock (&worker->qlock);

	FREE (req);

	return stub;
}


void *
iot_worker_ordered (void *arg)
{
        iot_worker_t    *worker = arg;
        call_stub_t     *stub = NULL;

	while (1) {

                stub = iot_dequeue_ordered (worker);
                /* If stub is NULL, we must've timed out waiting for a
                 * request and have now been allowed to exit.
                 */
                if (!stub)
                        break;
		call_resume (stub);
	}

        return NULL;
}


/* Must be called with worker lock held. */
gf_boolean_t
iot_can_unordered_exit (iot_worker_t * worker)
{
        gf_boolean_t    allow_exit = _gf_false;
        iot_conf_t      *conf = NULL;

        conf = worker->conf;
        /* We dont want this thread to exit if its index is
         * below the min thread count.
         */
        if (worker->thread_idx >= conf->min_u_threads)
                allow_exit = _gf_true;

        return allow_exit;
}


/* Must be called with worker lock held. */
gf_boolean_t
iot_unordered_exit (int cond_waitres, iot_worker_t *worker)
{
        gf_boolean_t     allow_exit = _gf_false;

        if (worker->state == IOT_STATE_EXIT_REQUEST) {
                allow_exit = 1;
        } else if (cond_waitres == ETIMEDOUT) {
                allow_exit = iot_can_unordered_exit (worker);
        }

        if (allow_exit) {
                worker->state = IOT_STATE_DEAD;
                worker->thread = 0;
        }

        return allow_exit;
}


int
iot_unordered_request_wait (iot_worker_t * worker)
{
        struct timeval  tv;
        struct timespec ts;
        int             waitres = 0;
        int             retstat = 0;

        if (worker->state == IOT_STATE_EXIT_REQUEST) {
                retstat = -1;
                goto out;
        }

        gettimeofday (&tv, NULL);
        ts.tv_sec = tv.tv_sec + worker->conf->u_idle_time;
        /* Slightly skew the idle time for threads so that, we dont
         * have all of them rushing to exit at the same time, if
         * they've been idle.
         */
        ts.tv_nsec = skew_usec_idle_time (tv.tv_usec) * 1000;
        waitres = pthread_cond_timedwait (&worker->dq_cond, &worker->qlock,
                                          &ts);
        if (iot_unordered_exit (waitres, worker)) {
                retstat = -1;
        }

out:
        return retstat;
}


call_stub_t *
iot_dequeue_unordered (iot_worker_t *worker)
{
        call_stub_t     *stub= NULL;
        iot_request_t   *req = NULL;
        int              waitstat = 0;

	pthread_mutex_lock (&worker->qlock);
        {
                while (!worker->queue_size) {
                        waitstat = 0;
                        waitstat = iot_unordered_request_wait (worker);
                        /* If -1, request wait must've timed
                         * out.
                         */
                        if (waitstat == -1)
                                goto out;
                }

                list_for_each_entry (req, &worker->rqlist, list)
                        break;
                list_del (&req->list);
                stub = req->stub;

                worker->queue_size--;
        }
out:
	pthread_mutex_unlock (&worker->qlock);

	FREE (req);

	return stub;
}


void *
iot_worker_unordered (void *arg)
{
        iot_worker_t    *worker = arg;
        call_stub_t     *stub = NULL;

	while (1) {

		stub = iot_dequeue_unordered (worker);
                /* If no request was received, we must've timed out,
                 * and can exit. */
                if (!stub)
                        break;

		call_resume (stub);
	}

        return NULL;
}


void
deallocate_worker_array (iot_worker_t **workers)
{
        FREE (workers);
}

void
deallocate_workers (iot_worker_t **workers,
                    int start_alloc_idx, int count)
{
        int     i;
        int     end_count;

        end_count = count + start_alloc_idx;
        for (i = start_alloc_idx; (i < end_count); i++) {
                if (workers[i] != NULL) {
                        free (workers[i]);
                        workers[i] = NULL;
                }
        }
        
}


iot_worker_t **
allocate_worker_array (int count)
{
        iot_worker_t    **warr = NULL;

        warr = CALLOC (count, sizeof (iot_worker_t *));

        return warr;
}


iot_worker_t *
allocate_worker (iot_conf_t * conf)
{
        iot_worker_t    *wrk = NULL;

        wrk = CALLOC (1, sizeof (iot_worker_t));
        if (wrk == NULL) {
                gf_log (conf->this->name, GF_LOG_ERROR, "out of memory");
                goto out;
        }

        INIT_LIST_HEAD (&wrk->rqlist);
        wrk->conf = conf;
        pthread_cond_init (&wrk->dq_cond, NULL);
        pthread_mutex_init (&wrk->qlock, NULL);
        wrk->state = IOT_STATE_DEAD;

out:
        return wrk;
}


int
allocate_workers (iot_conf_t *conf, iot_worker_t **workers, int start_alloc_idx,
                  int count)
{
        int     i;
        int     end_count, ret = -1;

        end_count = count + start_alloc_idx;
        for (i = start_alloc_idx; i < end_count; i++) {
                workers[i] = allocate_worker (conf);
                if (workers[i] == NULL) {
                        ret = -ENOMEM;
                        goto out;
                }
                workers[i]->thread_idx = i;
        }
        ret = 0;

out:
        return ret;
}


void
iot_stop_worker (iot_worker_t *worker)
{
        pthread_mutex_lock (&worker->qlock);
        {
                worker->state = IOT_STATE_EXIT_REQUEST;
        }
        pthread_mutex_unlock (&worker->qlock);

        pthread_cond_broadcast (&worker->dq_cond);
        pthread_join (worker->thread, NULL);
}


void
iot_stop_workers (iot_worker_t **workers, int start_idx, int count)
{
        int     i = 0;
        int     end_idx = 0;

        end_idx = start_idx + count;
        for (i = start_idx; i < end_idx; i++) {
                if (workers[i] != NULL) {
                        iot_stop_worker (workers[i]);
                }
        }
}


int
iot_startup_worker (iot_worker_t *worker, iot_worker_fn workerfunc)
{
        int     ret = -1;
        ret = pthread_create (&worker->thread, &worker->conf->w_attr,
                              workerfunc, worker);
        if (ret != 0) {
                gf_log (worker->conf->this->name, GF_LOG_ERROR,
                        "cannot start worker (%s)", strerror (errno));
                ret = -ret;
        } else {
                worker->state = IOT_STATE_ACTIVE;
        }

        return ret;
}


int
iot_startup_workers (iot_worker_t **workers, int start_idx, int count,
                     iot_worker_fn workerfunc)
{
        int     i = 0;
        int     end_idx = 0;
        int     ret = -1; 

        end_idx = start_idx + count;
        for (i = start_idx; i < end_idx; i++) {
                ret = iot_startup_worker (workers[i], workerfunc);
                if (ret < 0) {
                        goto out;
                }
        }

        ret = 0;
out:
        return ret;
}


void
set_stack_size (iot_conf_t *conf)
{
        int     err = 0;
        size_t  stacksize = IOT_THREAD_STACK_SIZE;

        pthread_attr_init (&conf->w_attr);
        err = pthread_attr_setstacksize (&conf->w_attr, stacksize);
        if (err == EINVAL) {
                gf_log (conf->this->name, GF_LOG_WARNING,
                                "Using default thread stack size");
        }
}


void
iot_cleanup_workers (iot_conf_t *conf)
{
        if (conf->uworkers != NULL) {
                iot_stop_workers (conf->uworkers, 0,
                                  conf->max_u_threads);
                    
                deallocate_workers (conf->uworkers, 0,
                                    conf->max_u_threads);

                deallocate_worker_array (conf->uworkers);
        }
                
        if (conf->oworkers != NULL) {
                iot_stop_workers (conf->oworkers, 0,
                                  conf->max_o_threads);
                        
                deallocate_workers (conf->oworkers, 0,
                                    conf->max_o_threads);
                        
                deallocate_worker_array (conf->oworkers);
        }
}


int
workers_init (iot_conf_t *conf)
{
        int     ret = -1;

        if (conf == NULL) {
                ret = -EINVAL;
                goto err;
        }

        /* Initialize un-ordered workers */
        conf->uworkers = allocate_worker_array (conf->max_u_threads);
        if (conf->uworkers == NULL) {
                gf_log (conf->this->name, GF_LOG_ERROR, "out of memory");
                ret = -ENOMEM;
                goto err;
        }

        ret = allocate_workers (conf, conf->uworkers, 0,
                                conf->max_u_threads);
        if (ret < 0) {
                gf_log (conf->this->name, GF_LOG_ERROR, "out of memory");
                goto err;
        }

        /* Initialize ordered workers */
        conf->oworkers = allocate_worker_array (conf->max_o_threads);
        if (conf->oworkers == NULL) {
                gf_log (conf->this->name, GF_LOG_ERROR, "out of memory");
                ret = -ENOMEM;
                goto err;
        }

        ret = allocate_workers (conf, conf->oworkers, 0,
                                conf->max_o_threads);
        if (ret < 0) {
                gf_log (conf->this->name, GF_LOG_ERROR, "out of memory");
                goto err;
        }

        set_stack_size (conf);
        ret = iot_startup_workers (conf->oworkers, 0, conf->min_o_threads,
                                   iot_worker_ordered);
        if (ret == -1) {
                /* logged inside iot_startup_workers */
                goto err;
        }

        ret = iot_startup_workers (conf->uworkers, 0, conf->min_u_threads,
                                   iot_worker_unordered);
        if (ret == -1) {
                /* logged inside iot_startup_workers */
                goto err;
        }

        return 0;

err:
        if (conf != NULL)  {
                iot_cleanup_workers (conf);
        }

        return ret;
}


int
init (xlator_t *this)
{
        iot_conf_t      *conf = NULL;
        dict_t          *options = this->options;
        int             thread_count = IOT_DEFAULT_THREADS;
        gf_boolean_t    autoscaling = IOT_SCALING_OFF;
        char            *scalestr = NULL;
        int             min_threads, max_threads, ret = -1;
        
	if (!this->children || this->children->next) {
		gf_log ("io-threads", GF_LOG_ERROR,
			"FATAL: iot not configured with exactly one child");
                goto out;
	}

	if (!this->parents) {
		gf_log (this->name, GF_LOG_WARNING,
			"dangling volume. check volfile ");
	}

	conf = (void *) CALLOC (1, sizeof (*conf));
        if (conf == NULL) {
                gf_log (this->name, GF_LOG_ERROR,
                        "out of memory");
                goto out;
        }

        if ((dict_get_str (options, "autoscaling", &scalestr)) == 0) {
                if ((gf_string2boolean (scalestr, &autoscaling)) == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                        "'autoscaling' option must be"
                                        " boolean");
                        goto out;
                }
        }

	if (dict_get (options, "thread-count")) {
                thread_count = data_to_int32 (dict_get (options,
                                        "thread-count"));
                if (scalestr != NULL)
                        gf_log (this->name, GF_LOG_WARNING,
                                        "'thread-count' is specified with "
                                        "'autoscaling' on. Ignoring"
                                        "'thread-count' option.");
                if (thread_count < 2)
                        thread_count = IOT_MIN_THREADS;
        }

        min_threads = IOT_DEFAULT_THREADS;
        max_threads = IOT_MAX_THREADS;
        if (dict_get (options, "min-threads"))
                min_threads = data_to_int32 (dict_get (options,
                                        "min-threads"));

        if (dict_get (options, "max-threads"))
                max_threads = data_to_int32 (dict_get (options,
                                        "max-threads"));
       
        if (min_threads > max_threads) {
                gf_log (this->name, GF_LOG_ERROR, " min-threads must be less "
                                "than max-threads");
                goto out;
        }

        /* If autoscaling is off, then adjust the min and max
         * threads according to thread-count.
         * This is based on the assumption that despite autoscaling
         * being off, we still want to have separate pools for data
         * and meta-data threads.
         */
        if (!autoscaling)
                max_threads = min_threads = thread_count;

        /* If user specifies an odd number of threads, increase it by
         * one. The reason for having an even number of threads is
         * explained later.
         */
        if (max_threads % 2)
                max_threads++;

        if(min_threads % 2)
                min_threads++;

        /* If the user wants to have only a single thread for
        * some strange reason, make sure we set this count to
        * 2. Explained later.
        */
        if (min_threads < IOT_MIN_THREADS)
                min_threads = IOT_MIN_THREADS;

        /* Again, have atleast two. Read on. */
        if (max_threads < IOT_MIN_THREADS)
                max_threads = IOT_MIN_THREADS;

        /* This is why we need atleast two threads.
         * We're dividing the specified thread pool into
         * 2 halves, equally between ordered and unordered
         * pools.
         */

        /* Init params for un-ordered workers. */
        pthread_mutex_init (&conf->utlock, NULL);
        conf->max_u_threads = max_threads / 2;
        conf->min_u_threads = min_threads / 2;
        conf->u_idle_time = IOT_DEFAULT_IDLE;
        conf->u_scaling = autoscaling;

        /* Init params for ordered workers. */
        pthread_mutex_init (&conf->otlock, NULL);
        conf->max_o_threads = max_threads / 2;
        conf->min_o_threads = min_threads / 2;
        conf->o_idle_time = IOT_DEFAULT_IDLE;
        conf->o_scaling = autoscaling;

        gf_log (this->name, GF_LOG_DEBUG,
                "io-threads: Autoscaling: %s, "
                "min_threads: %d, max_threads: %d",
                (autoscaling) ? "on":"off", min_threads, max_threads);

        conf->this = this;
	ret = workers_init (conf);
        if (ret == -1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "cannot initialize worker threads, exiting init");
                FREE (conf);
                goto out;
        }

	this->private = conf;
        ret = 0;
out:
	return ret;
}


void
fini (xlator_t *this)
{
	iot_conf_t *conf = this->private;

	FREE (conf);

	this->private = NULL;
	return;
}

/*
 * O - Goes to ordered threadpool.
 * U - Goes to un-ordered threadpool.
 * V - Variable, depends on whether the file is open.
 *     If it is, then goes to ordered, otherwise to
 *     un-ordered.
 */
struct xlator_fops fops = {
	.open        = iot_open,        /* U */
	.create      = iot_create,      /* U */
	.readv       = iot_readv,       /* O */
	.writev      = iot_writev,      /* O */
	.flush       = iot_flush,       /* O */
	.fsync       = iot_fsync,       /* O */
	.lk          = iot_lk,          /* O */
	.stat        = iot_stat,        /* V */
	.fstat       = iot_fstat,       /* O */
	.truncate    = iot_truncate,    /* V */
	.ftruncate   = iot_ftruncate,   /* O */
	.utimens     = iot_utimens,     /* V */
	.checksum    = iot_checksum,    /* U */
	.unlink      = iot_unlink,      /* U */
        .lookup      = iot_lookup,      /* U */
        .chmod       = iot_chmod,       /* V */
        .fchmod      = iot_fchmod,      /* O */
        .chown       = iot_chown,       /* V */
        .fchown      = iot_fchown,      /* O */
        .access      = iot_access,      /* U */
        .readlink    = iot_readlink,    /* U */
        .mknod       = iot_mknod,       /* U */
        .mkdir       = iot_mkdir,       /* U */
        .rmdir       = iot_rmdir,       /* U */
        .symlink     = iot_symlink,     /* U */
        .rename      = iot_rename,      /* U */
        .link        = iot_link,        /* U */
        .opendir     = iot_opendir,     /* U */
        .fsyncdir    = iot_fsyncdir,    /* O */
        .statfs      = iot_statfs,      /* U */
        .setxattr    = iot_setxattr,     /* U */
        .getxattr    = iot_getxattr,    /* U */
        .fgetxattr   = iot_fgetxattr,   /* O */
        .fsetxattr   = iot_fsetxattr,   /* O */
        .removexattr = iot_removexattr, /* U */
        .readdir     = iot_readdir,     /* O */
        .xattrop     = iot_xattrop,     /* U */
};

struct xlator_mops mops = {
};

struct xlator_cbks cbks = {
};

struct volume_options options[] = {
	{ .key  = {"thread-count"}, 
	  .type = GF_OPTION_TYPE_INT, 
	  .min  = IOT_MIN_THREADS, 
	  .max  = IOT_MAX_THREADS
	},
        { .key  = {"autoscaling"},
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key          = {"min-threads"},
          .type         = GF_OPTION_TYPE_INT,
          .min          = IOT_MIN_THREADS,
          .max          = IOT_MAX_THREADS,
          .description  = "Minimum number of threads must be greater than or "
                          "equal to 2. If the specified value is less than 2 "
                          "it is adjusted upwards to 2. This is a requirement"
                          " for the current model of threading in io-threads."
        },
        { .key          = {"max-threads"},
          .type         = GF_OPTION_TYPE_INT,
          .min          = IOT_MIN_THREADS,
          .max          = IOT_MAX_THREADS,
          .description  = "Maximum number of threads is advisory only so the "
                          "user specified value will be used."
        },
	{ .key  = {NULL} },
};
