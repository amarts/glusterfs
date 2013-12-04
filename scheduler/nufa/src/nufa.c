/*
   Copyright (c) 2006, 2007 Z RESEARCH, Inc. <http://www.zresearch.com>
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


#include "nufa.h"
#include <sys/time.h>

static int32_t
nufa_init (xlator_t *xl)
{
  int32_t index = 0;
  data_t *local_name = NULL;
  data_t *data = NULL;
  xlator_list_t *trav_xl = xl->children;
  struct nufa_struct *nufa_buf = calloc (1, sizeof (struct nufa_struct));

  data = dict_get (xl->options, "nufa.limits.min-free-disk");
  if (data) {
    nufa_buf->min_free_disk = gf_str_to_long_long (data->data);
  } else {
    nufa_buf->min_free_disk = gf_str_to_long_long ("5"); /* 5% free-disk */
  }
  data = dict_get (xl->options, "nufa.refresh-interval");
  if (data) {
    nufa_buf->refresh_interval = (int32_t)gf_str_to_long_long (data->data);
  } else {
    nufa_buf->refresh_interval = 10; /* 10 Seconds */
  }
  while (trav_xl) {
    index++;
    trav_xl = trav_xl->next;
  }
  nufa_buf->child_count = index;
  nufa_buf->sched_index = 0;
  nufa_buf->array = calloc (index, sizeof (struct nufa_sched_struct));
  trav_xl = xl->children;
  index = 0;

  local_name = dict_get (xl->options, "nufa.local-volume-name");
  if (!local_name) {
    /* Error */
    gf_log ("nufa", 
	    GF_LOG_ERROR, 
	    "No 'local-volume-name' option given in spec file\n");
    free (nufa_buf);
    return -1;
  }
  /* Check if the local_volume specified is proper subvolume of unify */
  trav_xl = xl->children;
  while (trav_xl) {
    if (strcmp (data_to_str (local_name), trav_xl->xlator->name) == 0)
      break;
    trav_xl = trav_xl->next;
  }
  if (!trav_xl) {
    /* entry for 'local-volume-name' is wrong, not present in subvolumes */
    gf_log ("nufa", 
	    GF_LOG_ERROR, 
	    "option 'nufa.local-volume-name' is wrong\n");
    free (nufa_buf);
    return -1;
  }

  trav_xl = xl->children;
  while (trav_xl) {
    nufa_buf->array[index].xl = trav_xl->xlator;
    nufa_buf->array[index].eligible = 1;
    nufa_buf->array[index].free_disk = nufa_buf->min_free_disk;
    nufa_buf->array[index].refresh_interval = nufa_buf->refresh_interval;
    if (strcmp (trav_xl->xlator->name, local_name->data) == 0)
      nufa_buf->local_xl_idx = index;
    trav_xl = trav_xl->next;
    index++;
  }

  LOCK_INIT (&nufa_buf->nufa_lock);
  *((long *)xl->private) = (long)nufa_buf; // put it at the proper place
  return 0;
}

static void
nufa_fini (xlator_t *xl)
{
  struct nufa_struct *nufa_buf = (struct nufa_struct *)*((long *)xl->private);
  LOCK_DESTROY (&nufa_buf->nufa_lock);
  free (nufa_buf->array);
  free (nufa_buf);
}

static int32_t 
update_stat_array_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno,
		       struct xlator_stats *trav_stats)
{
  struct nufa_struct *nufa_struct = (struct nufa_struct *)*((long *)xl->private);
  int32_t idx = 0;
  int32_t percent = 0;
  
  LOCK (&nufa_struct->nufa_lock);
  for (idx = 0; idx < nufa_struct->child_count; idx++) {
    if (strcmp (nufa_struct->array[idx].xl->name, (char *)cookie) == 0)
      break;
  }
  UNLOCK (&nufa_struct->nufa_lock);

  if (op_ret == 0) {
    percent = (trav_stats->free_disk * 100) / trav_stats->total_disk_size;
    if (nufa_struct->array[idx].free_disk > percent) {
      if (nufa_struct->array[idx].eligible)
	gf_log ("nufa", GF_LOG_CRITICAL, 
		"node \"%s\" is _almost_ full", 
		nufa_struct->array[idx].xl->name);
      nufa_struct->array[idx].eligible = 0;
    } else {
      nufa_struct->array[idx].eligible = 1;
    }
  } else {
    nufa_struct->array[idx].eligible = 0;
  }

  STACK_DESTROY (frame->root);
  return 0;
}

static void 
update_stat_array (xlator_t *xl)
{
  /* This function schedules the file in one of the child nodes */
  int32_t idx;
  call_ctx_t *cctx;
  struct nufa_struct *nufa_buf = (struct nufa_struct *)*((long *)xl->private);

  for (idx = 0; idx < nufa_buf->child_count; idx++) {
    call_pool_t *pool = xl->ctx->pool;
    cctx = calloc (1, sizeof (*cctx));
    cctx->frames.root  = cctx;
    cctx->frames.this  = xl;    
    cctx->pool = pool;
    LOCK (&pool->lock);
    {
      list_add (&cctx->all_frames, &pool->all_frames);
    }
    UNLOCK (&pool->lock);

    _STACK_WIND ((&cctx->frames), 
		 update_stat_array_cbk, 
		 nufa_buf->array[idx].xl->name,
		 nufa_buf->array[idx].xl, 
		 (nufa_buf->array[idx].xl)->mops->stats,
		 0); //flag
  }
  return;
}

static void 
nufa_update (xlator_t *xl)
{
  struct nufa_struct *nufa_buf = (struct nufa_struct *)*((long *)xl->private);
  struct timeval tv;
  gettimeofday (&tv, NULL);
  if (tv.tv_sec > (nufa_buf->array[nufa_buf->local_xl_idx].refresh_interval 
		   + nufa_buf->array[nufa_buf->local_xl_idx].last_stat_fetch.tv_sec)) {
  /* Update the stats from all the server */
    update_stat_array (xl);
    nufa_buf->array[nufa_buf->local_xl_idx].last_stat_fetch.tv_sec = tv.tv_sec;
  }
}

static xlator_t *
nufa_schedule (xlator_t *xl, int32_t size)
{
  int32_t rr;
  int32_t nufa_orig;  
  struct nufa_struct *nufa_buf = (struct nufa_struct *)*((long *)xl->private);

  //TODO: Do i need to do this here?

  nufa_update (xl);

  if (nufa_buf->array[nufa_buf->local_xl_idx].eligible) {
    return nufa_buf->array[nufa_buf->local_xl_idx].xl;
  }

  nufa_orig = nufa_buf->sched_index;
  
  while (1) {
    LOCK (&nufa_buf->nufa_lock);
    rr = nufa_buf->sched_index++;
    nufa_buf->sched_index = nufa_buf->sched_index % nufa_buf->child_count;
    UNLOCK (&nufa_buf->nufa_lock);

    /* if 'eligible' or there are _no_ eligible nodes */
    if (nufa_buf->array[rr].eligible) {
      break;
    }
    if ((rr + 1) % nufa_buf->child_count == nufa_orig) {
      gf_log ("nufa", 
	      GF_LOG_CRITICAL, 
	      "free space not available on any server");
      LOCK (&nufa_buf->nufa_lock);
      nufa_buf->sched_index++;
      nufa_buf->sched_index = nufa_buf->sched_index % nufa_buf->child_count;
      UNLOCK (&nufa_buf->nufa_lock);
      break;
    }
  }
  return nufa_buf->array[rr].xl;
}


/**
 * notify
 */
void
nufa_notify (xlator_t *xl, int32_t event, void *data)
{
  struct nufa_struct *nufa_buf = (struct nufa_struct *)*((long *)xl->private);
  int32_t idx = 0;
  
  for (idx = 0; idx < nufa_buf->child_count; idx++) {
    if (strcmp (nufa_buf->array[idx].xl->name, ((xlator_t *)data)->name) == 0)
      break;
  }

  switch (event)
    {
    case GF_EVENT_CHILD_UP:
      {
	//nufa_buf->array[idx].eligible = 1;
      }
      break;
    case GF_EVENT_CHILD_DOWN:
      {
	nufa_buf->array[idx].eligible = 0;
      }
      break;
    default:
      {
	;
      }
      break;
    }

}

struct sched_ops sched = {
  .init     = nufa_init,
  .fini     = nufa_fini,
  .update   = nufa_update,
  .schedule = nufa_schedule,
  .notify   = nufa_notify
};
