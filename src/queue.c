/*
PureSpice - A pure C implementation of the SPICE client protocol
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
https://github.com/gnif/PureSpice

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "queue.h"
#include "locking.h"

#include <stdlib.h>
#include <assert.h>

struct QueueItem
{
  void              * data;
  struct QueueItem * next;
};

struct Queue
{
  struct QueueItem * head;
  struct QueueItem * tail;
  struct QueueItem * pos;
  unsigned int count;
  atomic_flag lock;
};

struct Queue * queue_new(void)
{
  struct Queue * list = malloc(sizeof(struct Queue));
  list->head  = NULL;
  list->tail  = NULL;
  list->pos   = NULL;
  list->count = 0;
  SPICE_LOCK_INIT(list->lock);
  return list;
}

void queue_free(struct Queue * list)
{
  // never free a list with items in it!
  assert(!list->head);
  free(list);
}

void queue_push(struct Queue * list, void * data)
{
  struct QueueItem * item = malloc(sizeof(struct QueueItem));
  item->data = data;
  item->next = NULL;

  SPICE_LOCK(list->lock);
  ++list->count;

  if (!list->head)
  {
    list->head = item;
    list->tail = item;
    SPICE_UNLOCK(list->lock);
    return;
  }

  list->tail->next = item;
  list->tail       = item;
  SPICE_UNLOCK(list->lock);
}

bool queue_shift(struct Queue * list, void ** data)
{
  SPICE_LOCK(list->lock);
  if (!list->head)
  {
    SPICE_UNLOCK(list->lock);
    return false;
  }

  --list->count;
  struct QueueItem * item = list->head;
  list->head = item->next;
  list->pos  = NULL;
  if (list->tail == item)
    list->tail = NULL;

  SPICE_UNLOCK(list->lock);

  if (data)
    *data = item->data;

  free(item);
  return true;
}

bool queue_peek(struct Queue * list, void ** data)
{
  if (!list->head)
    return false;

  struct QueueItem * item = list->head;
  if (data)
    *data = item->data;

  return true;
}
