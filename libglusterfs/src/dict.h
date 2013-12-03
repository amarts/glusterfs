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
  License aint64_t with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#ifndef _DICT_H
#define _DICT_H

#include <inttypes.h>

typedef struct _data data_t;
typedef struct _dict dict_t;
typedef struct _data_pair data_pair_t;

struct _data {
  int32_t len;
  char *data;
  char is_static;
  char is_const;
};

struct _data_pair {
  struct _data_pair *hash_next;
  struct _data_pair *prev;
  struct _data_pair *next;
  data_t *value;
  char *key;
};

struct _dict {
  char is_static;
  int32_t hash_size;
  int32_t count;
  data_pair_t **members;
  data_pair_t *members_list;
};

int32_t is_data_equal (data_t *one, data_t *two);
void data_destroy (data_t *data);

int32_t dict_set (dict_t *this, char *key, data_t *value);
data_t *dict_get (dict_t *this, char *key);
void dict_del (dict_t *this, char *key);

int32_t dict_serialized_length (dict_t *dict);
int32_t dict_serialize (dict_t *dict, char *buf);
dict_t *dict_unserialize (char *buf, int32_t size, dict_t **fill);
			  
void dict_destroy (dict_t *dict);

/* 
   TODO: provide converts for differnt byte sizes, signedness, and void *
 */
data_t *int_to_data (int64_t value);
data_t *str_to_data (char *value);
data_t *bin_to_data (void *value, int32_t len);
data_t *static_str_to_data (char *value);
data_t *static_bin_to_data (void *value);

int64_t data_to_int (data_t *data);
char *data_to_str (data_t *data);
void *data_to_bin (data_t *data);

data_t *get_new_data ();
dict_t *get_new_dict_full (int size_hint);
dict_t *get_new_dict ();

data_pair_t *get_new_data_pair ();

void dict_foreach (dict_t *this,
		   void (*fn)(dict_t *this,
			      char *key,
			      data_t *value,
			      void *data),
		   void *data);
dict_t *dict_copy (dict_t *this);

#define STATIC_DICT {1, 15, 0, NULL, NULL};
#define STATIC_DATA_STR(str) {strlen (str) + 1, str, 1, 1};

#endif
