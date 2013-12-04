/*
  (C) 2006,2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
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

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "logging.h"
#include "common-utils.h"


typedef int32_t (*rw_op_t)(int32_t fd, char *buf, int32_t size);
typedef int32_t (*rwv_op_t)(int32_t fd, const struct iovec *buf, int32_t size);

static int32_t 
full_rw (int32_t fd, char *buf, int32_t size, 
	 rw_op_t op)
{
  int32_t bytes_xferd = 0;
  char *p = buf;

  while (bytes_xferd < size) {
    int32_t ret = op (fd, p, size - bytes_xferd);

    if (!ret || (ret < 0 && errno != EINTR)) {
      return -1;
    }
    
    bytes_xferd += ret;
    /* was: p += bytes_xferd. Took hours to find :O */
    p += ret;
  }

  return 0;
}

/*
  Make sure size bytes are read from the fd into the buf
*/
int32_t 
gf_full_read (int32_t fd, char *buf, int32_t size)
{
  return full_rw (fd, buf, size, (rw_op_t)read);
}

/*
  Make sure size bytes are written to the fd from the buf
*/
int32_t 
gf_full_write (int32_t fd, const char *buf, int32_t size)
{
  return full_rw (fd, (char *)buf, size, (rw_op_t)write);
}

static int32_t
full_rwv (int32_t fd,
	  const struct iovec *vector,
	  int count,
	  rwv_op_t fn)
{
  struct iovec *opvec = alloca (sizeof (*opvec) * count);
  int32_t i;
  int64_t total_len = 0;
  int64_t bytes_xferd = 0;
  int32_t ret;

  memcpy (opvec, vector, count * sizeof (*opvec));
  for (i = 0; i < count; i++)
    total_len += opvec[i].iov_len;

  while (bytes_xferd < total_len) {
    ret = fn (fd, opvec, count);

    if (!ret || (ret < 0 && errno != EINTR)) {
      return -1;
    }

    bytes_xferd += ret;

    if (bytes_xferd < total_len) {
      int32_t moved = 0;
      while (moved < ret) {
	if ((ret - moved) >= opvec[0].iov_len) {
	  moved += opvec[0].iov_len;
	  opvec++;
	  count--;
	} else {
	  opvec[0].iov_len -= (ret - moved);
	  opvec[0].iov_base += (ret - moved);
	  moved += (ret - moved);
	}
      }
    }
  }

  return 0;
}

int32_t
gf_full_readv (int fd,
	       const struct iovec *vector,
	       int count)
{
  return full_rwv (fd, vector, count, (rwv_op_t)readv);
}

int32_t
gf_full_writev (int fd,
		const struct iovec *vector,
		int count)
{
  return full_rwv (fd, vector, count, (rwv_op_t)writev);
}

in_addr_t
gf_resolve_ip (const char *hostname)
{
  in_addr_t addr;
  struct hostent *h = gethostbyname (hostname);

  if (!h)
    return INADDR_NONE;
  memcpy (&addr, h->h_addr, h->h_length);

  return addr;
}

int64_t 
gf_str_to_long_long (const char *number)
{
  int64_t unit = 1;
  int64_t ret = 0;
  char *endptr = NULL ;
  if (!number)
    return 0;

  ret = strtoll (number, &endptr, 0);

  if (endptr) {
    switch (*endptr) {
    case 'G':
    case 'g':
      if ((* (endptr + 1) == 'B') ||(* (endptr + 1) == 'b'))
	unit = 1024 * 1024 * 1024;
      break;
    case 'M':
    case 'm':
      if ((* (endptr + 1) == 'B') ||(* (endptr + 1) == 'b'))
	unit = 1024 * 1024;
      break;
    case 'K':
    case 'k':
      if ((* (endptr + 1) == 'B') ||(* (endptr + 1) == 'b'))
	unit = 1024;
      break;
    case '%':
      unit = 1;
      break;
    default:
      unit = 1;
      break;
    }
  }
  return ret * unit;
}

#if HAVE_BACKTRACE
/* Obtain a backtrace and print it to stdout. */
/* TODO: It looks like backtrace_symbols allocates memory,
   it may be problem because mostly memory allocation/free causes 'sigsegv' */
void
gf_print_trace (int32_t signum)
{
  extern FILE *gf_log_logfile;
  void *array[200];
  size_t size;
  int fd = fileno (gf_log_logfile);
  char msg[64];

  size = backtrace (array, 200);
  
  sprintf (msg, "\n---------\ngot signal (%d), printing backtrace\n---------\n", signum);
  
  write (fd, msg, strlen (msg));
  backtrace_symbols_fd (array, size, fd);
  sprintf (msg, "---------\n");
  write (fd, msg, strlen (msg));
  signal (signum, SIG_DFL);
  raise (signum);
}
#endif /* HAVE_BACKTRACE */

void
trap (void)
{

}
