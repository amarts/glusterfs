/*
  (C) 2006, 2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
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

#ifndef _SCHEDULER_H
#define _SCHEDULER_H

#include "xlator.h"

struct sched_ops {
  int32_t (*init) (xlator_t *this);
  void (*fini) (xlator_t *this);
  void (*update) (xlator_t *this);
  xlator_t *(*schedule) (xlator_t *this, int32_t size);
  void (*notify) (xlator_t *xl, int32_t event, void *data);
};

extern struct sched_ops *get_scheduler (const char *name);

#endif /* _SCHEDULER_H */
