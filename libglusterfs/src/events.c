/*
  Copyright (c) 2016 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#include <netinet/in.h>
#include <netdb.h>

#include "glusterfs/syscall.h"
#include "glusterfs/mem-pool.h"
#include "glusterfs/glusterfs.h"
#include "glusterfs/globals.h"
#include "glusterfs/events.h"

#define EVENT_HOST "127.0.0.1"
#define EVENT_PORT 24009

int
_gf_event(eventtypes_t event, const char *fmt, ...)
{
    int ret = 0;
    int sock = -1;
    char *eventstr = NULL;
    va_list arguments;
    char *msg = NULL;
    glusterfs_ctx_t *ctx = NULL;
    char *host = NULL;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    xlator_t *this = THIS;
    char *volfile_server_transport = NULL;

    /* Global context */
    ctx = this->ctx;

    if (IS_ERROR(event) || event >= EVENT_LAST) {
        ret = EVENT_ERROR_INVALID_INPUTS;
        goto out;
    }

    /* Initialize UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (IS_ERROR(sock)) {
        ret = EVENT_ERROR_SOCKET;
        goto out;
    }

    if (ctx) {
        volfile_server_transport = ctx->cmd_args.volfile_server_transport;
    }
    if (!volfile_server_transport) {
        volfile_server_transport = "tcp";
    }

    /* host = NULL returns localhost */
    host = NULL;
    if (ctx && ctx->cmd_args.volfile_server &&
        (strcmp(volfile_server_transport, "unix"))) {
        /* If it is client code then volfile_server is set
           use that information to push the events. */
        host = ctx->cmd_args.volfile_server;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_ADDRCONFIG;

    if ((getaddrinfo(host, TOSTRING(EVENT_PORT), &hints, &result)) != 0) {
        ret = EVENT_ERROR_RESOLVE;
        goto out;
    }

    va_start(arguments, fmt);
    ret = gf_vasprintf(&msg, fmt, arguments);
    va_end(arguments);

    if (IS_ERROR(ret)) {
        ret = EVENT_ERROR_INVALID_INPUTS;
        goto out;
    }

    ret = gf_asprintf(&eventstr, "%u %d %s", (unsigned)time(NULL), event, msg);
    GF_FREE(msg);
    if (ret <= 0) {
        ret = EVENT_ERROR_MSG_FORMAT;
        goto out;
    }

    /* Send Message */
    if (sendto(sock, eventstr, strlen(eventstr), 0, result->ai_addr,
               result->ai_addrlen) <= 0) {
        ret = EVENT_ERROR_SEND;
        goto out;
    }

    ret = EVENT_SEND_OK;

out:
    if (IS_SUCCESS(sock)) {
        sys_close(sock);
    }

    /* Allocated by gf_asprintf */
    if (eventstr)
        GF_FREE(eventstr);

    if (result)
        freeaddrinfo(result);

    return ret;
}
