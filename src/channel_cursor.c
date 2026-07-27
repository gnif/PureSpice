/**
 * PureSpice - A pure C implementation of the SPICE client protocol
 * Copyright © 2017-2025 Geoffrey McRae <geoff@hostfission.com>
 * https://github.com/gnif/PureSpice
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "purespice.h"

#include "ps.h"
#include "log.h"
#include "channel.h"
#include "channel_cursor.h"

#include <stddef.h>
#include <stdlib.h>

#include "messages.h"

const SpiceLinkHeader * channelCursor_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[CURSOR_CAPS_BYTES / sizeof(uint32_t)];
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
  const unsigned width  = (unsigned)header->width;
  const unsigned height = (unsigned)header->height;

  switch (header->type)
  {
    case SPICE_CURSOR_TYPE_ALPHA:
      return width * height * 4;

    case SPICE_CURSOR_TYPE_MONO:
      return (width + 7) / 8 * height * 2;

    case SPICE_CURSOR_TYPE_COLOR4:
      return (width + 1) / 2 * height + 16 * sizeof(uint32_t) +
        (width + 7) / 8 * height;

    case SPICE_CURSOR_TYPE_COLOR8:
      return width * height + 256 * sizeof(uint32_t) +
        (width + 7) / 8 * height;

    case SPICE_CURSOR_TYPE_COLOR16:
      return width * height * 2 +
        (width + 7) / 8 * height;

    case SPICE_CURSOR_TYPE_COLOR24:
      return width * height * 3 +
        (width + 7) / 8 * height;

    case SPICE_CURSOR_TYPE_COLOR32:
      return width * height * 4 +
        (width + 7) / 8 * height;
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

static struct PSCursorImage * convertCursor(
    SpiceCursor * cursor, size_t available, bool * valid)
{
  *valid = false;
  if (available < offsetof(SpiceCursor, header))
  {
    PS_LOG_ERROR("Cursor message is missing its flags");
    return NULL;
  }

  if (cursor->flags & SPICE_CURSOR_FLAGS_NONE)
  {
    *valid = true;
    return NULL;
  }

  if (available < sizeof(*cursor))
  {
    PS_LOG_ERROR("Cursor message is missing its cursor header");
    return NULL;
  }

  if (cursor->flags & SPICE_CURSOR_FLAGS_FROM_CACHE)
  {
    *valid = true;
    return loadCursor(cursor->header.unique);
  }

  if (cursor->header.width > 512 || cursor->header.height > 512)
  {
    PS_LOG_ERROR("Unexpected cursor size: %ux%u",
        cursor->header.width, cursor->header.height);
    return NULL;
  }

  size_t bufferSize = cursorBufferSize(&cursor->header);
  if (!bufferSize)
  {
    PS_LOG_ERROR("Unsupported cursor type: %u", cursor->header.type);
    return NULL;
  }

  if (bufferSize > available - sizeof(*cursor))
  {
    PS_LOG_ERROR("Cursor image exceeds its message payload "
        "(image: %zu, available: %zu)",
        bufferSize, available - sizeof(*cursor));
    return NULL;
  }

  struct PSCursorImage * node = malloc(sizeof(struct PSCursorImage) + bufferSize);
  if (!node)
  {
    PS_LOG_ERROR("Failed to allocate cursor image");
    return NULL;
  }

  node->cached = cursor->flags & SPICE_CURSOR_FLAGS_CACHE_ME;
  memcpy(&node->header, &cursor->header, sizeof(node->header));
  memcpy(node->buffer, cursor->data, bufferSize);

  *valid = true;
  return node;
}

static void cacheCursor(struct PSCursorImage * node)
{
  if (!node || !node->cached)
    return;

  struct PSCursorImage ** prev = &g_ps.cursor.cache;
  for(struct PSCursorImage * cached = g_ps.cursor.cache;
      cached; prev = &cached->next, cached = cached->next)
  {
    if (cached == node)
      return;

    if (cached->header.unique == node->header.unique)
    {
      *prev = cached->next;
      if (!cached->next)
        g_ps.cursor.cacheLast = prev;
      free(cached);
      break;
    }
  }

  node->next             = NULL;
  *g_ps.cursor.cacheLast = node;
  g_ps.cursor.cacheLast  = &node->next;
}

static bool cursorCacheContains(const struct PSCursorImage * cursor)
{
  for(const struct PSCursorImage * node = g_ps.cursor.cache;
      node; node = node->next)
    if (node == cursor)
      return true;

  return false;
}

static void clearCursorCache(bool preserveCurrent)
{
  struct PSCursorImage * node;
  struct PSCursorImage * next;
  for (node = g_ps.cursor.cache; node; node = next)
  {
    next = node->next;
    if (preserveCurrent && node == g_ps.cursor.current)
    {
      node->cached = false;
      node->next   = NULL;
    }
    else
      free(node);
  }

  g_ps.cursor.cache     = NULL;
  g_ps.cursor.cacheLast = &g_ps.cursor.cache;

  /* A cached current cursor should always be in the cache, but retain safe
   * ownership if an earlier cache miss or protocol reset broke that invariant. */
  if (preserveCurrent && g_ps.cursor.current)
    g_ps.cursor.current->cached = false;
}

static void clearCursorState(void)
{
  struct PSCursorImage * current = g_ps.cursor.current;
  const bool cacheOwnsCurrent =
    current && current->cached && cursorCacheContains(current);

  g_ps.cursor.current = NULL;
  if (current && !cacheOwnsCurrent)
    free(current);

  clearCursorCache(false);
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
      const unsigned width  = g_ps.cursor.current->header.width;
      const unsigned height = g_ps.cursor.current->header.height;
      const unsigned size   = (width + 7) / 8 * height;

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
  const size_t cursorOffset = offsetof(SpiceMsgCursorInit, cursor);
  if (!channel_validatePayload(
        channel, cursorOffset + offsetof(SpiceCursor, header), "CURSOR_INIT"))
    return PS_STATUS_ERROR;

  SpiceMsgCursorInit * msg = (SpiceMsgCursorInit *)channel->buffer;
  clearCursorState();

  bool valid;
  struct PSCursorImage * image = convertCursor(
      &msg->cursor, channel->header.size - cursorOffset, &valid);
  if (!valid)
    return PS_STATUS_ERROR;

  g_ps.cursor.x         = msg->position.x;
  g_ps.cursor.y         = msg->position.y;
  g_ps.cursor.visible   = msg->visible;
  g_ps.cursor.trailLen  = msg->trail_length;
  g_ps.cursor.trailFreq = msg->trail_frequency;

  g_ps.cursor.current = image;
  cacheCursor(image);

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
  clearCursorState();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorSet(PSChannel * channel)
{
  const size_t cursorOffset = offsetof(SpiceMsgCursorSet, cursor);
  if (!channel_validatePayload(
        channel, cursorOffset + offsetof(SpiceCursor, header), "CURSOR_SET"))
    return PS_STATUS_ERROR;

  SpiceMsgCursorSet * msg = (SpiceMsgCursorSet *)channel->buffer;
  bool valid;
  struct PSCursorImage * image = convertCursor(
      &msg->cursor, channel->header.size - cursorOffset, &valid);
  if (!valid)
    return PS_STATUS_ERROR;

  g_ps.cursor.x       = msg->position.x;
  g_ps.cursor.y       = msg->position.y;
  g_ps.cursor.visible = msg->visible;

  if (g_ps.cursor.current && !g_ps.cursor.current->cached)
    free(g_ps.cursor.current);

  g_ps.cursor.current = image;
  cacheCursor(image);

  if (!g_ps.cursor.current)
    g_ps.cursor.visible = false;

  updateCursorStatus();
  updateCursorImage();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorMove(PSChannel * channel)
{
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgCursorMove), "CURSOR_MOVE"))
    return PS_STATUS_ERROR;

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
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgCursorTrail), "CURSOR_TRAIL"))
    return PS_STATUS_ERROR;

  SpiceMsgCursorTrail * msg = (SpiceMsgCursorTrail *)channel->buffer;

  g_ps.cursor.trailLen  = msg->length;
  g_ps.cursor.trailFreq = msg->frequency;
  updateCursorTrail();

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_cursorInvalOne(PSChannel * channel)
{
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgCursorInvalOne), "CURSOR_INVAL_ONE"))
    return PS_STATUS_ERROR;

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

      if (node == g_ps.cursor.current)
      {
        node->cached = false;
        node->next   = NULL;
      }
      else
        free(node);
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

  clearCursorCache(true);

  return PS_STATUS_OK;
}

PSHandlerFn channelCursor_onMessage(PSChannel * channel)
{
  if (!channel->initDone)
  {
    if (channel->header.type == SPICE_MSG_CURSOR_INIT)
    {
      channel->initDone = true;
      return onMessage_cursorInit;
    }

    PS_LOG_ERROR("Expected SPICE_MSG_CURSOR_INIT but got %u",
        channel->header.type);
    return PS_HANDLER_ERROR;
  }

  switch(channel->header.type)
  {
    case SPICE_MSG_CURSOR_INIT:
      PS_LOG_ERROR("Unexpected SPICE_MSG_CURSOR_INIT");
      return PS_HANDLER_ERROR;

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
