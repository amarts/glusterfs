/*
  (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License aint64_t with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <netdb.h>
#include <argp.h>
#include <stdint.h>

#include "xlator.h"
#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "protocol.h"

/* using argp for command line parsing */
static char *mount_point = NULL;
static int32_t cmd_def_log_level = GF_LOG_MAX;
static char *cmd_def_log_file = DEFAULT_LOG_FILE;
int32_t gf_cmd_def_daemon_mode = GF_YES;

static char doc[] = "glusterfs is a glusterfs client";
static char argp_doc[] = "MOUNT-POINT";
const char *argp_program_version = PACKAGE_NAME " " PACKAGE_VERSION;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;

static struct spec_location spec;
error_t parse_opts (int32_t key, char *arg, struct argp_state *_state);

struct {
  char *f[2];
} f;

static struct argp_option options[] = {
  {"spec-file", 'f', "VOLUMESPEC-FILE", 0, "Load volume spec file VOLUMESPEC" },
  /*  {"spec-server-ip", 's', "VOLUMESPEC-SERVERIP", 0, "Get volume spec file from VOLUMESPEC-SERVERIP"},
  {"spec-server-port", 'p', "VOLUMESPEC-SERVERPORT", 0, "connect to VOLUMESPEC_SERVERPORT on spec server"},
  */
  {"log-level", 'L', "LOGLEVEL", 0, "Default LOGLEVEL"},
  {"log-file", 'l', "LOGFILE", 0, "Specify the file to redirect logs"},
  {"no-daemon", 'N', 0, 0, "Run glusterfs in foreground"},
  {"version", 'V', 0, 0, "print32_t version information"},
  { 0, }
};
static struct argp argp = { options, parse_opts, argp_doc, doc };

FILE *
from_remote (char *specfile)
{
  int32_t fd;
  int32_t ret;
  dict_t *request;
  dict_t *reply;
  gf_block_t *req_blk;
  gf_block_t *rpl_blk;
  int32_t dict_len;
  char *dict_buf;
  int32_t blk_len;
  char *blk_buf;
  char *content = NULL;
  data_t *content_data = NULL;
  FILE *spec_fp;
  struct sockaddr_in sin;
  char *spec = strdupa (specfile);
  char *port_str;
  unsigned short port = GF_DEFAULT_LISTEN_PORT;

  port_str = strchr (spec, ':');
  if (port_str) {
    *port_str = 0;
    port_str++;
    port = (unsigned short) strtol (port_str, NULL, 0);
  }

  fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd == -1)
    return NULL;

  sin.sin_family = AF_INET;
  sin.sin_port = htons (port);
  sin.sin_addr.s_addr = resolve_ip (spec);

  if (connect (fd, (struct sockaddr *)&sin, sizeof (sin)) != 0) {
    printf ("connect to server failed\n");
    return NULL;
  }

  request = get_new_dict ();
  dict_set (request, "foo", str_to_data ("bar"));
  dict_len = dict_serialized_length (request);
  dict_buf = alloca (dict_len);
  dict_serialize (request, dict_buf);
  dict_destroy (request);

  req_blk = gf_block_new (434343);
  req_blk->type = OP_TYPE_MOP_REQUEST;
  req_blk->op = OP_GETSPEC;
  req_blk->size = dict_len;
  req_blk->data = dict_buf;

  blk_len = gf_block_serialized_length (req_blk);
  blk_buf = alloca (blk_len);
  gf_block_serialize (req_blk, blk_buf);

  ret = full_write (fd, blk_buf, blk_len);

  free (req_blk);

  if (ret == -1) {
    printf ("full_write failed\n");
    return NULL;
  }

  rpl_blk = gf_block_unserialize (fd);
  if (!rpl_blk) {
    printf ("Protocol unserialize failed\n");
    return NULL;
  }
  reply = rpl_blk->dict;

  ret = -1;

  if (dict_get (reply, "RET"))
    ret = data_to_int (dict_get (reply, "RET"));
  /*
  if (ret == -1) {
    printf ("Remote server returned -1\n");
    return NULL;
    }*/

  content_data = dict_get (reply, "spec-file-data");

  if (!content_data) {
    printf ("spec-file-data missing from server\n");
    return NULL;
  }

  spec_fp = tmpfile ();
  if (!spec_fp) {
    printf ("unable to create temporary file\n");
    return NULL;
  }

  fwrite (content_data->data, content_data->len, 1, spec_fp);
  fseek (spec_fp, 0, SEEK_SET);
  return spec_fp;
}

static xlator_t *
get_xlator_graph ()
{
  xlator_t *trav = NULL, *tree = NULL;
  char *specfile = spec.spec.file;
  FILE *conf = NULL;

  if (access (specfile, R_OK) == 0){
    specfile = spec.spec.file;
    
    conf = fopen (specfile, "r");
    
    if (!conf) {
      perror (specfile);
      exit (1);
    }
    gf_log ("glusterfs-fuse",
	    GF_LOG_NORMAL,
	    "loading spec from %s",
	    specfile);
    tree = file_to_xlator_tree (conf);
    trav = tree;

    fclose (conf);

  }else{
    conf = from_remote (specfile);
    if (!conf) {
      perror (specfile);
      exit (1);
    }
    tree = file_to_xlator_tree (conf);
    trav = tree;
    fclose (conf);
  }
  
  if (tree == NULL) {
    gf_log ("glusterfs-fuse", GF_LOG_ERROR, "specification file parsing failed, exiting");
    exit (-1);
  }
  
  while (trav) {
    if (trav->init)
      if (trav->init (trav) != 0) {
	struct xlator *node = tree;
	while (node != trav) {
	  node->fini (node);
	  node = node->next;
	}
	gf_log ("glusterfs-fuse", GF_LOG_ERROR, "%s xlator initialization failed\n", trav->name);
	exit (1);
      }
    trav = trav->next;
  }

  while (tree->parent)
    tree = tree->parent;

  return tree;
}

struct client_ctx {
  int client_count;
  int pfd_count;
  struct pollfd *pfd;
  struct {
    int32_t (*handler) (int32_t fd,
			int32_t event,
			void *data);
    void *data;
  } *cbk_data;
};

static struct client_ctx *
get_client_ctx ()
{
  static struct client_ctx *ctx;

  if (!ctx) {
    ctx = (void *)calloc (1, sizeof (*ctx));
    ctx->pfd_count = 1024;
    ctx->pfd = (void *) calloc (1024, 
				sizeof (struct pollfd));
    ctx->cbk_data = (void *) calloc (1024,
				     sizeof (*ctx->cbk_data));
  }

  return ctx;
}

static void
unregister_member (struct client_ctx *ctx,
		    int32_t i)
{
  gf_log ("glusterfs/fuse",
	  GF_LOG_DEBUG,
	  "unregistering socket %d from main loop",
	  ctx->pfd[i].fd);

  ctx->pfd[i].fd = ctx->pfd[ctx->client_count - 1].fd;
  ctx->pfd[i].events = ctx->pfd[ctx->client_count - 1].events;
  ctx->pfd[i].revents = ctx->pfd[ctx->client_count - 1].revents;
  ctx->cbk_data[i].handler = ctx->cbk_data[ctx->client_count - 1].handler;
  ctx->cbk_data[i].data = ctx->cbk_data[ctx->client_count - 1].data;

  ctx->client_count--;
  return;
}

static int32_t
new_fd_cbk (int fd, 
	    int32_t (*handler)(int32_t fd,
			       int32_t event,
			       void *data),
	    void *data)
{
  struct client_ctx *ctx = get_client_ctx ();

  if (ctx->client_count == ctx->pfd_count) {
    ctx->pfd_count *= 2;
    ctx->pfd = realloc (ctx->pfd, 
			sizeof (*ctx->pfd) * ctx->pfd_count);
    ctx->cbk_data = realloc (ctx->pfd, 
			     sizeof (*ctx->cbk_data) * ctx->pfd_count);
  }

  ctx->pfd[ctx->client_count].fd = fd;
  ctx->pfd[ctx->client_count].events = POLLIN | POLLPRI | POLLERR | POLLHUP;
  ctx->pfd[ctx->client_count].revents = 0;

  ctx->cbk_data[ctx->client_count].handler = handler;
  ctx->cbk_data[ctx->client_count].data = data;

  ctx->client_count++;
  return 0;
}

static int32_t
client_init ()
{
  set_transport_register_cbk (new_fd_cbk);
  return 0;
}


static int32_t
client_loop ()
{
  struct client_ctx *ctx = get_client_ctx ();
  struct pollfd *pfd;

  while (1) {
    int32_t ret;
    int32_t i;

    pfd = ctx->pfd;
    if (!ctx->client_count)
      break;
    ret = poll (pfd,
		(unsigned int) ctx->client_count,
		-1);

    if (ret == -1) {
      if (errno == EINTR) {
	continue;
      } else {
	return -errno;
      }
    }

    for (i=0; i < ctx->client_count; i++) {
      if (pfd[i].revents) {
	if (ctx->cbk_data[i].handler (pfd[i].fd,
				      pfd[i].revents,
				      ctx->cbk_data[i].data) == -1) {
	  unregister_member (ctx, i);
	  i--;
	}
      }
    }
  }
  return 0;
}

static int32_t
glusterfs_print_version (void)
{
  printf ("%s\n", argp_program_version);
  printf ("Copyright (c) 2006 Z RESEARCH Inc. <http://www.zresearch.com>\n");
  printf ("GlusterFS comes with ABSOLUTELY NO WARRANTY.\nYou may redistribute copies of GlusterFS under the terms of the GNU General Public License.\n");
  exit (0);
}

error_t
parse_opts (int32_t key, char *arg, struct argp_state *_state)
{
  switch (key){
  case 'f':
    spec.where = SPEC_LOCAL_FILE;
    spec.spec.file = strdup (arg);
    break;
  case 's':
    spec.where = SPEC_REMOTE_FILE;
    spec.spec.server.ip = strdup (arg);
    break;
  case 'p':
    spec.spec.server.port = strdup (arg);
    break;
  case 'L':
    /* set log level */
    if (!strncmp (arg, "DEBUG", strlen ("DEBUG"))) {
      cmd_def_log_level = GF_LOG_DEBUG;
    } else if (!strncmp (arg, "NORMAL", strlen ("NORMAL"))) {
      cmd_def_log_level = GF_LOG_NORMAL;
    } else if (!strncmp (arg, "CRITICAL", strlen ("CRITICAL"))) {
      cmd_def_log_level = GF_LOG_CRITICAL;
    } else {
      cmd_def_log_level = GF_LOG_NORMAL;
    }
    break;
  case 'l':
    /* set log file */
    cmd_def_log_file = arg;
    break;
  case 'N':
    gf_cmd_def_daemon_mode = GF_NO;
    break;
  case 'V':
    glusterfs_print_version ();
    break;
  case ARGP_KEY_NO_ARGS:
    argp_usage (_state);
    break;
  case ARGP_KEY_ARG:
    mount_point = arg;
    break;
  }
  return 0;
}
  
void 
args_init (int32_t argc, char **argv)
{
  argp_parse (&argp, argc, argv, 0, 0, &f);
}


int32_t 
main (int32_t argc, char *argv[])
{
  xlator_t *graph = NULL;
  /* command line options: 
     -o allow_other -o default_permissions -o direct_io
  */

  struct rlimit lim;
  lim.rlim_cur = RLIM_INFINITY;
  lim.rlim_max = RLIM_INFINITY;
  setrlimit (RLIMIT_CORE, &lim);
  setrlimit (RLIMIT_NOFILE, &lim);

  args_init (argc, argv);

  if (gf_log_init (cmd_def_log_file) == -1) {
    fprintf (stderr, "%s: failed to open logfile \"%s\"\n", argv[0], cmd_def_log_file);
    return 1;
  }
  gf_log_set_loglevel (cmd_def_log_level);

  if (!mount_point) {
    argp_help (&argp, stderr,ARGP_HELP_USAGE , argv[0]);
    return 1;
  }

  if (!spec.where) {
    argp_help (&argp, stderr,ARGP_HELP_USAGE , argv[0]);
    return 1;
  }

  client_init ();

  graph = get_xlator_graph ();

  glusterfs_mount (graph, mount_point);

  if (gf_cmd_def_daemon_mode == GF_YES) {
  /* funky ps output */
    int i;
    for (i=0;i<argc;i++)
	  memset (argv[i], ' ', strlen (argv[i]));
    sprintf (argv[0], "[glusterfs]");
    daemon (0, 0);
  }

  client_loop ();

  return 0;
}
