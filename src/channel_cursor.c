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

#include <stdio.h>

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

static PS_STATUS onMessage_cursorInit(PSChannel * channel)
{
  SpiceMsgCursorInit * msg = (SpiceMsgCursorInit *)channel->buffer;

  g_ps.cursor.x       = msg->position.x;
  g_ps.cursor.y       = msg->position.y;
  g_ps.cursor.visible = msg->visible;

  return PS_STATUS_OK;
}

PSHandlerFn channelCursor_onMessage(PSChannel * channel)
{
  channel->initDone = true;
  switch(channel->header.type)
  {
    case SPICE_MSG_CURSOR_INIT:
      return onMessage_cursorInit;
  }

  return PS_HANDLER_DISCARD;
}
