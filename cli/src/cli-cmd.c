/*
  Copyright (c) 2010 Gluster, Inc. <http://www.gluster.com>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "cli.h"
#include "cli-cmd.h"
#include "cli-mem-types.h"

#include <fnmatch.h>

static int cmd_done;
static int cmd_sent;
static pthread_cond_t      cond  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t     cond_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t      conn  = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t     conn_mutex = PTHREAD_MUTEX_INITIALIZER;

int    cli_op_ret = 0;
int    connected = 0;

static gf_boolean_t
cli_cmd_needs_connection (struct cli_cmd_word *word)
{
        if (!strcasecmp ("quit", word->word))
                return _gf_false;

        if (!strcasecmp ("help", word->word))
                return _gf_false;

        return _gf_true;
}

int
cli_cmd_process (struct cli_state *state, int argc, char **argv)
{
        int                  ret = 0;
        struct cli_cmd_word *word = NULL;
        struct cli_cmd_word *next = NULL;
        int                  i = 0;
        gf_boolean_t         await_conn = _gf_false;

        word = &state->tree.root;

        if (!argc)
                return 0;

        for (i = 0; i < argc; i++) {
                next = cli_cmd_nextword (word, argv[i]);

                word = next;
                if (!word)
                        break;

                if (word->cbkfn)
                        break;
        }

        if (!word) {
                cli_out ("unrecognized word: %s (position %d)",
                         argv[i], i);
                return -1;
        }

        if (!word->cbkfn) {
                cli_out ("unrecognized command");
                return -1;
        }

        await_conn = cli_cmd_needs_connection (word);

        if (await_conn) {
                ret = cli_cmd_await_connected ();
                if (ret) {
                        cli_out ("Connection failed. Please check if gluster "
                                  "daemon is operational.");
                        gf_log ("", GF_LOG_NORMAL, "Exiting with: %d", ret);
                        exit (ret);
                }
        }


        ret = word->cbkfn (state, word, (const char **)argv, argc);

        return ret;
}


int
cli_cmd_input_token_count (const char *text)
{
        int          count = 0;
        const char  *trav = NULL;
        int          is_spc = 1;

        for (trav = text; *trav; trav++) {
                if (*trav == ' ') {
                        is_spc = 1;
                } else {
                        if (is_spc) {
                                count++;
                                is_spc = 0;
                        }
                }
        }

        return count;
}


int
cli_cmd_process_line (struct cli_state *state, const char *text)
{
        int     count = 0;
        char  **tokens = NULL;
        char  **tokenp = NULL;
        char   *token = NULL;
        char   *copy = NULL;
        char   *saveptr = NULL;
        int     i = 0;
        int     ret = -1;

        count = cli_cmd_input_token_count (text);

        tokens = calloc (count + 1, sizeof (*tokens));
        if (!tokens)
                return -1;

        copy = strdup (text);
        if (!copy)
                goto out;

        tokenp = tokens;

        for (token = strtok_r (copy, " \t\r\n", &saveptr); token;
             token = strtok_r (NULL, " \t\r\n", &saveptr)) {
                *tokenp = strdup (token);

                if (!*tokenp)
                        goto out;
                tokenp++;
                i++;

        }

        ret = cli_cmd_process (state, count, tokens);
out:
        if (copy)
                free (copy);

        if (tokens)
                cli_cmd_tokens_destroy (tokens);

        return ret;
}


int
cli_cmds_register (struct cli_state *state)
{
        int  ret = 0;

        ret = cli_cmd_volume_register (state);
        if (ret)
                goto out;

        ret = cli_cmd_probe_register (state);
        if (ret)
                goto out;

        ret = cli_cmd_misc_register (state);
        if (ret)
                goto out;

        ret = cli_cmd_log_register (state);
        if (ret)
                goto out;

out:
        return ret;
}

int
cli_cmd_cond_init ()
{

       pthread_mutex_init (&cond_mutex, NULL);
       pthread_cond_init (&cond, NULL);

       pthread_mutex_init (&conn_mutex, NULL);
       pthread_cond_init (&conn, NULL);

       return 0;
}

int
cli_cmd_lock ()
{
       pthread_mutex_lock (&cond_mutex);
       return 0;
}

int
cli_cmd_unlock ()
{
        pthread_mutex_unlock (&cond_mutex);
        return 0;
}

int
cli_cmd_await_response ()
{
        struct  timespec        ts = {0,};
        int                     ret = 0;

        cli_op_ret = -1;

        time (&ts.tv_sec);
        ts.tv_sec += CLI_DEFAULT_CMD_TIMEOUT;
        while (!cmd_done && !ret) {
                ret = pthread_cond_timedwait (&cond, &cond_mutex,
                                        &ts);
        }

        cmd_done = 0;

        cli_cmd_unlock ();

        if (ret)
                return ret;

        return cli_op_ret;
}

int
cli_cmd_broadcast_response (int32_t status)
{

        pthread_mutex_lock (&cond_mutex);
        {
                if (!cmd_sent)
                        goto out;
                cmd_done = 1;
                cli_op_ret = status;
                pthread_cond_broadcast (&cond);
        }


out:
        pthread_mutex_unlock (&cond_mutex);
        return 0;
}

int32_t
cli_cmd_await_connected ()
{
        int32_t                 ret = 0;
        struct  timespec        ts = {0,};

        pthread_mutex_lock (&conn_mutex);
        {
                time (&ts.tv_sec);
                ts.tv_sec += CLI_DEFAULT_CONN_TIMEOUT;
                while (!connected && !ret) {
                        ret = pthread_cond_timedwait (&conn, &conn_mutex,
                                                      &ts);
                }
        }
        pthread_mutex_unlock (&conn_mutex);


        return ret;
}

int32_t
cli_cmd_broadcast_connected ()
{
        pthread_mutex_lock (&conn_mutex);
        {
                connected = 1;
                pthread_cond_broadcast (&conn);
        }

        pthread_mutex_unlock (&conn_mutex);

        return 0;
}

int
cli_cmd_submit (void *req, call_frame_t *frame,
                rpc_clnt_prog_t *prog,
                int procnum, struct iobref *iobref,
                cli_serialize_t sfunc, xlator_t *this,
                fop_cbk_fn_t cbkfn)
{
        int     ret = -1;

        cli_cmd_lock ();
        cmd_sent = 0;
        ret = cli_submit_request (req, frame, prog,
                                  procnum, NULL, sfunc,
                                  this, cbkfn);

        if (!ret) {
                cmd_sent = 1;
                ret = cli_cmd_await_response ();
        } else
                cli_cmd_unlock ();

        gf_log ("cli", GF_LOG_DEBUG, "Returning %d", ret);
        return ret;
}
