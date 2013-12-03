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

#ifndef _RR_H
#define _RR_H

#include "scheduler.h"
#include <stdint.h>
#include <sys/time.h>

struct rr_sched_struct {
  xlator_t *xl;
  struct timeval last_stat_fetch;
  int64_t free_disk;
  int32_t refresh_interval;
  unsigned char eligible;
};

struct rr_struct {
  struct rr_sched_struct *array;
  struct timeval last_stat_fetch;
  int32_t refresh_interval;
  int64_t min_free_disk;
  
  pthread_mutex_t rr_mutex;
  int32_t child_count;
  int32_t sched_index;  
};

#endif /* _RR_H */
