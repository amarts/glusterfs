/*
  Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>
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

#ifndef _RPC_CLNT_H
#define _RPC_CLNT_H

#include "stack.h"
#include "rpc-transport.h"
#include "timer.h"
#include "xdr-common.h"

typedef enum {
        RPC_CLNT_CONNECT,
        RPC_CLNT_DISCONNECT,
        RPC_CLNT_MSG
} rpc_clnt_event_t;

#define AUTH_GLUSTERFS  5

struct xptr_clnt;
struct rpc_req;
struct rpc_clnt;
struct rpc_clnt_config;
struct rpc_clnt_program;

typedef int (*rpc_clnt_notify_t) (struct rpc_clnt *rpc, void *mydata,
                                  rpc_clnt_event_t fn, void *data);

typedef int (*fop_cbk_fn_t) (struct rpc_req *req, struct iovec *iov, int count,
                             void *myframe);

typedef int (*clnt_fn_t) (call_frame_t *fr, xlator_t *xl, void *args);

struct saved_frame {
	union {
		struct list_head list;
		struct {
			struct saved_frame *frame_next;
			struct saved_frame *frame_prev;
		};
	};
        void                    *capital_this;
	void                    *frame;
	struct timeval           saved_at;
	int32_t                  procnum;
        struct rpc_clnt_program *prog;
        fop_cbk_fn_t             cbkfn;
	uint64_t                 callid;
        rpc_transport_rsp_t      rsp;
};

struct saved_frames {
	int64_t            count;
	struct saved_frame sf;
};


/* Initialized by procnum */
typedef struct rpc_clnt_procedure {
        char         *procname;
        clnt_fn_t     fn;
} rpc_clnt_procedure_t;

typedef struct rpc_clnt_program {
        char                 *progname;
        int                   prognum;
        int                   progver;
        rpc_clnt_procedure_t *proctable;
        char                **procnames;
        int                   numproc;
} rpc_clnt_prog_t;

#define RPC_MAX_AUTH_BYTES   400
typedef struct rpc_auth_data {
        int             flavour;
        int             datalen;
        char            authdata[RPC_MAX_AUTH_BYTES];
} rpc_auth_data_t;

#define rpc_auth_flavour(au)    ((au).flavour)

struct rpc_clnt_connection {
        pthread_mutex_t        lock;
        rpc_transport_t       *trans;
        gf_timer_t            *reconnect;
        gf_timer_t            *timer;
        gf_timer_t            *ping_timer;
        struct rpc_clnt       *rpc_clnt;
        char                   connected;
        struct saved_frames   *saved_frames;
        int32_t                frame_timeout;
	struct timeval         last_sent;
	struct timeval         last_received;
	int32_t                ping_started;
};
typedef struct rpc_clnt_connection rpc_clnt_connection_t;

struct rpc_req {
        rpc_clnt_connection_t *conn;
        uint32_t               xid;
        struct iovec           req[2];
        int                    reqcnt;
        struct iovec           rsp[2];
        int                    rspcnt;
        struct iobuf          *rsp_prochdr;
        struct iobuf          *rsp_procpayload;
        int                    rpc_status;
        rpc_auth_data_t        verf;
        rpc_clnt_prog_t       *prog;
        int                    procnum;
};

struct rpc_clnt {
        pthread_mutex_t        lock;
        rpc_clnt_notify_t      notifyfn;
        rpc_clnt_connection_t  conn;
        void                  *mydata;
        uint64_t               xid;
        glusterfs_ctx_t       *ctx;
};

struct rpc_clnt_config {
        int    rpc_timeout;
        int    remote_port;
        char * remote_host;
};


struct rpc_clnt * rpc_clnt_init (struct rpc_clnt_config *config,
                                 dict_t *options, glusterfs_ctx_t *ctx,
                                 char *name);

int rpc_clnt_register_notify (struct rpc_clnt *rpc, rpc_clnt_notify_t fn,
                              void *mydata);

int rpc_clnt_submit (struct rpc_clnt *rpc, rpc_clnt_prog_t *prog,
                     int procnum, fop_cbk_fn_t cbkfn,
                     struct iovec *proghdr, int proghdrcount,
                     struct iovec *progpayload, int progpayloadcount,
                     struct iobref *iobref, void *frame);

void rpc_clnt_destroy (struct rpc_clnt *rpc);

void rpc_clnt_set_connected (rpc_clnt_connection_t *conn);

void rpc_clnt_unset_connected (rpc_clnt_connection_t *conn);

void rpc_clnt_reconnect (void *trans_ptr);

#endif /* !_RPC_CLNT_H */
