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

/*TODO: check for non null wb_file_data before getting wb_file */


#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"
#include "list.h"
#include "compat.h"
#include "compat-errno.h"
#include "common-utils.h"
#include "call-stub.h"

#define MAX_VECTOR_COUNT 8
 
typedef struct list_head list_head_t;
struct wb_conf;
struct wb_page;
struct wb_file;


typedef struct wb_file {
        int          disabled;
        uint64_t     disable_till;
        size_t       window_size;
        int32_t      refcount;
        int32_t      op_ret;
        int32_t      op_errno;
        list_head_t  request;
        fd_t        *fd;
        gf_lock_t    lock;
        xlator_t    *this;
}wb_file_t;


typedef struct wb_request {
        list_head_t  list;
        list_head_t  winds;
        list_head_t  unwinds;
        list_head_t  other_requests;
        call_stub_t *stub;
        int32_t      refcount;
        wb_file_t   *file;
        union {
                struct  {
                        char write_behind;
                        char stack_wound;
                        char got_reply;
                }write_request;
                
                struct {
                        char marked_for_resume;
                }other_requests;
        }flags;
} wb_request_t;


struct wb_conf {
        uint64_t     aggregate_size;
        uint64_t     window_size;
        uint64_t     disable_till;
        gf_boolean_t enable_O_SYNC;
        gf_boolean_t flush_behind;
};


typedef struct wb_local {
        list_head_t     winds;
        struct wb_file *file;
        wb_request_t   *request;
        int             op_ret;
        int             op_errno;
        call_frame_t   *frame;
        int32_t         reply_count;
} wb_local_t;


typedef struct wb_conf wb_conf_t;
typedef struct wb_page wb_page_t;


int32_t 
wb_process_queue (call_frame_t *frame, wb_file_t *file, char flush_all);

size_t
wb_sync (call_frame_t *frame, wb_file_t *file, list_head_t *winds);

size_t
__wb_mark_winds (list_head_t *list, list_head_t *winds, size_t aggregate_size);


static void
__wb_request_unref (wb_request_t *this)
{
        if (this->refcount <= 0) {
                gf_log ("wb-request", GF_LOG_ERROR,
                        "refcount(%d) is <= 0", this->refcount);
                return;
        }

        this->refcount--;
        if (this->refcount == 0) {
                list_del_init (&this->list);
                if (this->stub && this->stub->fop == GF_FOP_WRITE) {
                        call_stub_destroy (this->stub);
                }

                FREE (this);
        }
}


static void
wb_request_unref (wb_request_t *this)
{
        wb_file_t *file = NULL;
        if (this == NULL) {
                gf_log ("wb-request", GF_LOG_DEBUG,
                        "request is NULL");
                return;
        }
        
        file = this->file;
        LOCK (&file->lock);
        {
                __wb_request_unref (this);
        }
        UNLOCK (&file->lock);
}


static wb_request_t *
__wb_request_ref (wb_request_t *this)
{
        if (this->refcount < 0) {
                gf_log ("wb-request", GF_LOG_DEBUG,
                        "refcount(%d) is < 0", this->refcount);
                return NULL;
        }

        this->refcount++;
        return this;
}


wb_request_t *
wb_request_ref (wb_request_t *this)
{
        wb_file_t *file = NULL;
        if (this == NULL) {
                gf_log ("wb-request", GF_LOG_DEBUG,
                        "request is NULL");
                return NULL;
        }

        file = this->file;
        LOCK (&file->lock);
        {
                this = __wb_request_ref (this);
        }
        UNLOCK (&file->lock);

        return this;
}


wb_request_t *
wb_enqueue (wb_file_t *file, 
            call_stub_t *stub)
{
        wb_request_t *request = NULL;
        call_frame_t *frame = NULL;
        wb_local_t   *local = NULL;
        struct iovec *vector = NULL;
        int32_t       count = 0;
                        
        request = CALLOC (1, sizeof (*request));

        INIT_LIST_HEAD (&request->list);
        INIT_LIST_HEAD (&request->winds);
        INIT_LIST_HEAD (&request->unwinds);
        INIT_LIST_HEAD (&request->other_requests);

        request->stub = stub;
        request->file = file;

        frame = stub->frame;
        local = frame->local;
        if (local) {
                local->request = request;
        }

        if (stub->fop == GF_FOP_WRITE) {
                vector = stub->args.writev.vector;
                count = stub->args.writev.count;

                frame = stub->frame;
                local = frame->local;
                local->op_ret = iov_length (vector, count);
                local->op_errno = 0;
        }

        LOCK (&file->lock);
        {
                list_add_tail (&request->list, &file->request);
                if (stub->fop == GF_FOP_WRITE) {
                        /* reference for stack winding */
                        __wb_request_ref (request);

                        /* reference for stack unwinding */
                        __wb_request_ref (request);
                } else {
                        /*reference for resuming */
                        __wb_request_ref (request);
                }
        }
        UNLOCK (&file->lock);

        return request;
}


wb_file_t *
wb_file_create (xlator_t *this,
                fd_t *fd)
{
        wb_file_t *file = NULL;
        wb_conf_t *conf = this->private; 

        file = CALLOC (1, sizeof (*file));
        INIT_LIST_HEAD (&file->request);

        /* fd_ref() not required, file should never decide the existance of
         * an fd */
        file->fd= fd;
        file->disable_till = conf->disable_till;
        file->this = this;
        file->refcount = 1;

        fd_ctx_set (fd, this, (uint64_t)(long)file);
        
        return file;
}

void
wb_file_destroy (wb_file_t *file)
{
        int32_t refcount = 0;

        LOCK (&file->lock);
        {
                refcount = --file->refcount;
        }
        UNLOCK (&file->lock);

        if (!refcount){
                LOCK_DESTROY (&file->lock);
                FREE (file);
        }

        return;
}


int32_t
wb_sync_cbk (call_frame_t *frame,
             void *cookie,
             xlator_t *this,
             int32_t op_ret,
             int32_t op_errno,
             struct stat *stbuf)
{
        wb_local_t   *local = NULL;
        list_head_t  *winds = NULL;
        wb_file_t    *file = NULL;
        wb_request_t *request = NULL, *dummy = NULL;
        wb_local_t   *per_request_local = NULL;


        local = frame->local;
        winds = &local->winds;
        file = local->file;

        LOCK (&file->lock);
        {
                list_for_each_entry_safe (request, dummy, winds, winds) {
                        request->flags.write_request.got_reply = 1;

                        if (!request->flags.write_request.write_behind
                            && (op_ret == -1)) {
                                per_request_local = request->stub->frame->local;
                                per_request_local->op_ret = op_ret;
                                per_request_local->op_errno = op_errno;
                        }

                        __wb_request_unref (request);
                }
                
                if (op_ret == -1) {
                        file->op_ret = op_ret;
                        file->op_errno = op_errno;
                }
        }
        UNLOCK (&file->lock);

        wb_process_queue (frame, file, 0);  
  
        /* safe place to do fd_unref */
        fd_unref (file->fd);

        STACK_DESTROY (frame->root);

        return 0;
}


size_t
wb_sync (call_frame_t *frame, wb_file_t *file, list_head_t *winds)
{
        wb_request_t   *dummy = NULL, *request = NULL;
        wb_request_t   *first_request = NULL, *next = NULL;
        size_t          total_count = 0, count = 0;
        size_t          copied = 0;
        call_frame_t   *sync_frame = NULL;
        dict_t         *refs = NULL;
        wb_local_t     *local = NULL;
        struct iovec   *vector = NULL;
        size_t          bytes = 0;
        size_t          bytecount = 0;

        list_for_each_entry (request, winds, winds) {
                total_count += request->stub->args.writev.count;
                bytes += iov_length (request->stub->args.writev.vector,
                                     request->stub->args.writev.count);
        }

        if (!total_count) {
                return 0;
        }
  
        list_for_each_entry_safe (request, dummy, winds, winds) {
                if (!vector) {
                        vector = MALLOC (VECTORSIZE (MAX_VECTOR_COUNT));
                        refs = get_new_dict ();
        
                        local = CALLOC (1, sizeof (*local));
                        INIT_LIST_HEAD (&local->winds);
            
                        first_request = request;
                }

                count += request->stub->args.writev.count;
                bytecount = VECTORSIZE (request->stub->args.writev.count);
                memcpy (((char *)vector)+copied,
                        request->stub->args.writev.vector,
                        bytecount);
                copied += bytecount;
      
                if (request->stub->args.writev.req_refs) {
                        dict_copy (request->stub->args.writev.req_refs, refs);
                }

                next = NULL;
                if (request->winds.next != winds) {    
                        next = list_entry (request->winds.next,
                                           wb_request_t, winds);
                }

                list_del_init (&request->winds);
                list_add_tail (&request->winds, &local->winds);

                if ((!next)
                    || ((count + next->stub->args.writev.count) > MAX_VECTOR_COUNT))
                {
                        sync_frame = copy_frame (frame);  
                        sync_frame->local = local;
                        local->file = file;
                        sync_frame->root->req_refs = dict_ref (refs);
                        fd_ref (file->fd);
                        STACK_WIND (sync_frame,
                                    wb_sync_cbk,
                                    FIRST_CHILD(sync_frame->this),
                                    FIRST_CHILD(sync_frame->this)->fops->writev,
                                    file->fd, vector,
                                    count,
                                    first_request->stub->args.writev.off);
        
                        dict_unref (refs);
                        FREE (vector);
                        first_request = NULL;
                        refs = NULL;
                        vector = NULL;
                        copied = count = 0;
                }
        }

        return bytes;
}


int32_t 
wb_stat_cbk (call_frame_t *frame,
             void *cookie,
             xlator_t *this,
             int32_t op_ret,
             int32_t op_errno,
             struct stat *buf)
{
        wb_local_t   *local = NULL;
        wb_request_t *request = NULL; 
        call_frame_t *process_frame = NULL;
        wb_file_t    *file = NULL;
  
        local = frame->local;
        file = local->file;

        request = local->request;
        if (request) {
                process_frame = copy_frame (frame);
        }

        STACK_UNWIND (frame, op_ret, op_errno, buf);

        if (request) {
                wb_request_unref (request);
                wb_process_queue (process_frame, file, 0);
                STACK_DESTROY (process_frame->root);
        }

        if (file) {
                fd_unref (file->fd);
        }

        return 0;
}


static int32_t
wb_stat_helper (call_frame_t *frame,
                xlator_t *this,
                loc_t *loc)
{
        STACK_WIND (frame, wb_stat_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->stat,
                    loc);
        return 0;
}


int32_t
wb_stat (call_frame_t *frame,
         xlator_t *this,
         loc_t *loc)
{
        wb_file_t   *file = NULL;
        fd_t        *iter_fd = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (loc->inode) {
                /* FIXME: fd_lookup extends life of fd till stat returns */
                iter_fd = fd_lookup (loc->inode, frame->root->pid);
                if (iter_fd) {
                        if (!fd_ctx_get (iter_fd, this, &tmp_file)) {
				file = (wb_file_t *)(long)tmp_file;
                        } else {
                                fd_unref (iter_fd);
                        }
                }
        }

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;

        if (file) {
                stub = fop_stat_stub (frame, wb_stat_helper, loc);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }
                
                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame, wb_stat_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->stat,
                            loc);
        }

        return 0;
}


int32_t 
wb_fstat_cbk (call_frame_t *frame,
              void *cookie,
              xlator_t *this,
              int32_t op_ret,
              int32_t op_errno,
              struct stat *buf)
{
        wb_local_t   *local = NULL;
        wb_request_t *request = NULL; 
        wb_file_t    *file = NULL;
  
        local = frame->local;
        file = local->file;

        request = local->request;
        if (request) {
                wb_request_unref (request);
                wb_process_queue (frame, file, 0);
        }

        STACK_UNWIND (frame, op_ret, op_errno, buf);

        return 0;
}


int32_t 
wb_fstat_helper (call_frame_t *frame,
                 xlator_t *this,
                 fd_t *fd)
{
        STACK_WIND (frame,
                    wb_fstat_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fstat,
                    fd);
        return 0;
}


int32_t 
wb_fstat (call_frame_t *frame,
          xlator_t *this,
          fd_t *fd)
{
        wb_file_t   *file = NULL;
        wb_local_t  *local = NULL;
  	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD, NULL);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;
        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;

        if (file) {
                stub = fop_fstat_stub (frame, wb_fstat_helper, fd);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }
                
                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame,
                            wb_fstat_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->fstat,
                            fd);
        }

        return 0;
}


int32_t
wb_truncate_cbk (call_frame_t *frame,
                 void *cookie,
                 xlator_t *this,
                 int32_t op_ret,
                 int32_t op_errno,
                 struct stat *buf)
{
        wb_local_t   *local = NULL; 
        wb_request_t *request = NULL;
        wb_file_t    *file = NULL;
        call_frame_t *process_frame = NULL;

        local = frame->local;
        file = local->file;
        request = local->request;

        if (request) {
                process_frame = copy_frame (frame);
        }

        STACK_UNWIND (frame, op_ret, op_errno, buf);

        if (request) {
                wb_request_unref (request);
                wb_process_queue (process_frame, file, 0);
                STACK_DESTROY (process_frame->root);
        }

        if (file) {
                fd_unref (file->fd);
        }

        return 0;
}


static int32_t 
wb_truncate_helper (call_frame_t *frame,
                    xlator_t *this,
                    loc_t *loc,
                    off_t offset)
{
        STACK_WIND (frame,
                    wb_truncate_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->truncate,
                    loc,
                    offset);

        return 0;
}


int32_t 
wb_truncate (call_frame_t *frame,
             xlator_t *this,
             loc_t *loc,
             off_t offset)
{
        wb_file_t   *file = NULL;
        fd_t        *iter_fd = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (loc->inode)
        {
                /* FIXME: fd_lookup extends life of fd till the execution of
                   truncate_cbk */
                iter_fd = fd_lookup (loc->inode, frame->root->pid);
                if (iter_fd) {
                        if (!fd_ctx_get (iter_fd, this, &tmp_file)){
				file = (wb_file_t *)(long)tmp_file;
                        } else {
                                fd_unref (iter_fd);
                        }
                }
        }
  
        local = CALLOC (1, sizeof (*local));
        local->file = file;
        
        frame->local = local;
        if (file) {
                stub = fop_truncate_stub (frame, wb_truncate_helper, loc,
                                          offset);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }

                wb_enqueue (file, stub);
                
                wb_process_queue (frame, file, 1);

        } else {
                STACK_WIND (frame,
                            wb_truncate_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->truncate,
                            loc,
                            offset);
        }

        return 0;
}


int32_t
wb_ftruncate_cbk (call_frame_t *frame,
                  void *cookie,
                  xlator_t *this,
                  int32_t op_ret,
                  int32_t op_errno,
                  struct stat *buf)
{
        wb_local_t   *local = NULL; 
        wb_request_t *request = NULL;
        wb_file_t    *file = NULL;

        local = frame->local;
        file = local->file;
        request = local->request;

        if (request) {
                wb_request_unref (request);
                wb_process_queue (frame, file, 0);
        }

        STACK_UNWIND (frame, op_ret, op_errno, buf);

        return 0;
}


static int32_t
wb_ftruncate_helper (call_frame_t *frame,
                     xlator_t *this,
                     fd_t *fd,
                     off_t offset)
{
        STACK_WIND (frame,
                    wb_ftruncate_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->ftruncate,
                    fd,
                    offset);
        return 0;
}

        
int32_t
wb_ftruncate (call_frame_t *frame,
              xlator_t *this,
              fd_t *fd,
              off_t offset)
{
        wb_file_t   *file = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD, NULL);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;

        if (file) {
                stub = fop_ftruncate_stub (frame, wb_ftruncate_helper, fd,
                                           offset);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }

                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame,
                            wb_ftruncate_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->ftruncate,
                            fd,
                            offset);
        }

        return 0;
}


int32_t 
wb_utimens_cbk (call_frame_t *frame,
                void *cookie,
                xlator_t *this,
                int32_t op_ret,
                int32_t op_errno,
                struct stat *buf)
{
        wb_local_t   *local = NULL;       
        wb_request_t *request = NULL;
        call_frame_t *process_frame = NULL; 
        wb_file_t    *file = NULL;
  
        local = frame->local;
        file = local->file;
        request = local->request;

        if (request) {
                process_frame = copy_frame (frame);
        }

        STACK_UNWIND (frame, op_ret, op_errno, buf);

        if (request) {
                wb_request_unref (request);
                wb_process_queue (process_frame, file, 0);
                STACK_DESTROY (process_frame->root);
        }

        if (file) {
                fd_unref (file->fd);
        }

        return 0;
}


static int32_t 
wb_utimens_helper (call_frame_t *frame,
                   xlator_t *this,
                   loc_t *loc,
                   struct timespec tv[2])
{
        STACK_WIND (frame,
                    wb_utimens_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->utimens,
                    loc,
                    tv);

        return 0;
}


int32_t 
wb_utimens (call_frame_t *frame,
            xlator_t *this,
            loc_t *loc,
            struct timespec tv[2])
{
        wb_file_t   *file = NULL;
        fd_t        *iter_fd = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (loc->inode) {
                /* FIXME: fd_lookup extends life of fd till the execution
                   of wb_utimens_cbk */
                iter_fd = fd_lookup (loc->inode, frame->root->pid);
                if (iter_fd) {
                        if (!fd_ctx_get (iter_fd, this, &tmp_file)) {
				file = (wb_file_t *)(long)tmp_file;
                        } else {
                                fd_unref (iter_fd);
                        }
                }

        }

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;

        if (file) {
                stub = fop_utimens_stub (frame, wb_utimens_helper, loc, tv);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }

                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame,
                            wb_utimens_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->utimens,
                            loc,
                            tv);
        }

        return 0;
}

int32_t
wb_open_cbk (call_frame_t *frame,
             void *cookie,
             xlator_t *this,
             int32_t op_ret,
             int32_t op_errno,
             fd_t *fd)
{
        int32_t    flags = 0;
        wb_file_t *file = NULL;
        wb_conf_t *conf = this->private;

        if (op_ret != -1)
        {
                file = wb_file_create (this, fd);

                /* If mandatory locking has been enabled on this file,
                   we disable caching on it */

                if ((fd->inode->st_mode & S_ISGID)
                    && !(fd->inode->st_mode & S_IXGRP))
                        file->disabled = 1;

                /* If O_DIRECT then, we disable chaching */
                if (frame->local)
                {
                        flags = *((int32_t *)frame->local);
                        if (((flags & O_DIRECT) == O_DIRECT)
                            || ((flags & O_RDONLY) == O_RDONLY)
                            || (((flags & O_SYNC) == O_SYNC)
                            && conf->enable_O_SYNC == _gf_true)) { 
                                file->disabled = 1;
                        }
                }

                LOCK_INIT (&file->lock);
        }

        STACK_UNWIND (frame, op_ret, op_errno, fd);
        return 0;
}


int32_t
wb_open (call_frame_t *frame,
         xlator_t *this,
         loc_t *loc,
         int32_t flags,
         fd_t *fd)
{
        frame->local = CALLOC (1, sizeof(int32_t));
        *((int32_t *)frame->local) = flags;

        STACK_WIND (frame,
                    wb_open_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->open,
                    loc, flags, fd);
        return 0;
}


int32_t
wb_create_cbk (call_frame_t *frame,
               void *cookie,
               xlator_t *this,
               int32_t op_ret,
               int32_t op_errno,
               fd_t *fd,
               inode_t *inode,
               struct stat *buf)
{
        wb_file_t *file = NULL;

        if (op_ret != -1)
        {
                file = wb_file_create (this, fd);
                /* 
                 * If mandatory locking has been enabled on this file,
                 * we disable caching on it
                 */
                if ((fd->inode->st_mode & S_ISGID)
                    && !(fd->inode->st_mode & S_IXGRP))
                {
                        file->disabled = 1;
                }

                LOCK_INIT (&file->lock);
        }

        STACK_UNWIND (frame, op_ret, op_errno, fd, inode, buf);
        return 0;
}


int32_t
wb_create (call_frame_t *frame,
           xlator_t *this,
           loc_t *loc,
           int32_t flags,
           mode_t mode,
           fd_t *fd)
{
        STACK_WIND (frame,
                    wb_create_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->create,
                    loc, flags, mode, fd);
        return 0;
}


size_t
__wb_mark_wind_all (list_head_t *list, list_head_t *winds)
{
        wb_request_t *request = NULL;
        size_t        size = 0;
        struct iovec *vector = NULL;
        int32_t       count = 0;
        char          first_request = 1;
        off_t         offset_expected = 0;
        size_t        length = 0;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        break;
                }

                vector = request->stub->args.writev.vector;
                count = request->stub->args.writev.count;
                if (!request->flags.write_request.stack_wound) {
                        if (first_request) {
                                first_request = 0;
                                offset_expected = request->stub->args.writev.off;
                        }
                        
                        if (request->stub->args.writev.off != offset_expected) {
                                break;
                        }

                        length = iov_length (vector, count);
                        size += length;
                        offset_expected += length;

                        request->flags.write_request.stack_wound = 1;
                        list_add_tail (&request->winds, winds);
                } 
        }
  
        return size;
}


size_t 
__wb_get_aggregate_size (list_head_t *list, char *other_fop_in_queue,
                         char *non_contiguous_writes)
{
        wb_request_t *request = NULL;
        size_t        size = 0, length = 0;
        struct iovec *vector = NULL;
        int32_t       count = 0;
        char          first_request = 1;
        off_t         offset_expected = 0;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        if (request->stub && other_fop_in_queue) {
                                *other_fop_in_queue = 1;
                        }
                        break;
                }

                vector = request->stub->args.writev.vector;
                count = request->stub->args.writev.count;
                if (!request->flags.write_request.stack_wound) {
                        if (first_request) {
                                first_request = 0;
                                offset_expected = request->stub->args.writev.off;
                        } 

                        if (offset_expected != request->stub->args.writev.off) {
                                if (non_contiguous_writes) {
                                        *non_contiguous_writes = 1;
                                }
                                break;
                        }

                        length = iov_length (vector, count);
                        size += length;
                        offset_expected += length;
                }
        }

        return size;
}


uint32_t
__wb_get_incomplete_writes (list_head_t *list)
{
        wb_request_t *request = NULL;
        uint32_t      count = 0;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        break;
                }

                if (request->flags.write_request.stack_wound
                    && !request->flags.write_request.got_reply) {
                        count++;
                }
        }

        return count;
}


size_t
__wb_mark_winds (list_head_t *list, list_head_t *winds, size_t aggregate_conf)
{
        size_t   aggregate_current = 0;
        uint32_t incomplete_writes = 0;
        char     other_fop_in_queue = 0;
        char     non_contiguous_writes = 0;

        incomplete_writes = __wb_get_incomplete_writes (list); 

        aggregate_current = __wb_get_aggregate_size (list, &other_fop_in_queue,
                                                     &non_contiguous_writes);

        if ((incomplete_writes == 0) || (aggregate_current >= aggregate_conf)
            || other_fop_in_queue || non_contiguous_writes) {
                __wb_mark_wind_all (list, winds);
        }

        return aggregate_current;
}


size_t
__wb_get_window_size (list_head_t *list)
{
        wb_request_t *request = NULL;
        size_t        size = 0;
        struct iovec *vector = NULL;
        int32_t       count = 0;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        continue;
                }

                vector = request->stub->args.writev.vector;
                count = request->stub->args.writev.count;

                if (request->flags.write_request.write_behind
                    && !request->flags.write_request.got_reply)
                {
                        size += iov_length (vector, count);
                }
        }

        return size;
}


size_t 
__wb_mark_unwind_till (list_head_t *list, list_head_t *unwinds, size_t size)
{
        size_t        written_behind = 0;
        wb_request_t *request = NULL;
        struct iovec *vector = NULL;
        int32_t       count = 0;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        continue;
                }

                vector = request->stub->args.writev.vector;
                count = request->stub->args.writev.count;

                if (written_behind <= size) {
                        if (!request->flags.write_request.write_behind) {
                                written_behind += iov_length (vector, count);
                                request->flags.write_request.write_behind = 1;
                                list_add_tail (&request->unwinds, unwinds);
                        }
                } else {
                        break;
                }
        }

        return written_behind;
}


int32_t 
__wb_mark_unwinds (list_head_t *list, list_head_t *unwinds, size_t window_conf)
{
        size_t window_current = 0;

        window_current = __wb_get_window_size (list);
        if (window_current <= window_conf)
        {
                window_current += __wb_mark_unwind_till (list, unwinds,
                                                         window_conf - window_current);
        }

        return window_current;
}


uint32_t
__wb_get_other_requests (list_head_t *list, list_head_t *other_requests)
{
        wb_request_t *request = NULL;
        uint32_t      count = 0;
        list_for_each_entry (request, list, list) {
                if ((request->stub == NULL)
                    || (request->stub->fop == GF_FOP_WRITE)) {
                        break;
                }
                
                if (!request->flags.other_requests.marked_for_resume) {
                        request->flags.other_requests.marked_for_resume = 1;
                        list_add_tail (&request->other_requests,
                                       other_requests);
                        count++;

                        /* lets handle one at a time */
                        break;
                }
        }

        return count;
}


int32_t
wb_stack_unwind (list_head_t *unwinds)
{
        struct stat   buf = {0,};
        wb_request_t *request = NULL, *dummy = NULL;
        call_frame_t *frame = NULL;
        wb_local_t   *local = NULL;

        list_for_each_entry_safe (request, dummy, unwinds, unwinds)
        {
                frame = request->stub->frame;
                local = frame->local;

                STACK_UNWIND (frame, local->op_ret, local->op_errno, &buf);

                wb_request_unref (request);
        }

        return 0;
}


int32_t
wb_resume_other_requests (call_frame_t *frame, wb_file_t *file,
                          list_head_t *other_requests)
{
        int32_t       ret = 0;
        wb_request_t *request = NULL, *dummy = NULL;
        int32_t       fops_removed = 0;
        char          wind = 0;
        call_stub_t  *stub = NULL;

        if (list_empty (other_requests)) {
                goto out;
        }

        list_for_each_entry_safe (request, dummy, other_requests,
                                  other_requests) {
                wind = request->stub->wind;
                stub = request->stub;
                
                LOCK (&file->lock); 
                {
                        request->stub = NULL;
                }
                UNLOCK (&file->lock);

                if (!wind) {
                        wb_request_unref (request);
                        fops_removed++;
                } 
                
                call_resume (stub);
        }

        if (fops_removed > 0) {
                wb_process_queue (frame, file, 0);
        }
        
out:
        return ret;
}


int32_t
wb_do_ops (call_frame_t *frame, wb_file_t *file, list_head_t *winds,
           list_head_t *unwinds, list_head_t *other_requests)
{
        wb_stack_unwind (unwinds);
        wb_sync (frame, file, winds);
        wb_resume_other_requests (frame, file, other_requests);

        return 0;
}


int32_t 
wb_process_queue (call_frame_t *frame, wb_file_t *file, char flush_all) 
{
        list_head_t winds, unwinds, other_requests;
        size_t      size = 0;
        wb_conf_t  *conf = file->this->private;
        uint32_t    count = 0;

        INIT_LIST_HEAD (&winds);
        INIT_LIST_HEAD (&unwinds);
        INIT_LIST_HEAD (&other_requests);
                
        if (!file) {
                return -1;
        }

        size = flush_all ? 0 : conf->aggregate_size;
        LOCK (&file->lock);
        {
                count = __wb_get_other_requests (&file->request,
                                                 &other_requests);

                if (count == 0) {
                        __wb_mark_winds (&file->request, &winds, size);
                }

                __wb_mark_unwinds (&file->request, &unwinds, conf->window_size);
        }
        UNLOCK (&file->lock);

        wb_do_ops (frame, file, &winds, &unwinds, &other_requests);
        return 0;
}


int32_t
wb_writev_cbk (call_frame_t *frame,
               void *cookie,
               xlator_t *this,
               int32_t op_ret,
               int32_t op_errno,
               struct stat *stbuf)
{
        STACK_UNWIND (frame, op_ret, op_errno, stbuf);
        return 0;
}


int32_t
wb_writev (call_frame_t *frame,
           xlator_t *this,
           fd_t *fd,
           struct iovec *vector,
           int32_t count,
           off_t offset)
{
        wb_file_t    *file = NULL;
        char          wb_disabled = 0;
        call_frame_t *process_frame = NULL;
        size_t        size = 0;
	uint64_t      tmp_file = 0;
        call_stub_t  *stub = NULL;
        wb_local_t   *local = NULL;

        if (vector != NULL) 
                size = iov_length (vector, count);

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD, NULL);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;
        if (!file) {
                gf_log (this->name, GF_LOG_ERROR,
                        "wb_file not found for fd %p", fd);
                STACK_UNWIND (frame, -1, EBADFD, NULL);
                return 0;
        }

        LOCK (&file->lock);
        {
                if (file->disabled || file->disable_till) {
                        if (size > file->disable_till) {
                                file->disable_till = 0;
                        } else {
                                file->disable_till -= size;
                        }
                        wb_disabled = 1;
                }
        }
        UNLOCK (&file->lock);

        if (wb_disabled) {
                STACK_WIND (frame,
                            wb_writev_cbk,
                            FIRST_CHILD (frame->this),
                            FIRST_CHILD (frame->this)->fops->writev,
                            fd,
                            vector,
                            count,
                            offset);
                return 0;
        }

        process_frame = copy_frame (frame);

        local = CALLOC (1, sizeof (*local));
        frame->local = local;
        local->file = file;

        stub = fop_writev_stub (frame, NULL, fd, vector, count, offset);
        if (stub == NULL) {
                STACK_UNWIND (frame, -1, ENOMEM, NULL);
                return 0;
        }

        wb_enqueue (file, stub);
        wb_process_queue (process_frame, file, 0);

        STACK_DESTROY (process_frame->root);

        return 0;
}


int32_t
wb_readv_cbk (call_frame_t *frame,
              void *cookie,
              xlator_t *this,
              int32_t op_ret,
              int32_t op_errno,
              struct iovec *vector,
              int32_t count,
              struct stat *stbuf)
{
        wb_local_t   *local = NULL;
        wb_file_t    *file = NULL;
        wb_request_t *request = NULL;

        local = frame->local;
        file = local->file;
        request = local->request;

        if (request) {
                wb_request_unref (request);
                wb_process_queue (frame, file, 0);
        }

        STACK_UNWIND (frame, op_ret, op_errno, vector, count, stbuf);

        return 0;
}


static int32_t
wb_readv_helper (call_frame_t *frame,
                 xlator_t *this,
                 fd_t *fd,
                 size_t size,
                 off_t offset)
{
        STACK_WIND (frame,
                    wb_readv_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readv,
                    fd, size, offset);
        
        return 0;
}


int32_t
wb_readv (call_frame_t *frame,
          xlator_t *this,
          fd_t *fd,
          size_t size,
          off_t offset)
{
        wb_file_t   *file = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD, NULL);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;
        if (file) {
                stub = fop_readv_stub (frame, wb_readv_helper, fd, size,
                                       offset);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM, NULL);
                        return 0;
                }

                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame,
                            wb_readv_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->readv,
                            fd, size, offset);
        }

        return 0;
}


int32_t
wb_ffr_bg_cbk (call_frame_t *frame,
               void *cookie,
               xlator_t *this,
               int32_t op_ret,
               int32_t op_errno)
{
        STACK_DESTROY (frame->root);
        return 0;
}


int32_t
wb_ffr_cbk (call_frame_t *frame,
            void *cookie,
            xlator_t *this,
            int32_t op_ret,
            int32_t op_errno)
{
        wb_local_t *local = NULL;
        wb_file_t  *file = NULL;
        wb_conf_t  *conf = NULL;
        char        unwind = 0;

        conf = this->private;
        local = frame->local;
        file = local->file;

        if (conf->flush_behind
            && (!file->disabled) && (file->disable_till == 0)) {
                unwind = 1;
        } else {
                local->reply_count++;
                /* without flush-behind, unwind should wait for replies of
                   writes queued before and the flush */
                if (local->reply_count == 2) {
                        unwind = 1;
                }
        }

        if (unwind) {
                if (file->op_ret == -1) {
                        op_ret = file->op_ret;
                        op_errno = file->op_errno;

                        file->op_ret = 0;
                }

                wb_process_queue (frame, file, 0);
                
                STACK_UNWIND (frame, op_ret, op_errno);
        }

        return 0;
}


int32_t
wb_flush (call_frame_t *frame,
          xlator_t *this,
          fd_t *fd)
{
        wb_conf_t    *conf = NULL;
        wb_file_t    *file = NULL;
        wb_local_t   *local = NULL;
	uint64_t      tmp_file = 0;
        call_stub_t  *stub = NULL;
        call_frame_t *process_frame = NULL;
        wb_local_t   *tmp_local = NULL;

        conf = this->private;

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;
        stub = fop_flush_cbk_stub (frame, wb_ffr_cbk, 0, 0);
        if (stub == NULL) {
                STACK_UNWIND (frame, -1, ENOMEM);
                return 0;
        }

        process_frame = copy_frame (frame);
        if (conf->flush_behind
            && (!file->disabled) && (file->disable_till == 0)) {
                tmp_local = CALLOC (1, sizeof (*local));
                tmp_local->file = file;

                process_frame->local = tmp_local;
        }

        fd_ref (fd);

        wb_enqueue (file, stub);

        wb_process_queue (process_frame, file, 1); 
                
        if (conf->flush_behind
            && (!file->disabled) && (file->disable_till == 0)) {
                STACK_WIND (process_frame,
                            wb_ffr_bg_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->flush,
                            fd);
        } else {
                STACK_WIND (frame,
                            wb_ffr_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->flush,
                            fd);
                STACK_DESTROY (process_frame->root);
        }

        fd_unref (fd);

        return 0;
}


static int32_t
wb_fsync_cbk (call_frame_t *frame,
              void *cookie,
              xlator_t *this,
              int32_t op_ret,
              int32_t op_errno)
{
        wb_local_t   *local = NULL;
        wb_file_t    *file = NULL;
        wb_request_t *request = NULL;

        local = frame->local;
        file = local->file;
        request = local->request;

        if (file->op_ret == -1) {
                op_ret = file->op_ret;
                op_errno = file->op_errno;

                file->op_ret = 0;
        }

        if (request) {
                wb_request_unref (request);
                wb_process_queue (frame, file, 0);
        }

        STACK_UNWIND (frame, op_ret, op_errno);
        
        return 0;
}


static int32_t
wb_fsync_helper (call_frame_t *frame,
                 xlator_t *this,
                 fd_t *fd,
                 int32_t datasync)
{
        STACK_WIND (frame,
                    wb_fsync_cbk,
                    FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsync,
                    fd, datasync);
        return 0;
}


int32_t
wb_fsync (call_frame_t *frame,
          xlator_t *this,
          fd_t *fd,
          int32_t datasync)
{
        wb_file_t   *file = NULL;
        wb_local_t  *local = NULL;
	uint64_t     tmp_file = 0;
        call_stub_t *stub = NULL;

        if (fd_ctx_get (fd, this, &tmp_file)) {
                gf_log (this->name, GF_LOG_ERROR, "returning EBADFD");
                STACK_UNWIND (frame, -1, EBADFD);
                return 0;
        }

	file = (wb_file_t *)(long)tmp_file;

        local = CALLOC (1, sizeof (*local));
        local->file = file;

        frame->local = local;

        if (file) {
                stub = fop_fsync_stub (frame, wb_fsync_helper, fd, datasync);
                if (stub == NULL) {
                        STACK_UNWIND (frame, -1, ENOMEM);
                        return 0;
                }
        
                wb_enqueue (file, stub);

                wb_process_queue (frame, file, 1);
        } else {
                STACK_WIND (frame,
                            wb_fsync_cbk,
                            FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->fsync,
                            fd, datasync);
        }

        return 0;
}


int32_t
wb_release (xlator_t *this,
            fd_t *fd)
{
        uint64_t   file_ptr = 0;
        wb_file_t *file = NULL;

	fd_ctx_get (fd, this, &file_ptr);
        file = (wb_file_t *) (long) file_ptr;

        LOCK (&file->lock);
        {
                assert (list_empty (&file->request));
        }
        UNLOCK (&file->lock);

  	wb_file_destroy (file);

        return 0;
}


int32_t 
init (xlator_t *this)
{
        dict_t    *options = NULL;
        wb_conf_t *conf = NULL;
        char      *aggregate_size_string = NULL;
        char      *window_size_string    = NULL;
        char      *flush_behind_string   = NULL;
        char      *disable_till_string = NULL;
        char      *enable_O_SYNC_string = NULL;
        int32_t    ret = -1;

        if ((this->children == NULL)
            || this->children->next) {
                gf_log (this->name, GF_LOG_ERROR,
                        "FATAL: write-behind (%s) not configured with exactly one child",
                        this->name);
                return -1;
        }

        if (this->parents == NULL) {
                gf_log (this->name, GF_LOG_WARNING,
                        "dangling volume. check volfile");
        }
        
        options = this->options;

        conf = CALLOC (1, sizeof (*conf));
        
        conf->enable_O_SYNC = _gf_false;
        ret = dict_get_str (options, "enable-O_SYNC",
                            &enable_O_SYNC_string);
        if (ret == 0) {
                ret = gf_string2boolean (enable_O_SYNC_string,
                                         &conf->enable_O_SYNC);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "'enable-O_SYNC' takes only boolean arguments");
                        return -1;
                }
        }

        /* configure 'options aggregate-size <size>' */
        conf->aggregate_size = 0;
        ret = dict_get_str (options, "block-size", 
                            &aggregate_size_string);
        if (ret == 0) {
                ret = gf_string2bytesize (aggregate_size_string, 
                                          &conf->aggregate_size);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_ERROR, 
                                "invalid number format \"%s\" of \"option aggregate-size\"", 
                                aggregate_size_string);
                        return -1;
                }
        }

        gf_log (this->name, GF_LOG_DEBUG,
                "using aggregate-size = %"PRIu64"", 
                conf->aggregate_size);
  
        conf->disable_till = 1;
        ret = dict_get_str (options, "disable-for-first-nbytes", 
                            &disable_till_string);
        if (ret == 0) {
                ret = gf_string2bytesize (disable_till_string, 
                                          &conf->disable_till);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_ERROR, 
                                "invalid number format \"%s\" of \"option disable-for-first-nbytes\"", 
                                disable_till_string);
                        return -1;
                }
        }

        gf_log (this->name, GF_LOG_DEBUG,
                "disabling write-behind for first %"PRIu64" bytes", 
                conf->disable_till);
  
        /* configure 'option window-size <size>' */
        conf->window_size = 0;
        ret = dict_get_str (options, "cache-size", 
                            &window_size_string);
        if (ret == 0) {
                ret = gf_string2bytesize (window_size_string, 
                                          &conf->window_size);
                if (ret != 0) {
                        gf_log (this->name, GF_LOG_ERROR, 
                                "invalid number format \"%s\" of \"option window-size\"", 
                                window_size_string);
                        FREE (conf);
                        return -1;
                }
        }

        if (!conf->window_size && conf->aggregate_size) {
                gf_log (this->name, GF_LOG_WARNING,
                        "setting window-size to be equal to aggregate-size(%"PRIu64")",
                        conf->aggregate_size);
                conf->window_size = conf->aggregate_size;
        }

        if (conf->window_size < conf->aggregate_size) {
                gf_log (this->name, GF_LOG_ERROR,
                        "aggregate-size(%"PRIu64") cannot be more than window-size"
                        "(%"PRIu64")", conf->window_size, conf->aggregate_size);
                FREE (conf);
                return -1;
        }

        /* configure 'option flush-behind <on/off>' */
        conf->flush_behind = 0;
        ret = dict_get_str (options, "flush-behind", 
                            &flush_behind_string);
        if (ret == 0) {
                ret = gf_string2boolean (flush_behind_string, 
                                         &conf->flush_behind);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "'flush-behind' takes only boolean arguments");
                        return -1;
                }

                if (conf->flush_behind) {
			gf_log (this->name, GF_LOG_DEBUG,
				"enabling flush-behind");
                }
        }
        this->private = conf;
        return 0;
}


void
fini (xlator_t *this)
{
        wb_conf_t *conf = this->private;

        FREE (conf);
        return;
}


struct xlator_fops fops = {
        .writev      = wb_writev,
        .open        = wb_open,
        .create      = wb_create,
        .readv       = wb_readv,
        .flush       = wb_flush,
        .fsync       = wb_fsync,
        .stat        = wb_stat,
        .fstat       = wb_fstat,
        .truncate    = wb_truncate,
        .ftruncate   = wb_ftruncate,
        .utimens     = wb_utimens,
};

struct xlator_mops mops = {
};

struct xlator_cbks cbks = {
        .release  = wb_release
};

struct volume_options options[] = {
        { .key  = {"flush-behind"}, 
          .type = GF_OPTION_TYPE_BOOL
        },
        { .key  = {"block-size", "aggregate-size"}, 
          .type = GF_OPTION_TYPE_SIZET, 
          .min  = 128 * GF_UNIT_KB, 
          .max  = 4 * GF_UNIT_MB 
        },
        { .key  = {"cache-size", "window-size"}, 
          .type = GF_OPTION_TYPE_SIZET, 
          .min  = 512 * GF_UNIT_KB, 
          .max  = 1 * GF_UNIT_GB 
        },
        { .key = {"disable-for-first-nbytes"},
          .type = GF_OPTION_TYPE_SIZET,
          .min = 1,
          .max = 1 * GF_UNIT_MB,
        },
        { .key = {"enable-O_SYNC"},
          .type = GF_OPTION_TYPE_BOOL,
        }, 
        { .key = {NULL} },
};
