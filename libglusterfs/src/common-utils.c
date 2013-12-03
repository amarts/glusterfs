/*
  (C) 2006 Gluster core team <http://www.gluster.org/>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

char *
stripwhite (char *string)
{
  register char *s, *t;

  for (s = string; isspace (*s); s++)
    ;

  if (*s == 0)
    return s;

  t = s + strlen (s) - 1;
  while (t > s && isspace (*t))
    t--;
  *++t = '\0';

  return s;
}

char *
get_token (char **line)
{
  char *command;
  while (1)
    {
      command = (char *) strsep (line, " ,");
      if (!command)
        break;
      if (*(command))
        break;
    }
  
  if (command)
    return strdup (stripwhite (command));
  return command;
}

int 
str2long (char *str, int base, long *l)
{
  long value;
  char *tail = NULL;
  int errnum;
  
  errno = 0;
  value = strtol (str, &tail, base);
  errnum = errno;
  
  if (errnum)
    return (-1); // overflow
  
  if (tail[0] != '\0')
    return (-1); // invalid integer format
  
  *l = value;
  
  return 0;
}

int 
str2ulong (char *str, int base, unsigned long *ul)
{
  long l;
  unsigned long value;
  char *tail = NULL;
  int errnum;
  
  errno = 0;
  l = strtol (str, &tail, base);
  errnum = errno;
  
  if (tail[0] != '\0')
    return (-1); // invalid integer format
  
  if (!errnum)
    {
      if (l < 0)
	return (-1); // minus sign is present
    }
  
  errno = 0;
  value = strtoul (str, &tail, base);
  errnum = errno;
  
  if (errnum)
    return (-1); // overflow
  
  if (tail[0] != '\0')
    return (-1); // invalid integer format
  
  *ul = value;
  
  return 0;
}

int 
str2int (char *str, int base, int *i)
{
  long l;
  
  if (!str2long (str, base, &l))
    {
      if (l >= INT_MIN && l <= INT_MAX)
	{
	  *i = l;
	  return 0;
	}
    }
  
  return (-1);
}

int 
str2uint (char *str, int base, unsigned int *ui)
{
  unsigned long ul;
  
  if (!str2ulong (str, base, &ul))
    {
      if (ul <= UINT_MAX)
	{
	  *ui = ul;
	  return 0;
	}
    }
  
  return (-1);
}

int 
str2double (char *str, double *d)
{
  double value;
  char *tail = NULL;
  int errnum;
  
  if (!(str && d && str[0]))
    return (-1);
  
  errno = 0;
  value = strtod (str, &tail);
  errnum = errno;
  
  if (errnum)
    return (-1);
  
  if (tail[0] != '\0')
    return (-1); // invalid format
  
  *d = value;
  
  return 0;
}

int 
validate_ip_address (char *ip_address)
{
  struct in_addr inp;
  int i;
  int c;
  
  if (!ip_address)
    return (-1);
  
  for (i = 0, c = 0; ip_address[i]; i++)
    {
      if (ip_address[i] == '.')
	c++;
    }
  
  if (c != 3)
    return (-1);
  
  return (!inet_aton (ip_address, &inp));
}

typedef int (*rw_op_t)(int fd, char *buf, int size);

static int 
full_rw (int fd, char *buf, int size, 
	 rw_op_t op)
{
  int bytes_xferd = 0;
  char *p = buf;

  while (bytes_xferd < size) {
    int ret = op (fd, p, size - bytes_xferd);
    if (ret <= 0) {
      if (errno == EINTR)
	continue;
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
int 
full_read (int fd, char *buf, int size)
{
  return full_rw (fd, buf, size, (rw_op_t)read);
}

/*
  Make sure size bytes are written to the fd from the buf
*/
int 
full_write (int fd, const char *buf, int size)
{
  return full_rw (fd, (char *)buf, size, (rw_op_t)write);
}
