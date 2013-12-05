#include "fd.h"

static uint32_t 
gf_fd_fdtable_expand (fdtable_t *fdtable, uint32_t nr);

/* 
   Allocate in memory chunks of power of 2 starting from 1024B 
   Assumes fdtable->lock is held
*/
static inline uint32_t 
gf_roundup_power_of_two (uint32_t nr)
{
  uint32_t result = nr;

  if (nr < 0) {
    gf_log ("server-protocol/fd",
	    GF_LOG_ERROR,
	    "Negative number passed");
    return -1;
  }

  switch (nr) {
  case 0:
  case 1: 
    result = 1;
    break;

  default:
    {
      uint32_t cnt = 0, tmp = nr;
      uint8_t remainder = 0;
      while (tmp != 1){
	if (tmp % 2)
	  remainder = 1;
	tmp /= 2;
	cnt++;
      }

      if (remainder)
	result = 0x1 << (cnt + 1);
      break;
    }
  }

  return result;
}

static uint32_t 
gf_fd_fdtable_expand (fdtable_t *fdtable, uint32_t nr)
{
  fd_t **oldfds = NULL;
  uint32_t oldmax_fds = -1;

  if (!fdtable || nr < 0)
    return EINVAL;

  nr /= (1024 / sizeof (fd_t *));
  nr = gf_roundup_power_of_two (nr + 1);
  nr *= (1024 / sizeof (fd_t *));

  oldfds = fdtable->fds;
  oldmax_fds = fdtable->max_fds;

  fdtable->fds = calloc (nr, sizeof (fd_t *));
  fdtable->max_fds = nr; 

  if (oldfds) {
    uint32_t cpy = oldmax_fds * sizeof (fd_t *);
    memcpy (fdtable->fds, oldfds, cpy);
  }

  free (oldfds);
  return 0;
}

fdtable_t *
gf_fd_fdtable_alloc (void) 
{
  fdtable_t *fdtable = NULL;

  fdtable = calloc (1, sizeof (*fdtable));
  if (!fdtable) 
    return NULL;

  pthread_mutex_init (&fdtable->lock, NULL);

  pthread_mutex_lock (&fdtable->lock);
  {
    gf_fd_fdtable_expand (fdtable, 0);
  }
  pthread_mutex_unlock (&fdtable->lock);

  return fdtable;
}

void 
gf_fd_fdtable_destroy (fdtable_t *fdtable)
{
  if (fdtable) {
    pthread_mutex_lock (&fdtable->lock);
    {
      free (fdtable->fds);
    }
    pthread_mutex_unlock (&fdtable->lock);
    free (fdtable);
  }
}

int32_t 
gf_fd_unused_get (fdtable_t *fdtable, fd_t *fdptr)
{
  int32_t fd = -1, i = 0;

  pthread_mutex_lock (&fdtable->lock);
  {
    for (i = 0; i<fdtable->max_fds; i++) 
      {
	if (!fdtable->fds[i])
	  break;
      }

    if (i < fdtable->max_fds) {
      fdtable->fds[i] = fdptr;
      fd = i;
    } else {
      int32_t error;
      error = gf_fd_fdtable_expand (fdtable, fdtable->max_fds + 1);
      if (error) {
	gf_log ("server-protocol.c",
		GF_LOG_ERROR,
		"Cannot expand fdtable:%s", strerror (error));
      } else {
	fdtable->fds[i] = fdptr;
	fd = i;
      }
    }
  }
  pthread_mutex_unlock (&fdtable->lock);

  return fd;
}

inline void 
gf_fd_put (fdtable_t *fdtable, int32_t fd)
{
  if (fd < 0 || !fdtable || !(fd < fdtable->max_fds))
    return;

  pthread_mutex_lock (&fdtable->lock);
  {
    fdtable->fds[fd] = NULL;
  }
  pthread_mutex_unlock (&fdtable->lock);
}

fd_t *
gf_fd_fdptr_get (fdtable_t *fdtable, int32_t fd)
{
  fd_t *fdptr = NULL;
  if (fd < 0 || !fdtable || !(fd < fdtable->max_fds)) {
    gf_log ("server-protocol/fd",
	    GF_LOG_ERROR,
	    "Invalid parameters passed");
    errno = EINVAL;
    return NULL;
  }

  pthread_mutex_lock (&fdtable->lock);
  {
    fdptr = fdtable->fds[fd];
  }
  pthread_mutex_unlock (&fdtable->lock);

  return fdptr;
}
