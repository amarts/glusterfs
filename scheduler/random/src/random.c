#include <stdlib.h>
#include "random.h"

static int
random_init (struct xlator *xl)
{
  struct random_struct *random_buf = calloc (1, sizeof (struct random_struct));
  struct xlator *trav_xl = xl->first_child;
  
  int index = 0;

  while (trav_xl) {
    index++;
    trav_xl = trav_xl->next_sibling;
  }
  random_buf->child_count = index;
  random_buf->array = calloc (index, sizeof (struct random_sched_struct));
  trav_xl = xl->first_child;
  index = 0;

  while (trav_xl) {
    random_buf->array[index].xl = trav_xl;
    random_buf->array[index].eligible = 1;
    trav_xl = trav_xl->next_sibling;
    index++;
  }
  
  *((int *)xl->private) = (int)random_buf; // put it at the proper place
  return 0;
}

static void
random_fini (struct xlator *xl)
{
  struct random_struct *random_buf = (struct random_struct *)*((int *)xl->private);
  free (random_buf->array);
  free (random_buf);
}

static struct xlator *
random_schedule (struct xlator *xl, int size)
{
  struct random_struct *random_buf = (struct random_struct *)*((int *)xl->private);
  int rand = random () % random_buf->child_count;
  while (!random_buf->array[rand].eligible) {
    rand = random () % random_buf->child_count;
  }
  return random_buf->array[rand].xl;
}

struct sched_ops sched = {
  .init     = random_init,
  .fini     = random_fini,
  .schedule = random_schedule
};
