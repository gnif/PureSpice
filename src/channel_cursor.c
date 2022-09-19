/*
PureSpice - A pure C implementation of the SPICE client protocol
Copyright (C) 2017-2022 Geoffrey McRae <geoff@hostfission.com>
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

#include "purespice.h"

#include "ps.h"
#include "log.h"
#include "channel.h"
#include "channel_cursor.h"

#include <stdlib.h>

#include "messages.h"

const SpiceLinkHeader * channelCursor_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[RECORD_CAPS_BYTES / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  static ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .channel_type     = SPICE_CHANNEL_CURSOR,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = CURSOR_CAPS_BYTES / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  p.message.connection_id = g_ps.sessionID;
  p.message.channel_id    = g_ps.channelID;

  memset(p.supportCaps, 0, sizeof(p.supportCaps));
  memset(p.channelCaps, 0, sizeof(p.channelCaps));

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  return &p.header;
}

static size_t cursorBufferSize(SpiceCursorHeader * header)
{
  switch (header->type)
  {
    case SPICE_CURSOR_TYPE_ALPHA:
      return header->width * header->height * 4;

    case SPICE_CURSOR_TYPE_MONO:
      return (header->width + 7) / 8 * header->height * 2;

    case SPICE_CURSOR_TYPE_COLOR4:
      return (header->width + 1) / 2 * header->height + 16 * sizeof(uint32_t) +
        (header->width + 7) / 8 * header->height;

    case SPICE_CURSOR_TYPE_COLOR8:
      return header->width * header->height + 256 * sizeof(uint32_t) +
        (header->width + 7) / 8 * header->height;

    case SPICE_CURSOR_TYPE_COLOR16:
      return header->width * header->height * 2 +
        (header->width + 7) / 8 * header->height;

    case SPICE_CURSOR_TYPE_COLOR24:
      return header->width * header->height * 3 +
        (header->width + 7) / 8 * header->height;

    case SPICE_CURSOR_TYPE_COLOR32:
      return header->width * header->height * 4 +
        (header->width + 7) / 8 * header->height;
  }

  return 0;
}

static struct PSCursorImage * loadCursor(uint64_t id)
{
  for (struct PSCursorImage * node = g_ps.cursor.cache; node; node = node->next)
    if (node->header.unique == id)
      return node;

  return NULL;
}

static struct PSCursorImage * convertCursor(SpiceCursor * cursor)
{
  if (cursor->flags & SPICE_CURSOR_FLAGS_NONE)
    return NULL;

  if (cursor->flags & SPICE_CURSOR_FLAGS_FROM_CACHE)
    return loadCursor(cursor->header.unique);

  size_t bufferSize = cursorBufferSize(&cursor->header);
  struct PSCursorImage * node = malloc(sizeof(struct PSCursorImage) + bufferSize);

  node->cached = cursor->flags & SPICE_CURSOR_FLAGS_CACHE_ME;
  memcpy(&node->header, &cursor->header, sizeof(node->header));
  memcpy(node->buffer, cursor->data, bufferSize);

  if (node->cached)
  {
    node->next             = NULL;
    *g_ps.cursor.cacheLast = node;
    g_ps.cursor.cacheLast  = &node->next;
  }

  return node;
}

static void clearCursorCache(void)
{
  struct PSCursorImage * node;
  struct PSCursorImage * next;
  for (node = g_ps.cursor.cache; node; node = next)
  {
    next = node->next;
    free(node);
  }

  g_ps.cursor.cache     = NULL;
  g_ps.cursor.cacheLast = &g_ps.cursor.cache;
}

static void updateCursorImage(void)
{
  if (!g_ps.cursor.current)
    return;

  switch (g_ps.cursor.current->header.type)
  {
    case SPICE_CURSOR_TYPE_ALPHA:
      g_ps.config.cursor.setRGBAImage(
        g_ps.cursor.current->header.width,
        g_ps.cursor.current->header.height,
        g_ps.cursor.current->header.hot_spot_x,
        g_ps.cursor.current->header.hot_spot_y,
        g_ps.cursor.current->buffer
      );
      break;

    case SPICE_CURSOR_TYPE_MONO:
    {
      size_t size = (g_ps.cursor.current->header.width + 7) / 8 *
        g_ps.cursor.current->header.height;

      const uint8_t * xorBuffer = g_ps.cursor.current->buffer;
      const uint8_t * andBuffer = xorBuffer + size;

      g_ps.config.cursor.setMonoImage(
        g_ps.cursor.current->header.width,
        g_ps.cursor.current->header.height,
        g_ps.cursor.current->header.hot_spot_x,
        g_ps.cursor.current->header.hot_spot_y,
        xorBuffer,
        andBuffer
      );
      break;
    }

    default:
      PS_LOG_ERROR("Attempt to use unsupported cursor type: %d",
        g_ps.cursor.current->header.type);
  }
}

static void updateCursorStatus(void)
{
  g_ps.config.cursor.setState(g_ps.cursor.visible, g_ps.cursor.x, g_ps.cursor.y);
}

static void updateCursorTrail(void)
{
  if (g_ps.config.cursor.setTrail)
    g_ps.config.cursor.setTrail(g_ps.cursor.trailLen, g_ps.cursor.trailFreq);
}

static PS_STATUS onMessage_cursorInit(PSChannel * channel)
{
  SpiceMsgCursorInit * msg = (SpiceMsgCursorInit *)channel->buffer;

  g_ps.cursor.x         = msg->position.x;
  g_ps.cursor.y         = msg->position.y;
  g_ps.cursor.visible   = msg->visible;
  g_ps.cursor.trailLen  = msg->trail_length;
  g_ps.cursor.trailFreq = msg->trail_frequency;

  g_ps.cursor.cache     = NULL;
  g_ps.cursor.cacheLast = &g_ps.cursor.cache;
  g_ps.cursor.current   = convertCursor(&msg->cursor);

  if (!g_ps.cursor.current)
    g_ps.cursor.visible = false;

  updateCursorImage();
  updateCursorStatus();
  updateCursorTrail();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorReset(PSChannel * channel)
{
  (void) channel;

  g_ps.cursor.visible = false;
  g_ps.cursor.current = NULL;

  struct PSCursorImage * node;
  struct PSCursorImage * next;
  for (node = g_ps.cursor.cache; node; node = next)
  {
    next = node->next;
    free(node);
  }

  clearCursorCache();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorSet(PSChannel * channel)
{
  SpiceMsgCursorSet * msg = (SpiceMsgCursorSet *)channel->buffer;

  g_ps.cursor.x       = msg->position.x;
  g_ps.cursor.y       = msg->position.y;
  g_ps.cursor.visible = msg->visible;

  if (g_ps.cursor.current && !g_ps.cursor.current->cached)
    free(g_ps.cursor.current);

  g_ps.cursor.current = convertCursor(&msg->cursor);

  if (!g_ps.cursor.current)
    g_ps.cursor.visible = false;

  updateCursorStatus();
  updateCursorImage();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorMove(PSChannel * channel)
{
  SpiceMsgCursorMove * msg = (SpiceMsgCursorMove *)channel->buffer;

  g_ps.cursor.x = msg->position.x;
  g_ps.cursor.y = msg->position.y;
  updateCursorStatus();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorHide(PSChannel * channel)
{
  (void) channel;

  g_ps.cursor.visible = false;
  updateCursorStatus();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorTrail(PSChannel * channel)
{
  SpiceMsgCursorTrail * msg = (SpiceMsgCursorTrail *)channel->buffer;

  g_ps.cursor.trailLen  = msg->length;
  g_ps.cursor.trailFreq = msg->frequency;
  updateCursorTrail();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorInvalOne(PSChannel * channel)
{
  SpiceMsgCursorInvalOne * msg = (SpiceMsgCursorInvalOne *)channel->buffer;

  struct PSCursorImage ** prev = &g_ps.cursor.cache;
  struct PSCursorImage  * node = g_ps.cursor.cache;

  while (node)
  {
    if (node->header.unique == msg->cursor_id)
    {
      *prev = node->next;
      if (!node->next)
        g_ps.cursor.cacheLast = prev;
      break;
    }

    prev = &node->next;
    node = node->next;
  }

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorInvalAll(PSChannel * channel)
{
  (void) channel;

  clearCursorCache();

  return PS_STATUS_OK;
}

PSHandlerFn channelCursor_onMessage(PSChannel * channel)
{
  channel->initDone = true;
  switch(channel->header.type)
  {
    case SPICE_MSG_CURSOR_INIT:
      return onMessage_cursorInit;

    case SPICE_MSG_CURSOR_RESET:
      return onMessage_cursorReset;

    case SPICE_MSG_CURSOR_SET:
      return onMessage_cursorSet;

    case SPICE_MSG_CURSOR_MOVE:
      return onMessage_cursorMove;

    case SPICE_MSG_CURSOR_HIDE:
      return onMessage_cursorHide;

    case SPICE_MSG_CURSOR_TRAIL:
      return onMessage_cursorTrail;

    case SPICE_MSG_CURSOR_INVAL_ONE:
      return onMessage_cursorInvalOne;

    case SPICE_MSG_CURSOR_INVAL_ALL:
      return onMessage_cursorInvalAll;
  }

  return PS_HANDLER_DISCARD;
}
