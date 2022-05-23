/*
PureSpice - A pure C implementation of the SPICE client protocol
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#include "log.h"
#include "channel_main.h"
#include "channel.h"
#include "agent.h"
#include "messages.h"

#include <stdlib.h>

struct ChannelMain
{
  bool ready;

  bool capAgentTokens;
  bool capNameAndUUID;
  bool hasName;
  bool hasUUID;
  bool hasList;
};

static struct ChannelMain cm = { 0 };

const SpiceLinkHeader * channelMain_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[MAIN_CAPS_BYTES   / sizeof(uint32_t)];
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
      .channel_type     = SPICE_CHANNEL_MAIN,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = MAIN_CAPS_BYTES   / sizeof(uint32_t),
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

  MAIN_SET_CAPABILITY(p.channelCaps, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);
  MAIN_SET_CAPABILITY(p.channelCaps, SPICE_MAIN_CAP_NAME_AND_UUID         );

  memset(&cm, 0, sizeof(cm));

  return &p.header;
}

void channelMain_setCaps(const uint32_t * common, int numCommon,
    const uint32_t * channel, int numChannel)
{
  /* for whatever reason the spice server does not report that it supports these
   * capabilities so we are just going to assume it does until the below PR is
   * merged, or indefiniately if it's rejected.
   * https://gitlab.freedesktop.org/spice/spice/-/merge_requests/198
   */
#if 0
  cm.capAgentTokens = HAS_CAPABILITY(channel, numChannel,
      SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);
  cm.capNameAndUUID = HAS_CAPABILITY(channel, numChannel,
      SPICE_MAIN_CAP_NAME_AND_UUID);
#else
  (void) common;
  (void) numCommon;
  (void) channel;
  (void) numChannel;
  cm.capAgentTokens = true;
  cm.capNameAndUUID = true;
#endif
}

static void checkReady(void)
{
  if (cm.ready)
    return;

  if (cm.capNameAndUUID)
  {
    if (!cm.hasName || !cm.hasUUID)
      return;
  }

  if (!cm.hasList)
    return;

  cm.ready = true;
  if (g_ps.config.ready)
    g_ps.config.ready();
}

static PS_STATUS onMessage_mainInit(struct PSChannel * channel)
{
  channel->initDone = true;

  SpiceMsgMainInit * msg = (SpiceMsgMainInit *)channel->buffer;
  g_ps.sessionID = msg->session_id;
  agent_setServerTokens(msg->agent_tokens);

  if (msg->agent_connected)
  {
    PS_STATUS status;
    if ((status = agent_connect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }
  }

  if (msg->current_mouse_mode != SPICE_MOUSE_MODE_CLIENT &&
      !purespice_mouseMode(false))
  {
    PS_LOG_ERROR("Failed to set the initial mouse mode");
    return PS_STATUS_ERROR;
  }

  void * packet = SPICE_RAW_PACKET(SPICE_MSGC_MAIN_ATTACH_CHANNELS, 0, 0);
  if (!SPICE_SEND_PACKET(channel, packet))
  {
    purespice_disconnect();
    PS_LOG_ERROR("Failed to write SPICE_MSGC_MAIN_ATTACH_CHANNELS");
    return PS_STATUS_ERROR;
  }

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainName(struct PSChannel * channel)
{
  SpiceMsgMainName * msg = (SpiceMsgMainName *)channel->buffer;
  PS_LOG_INFO("Guest name: %s", msg->name);

  if (g_ps.guestName)
    free(g_ps.guestName);

  g_ps.guestName = strdup((char *)msg->name);
  cm.hasName = true;

  checkReady();
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainUUID(struct PSChannel * channel)
{
  SpiceMsgMainUUID * msg = (SpiceMsgMainUUID *)channel->buffer;

  PS_LOG_INFO("Guest UUID: "
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      msg->uuid[ 0], msg->uuid[ 1], msg->uuid[ 2], msg->uuid[ 3], msg->uuid[ 4],
      msg->uuid[ 5], msg->uuid[ 6], msg->uuid[ 7], msg->uuid[ 8], msg->uuid[ 9],
      msg->uuid[10], msg->uuid[11], msg->uuid[12], msg->uuid[13], msg->uuid[14],
      msg->uuid[15]);

  memcpy(g_ps.guestUUID, msg->uuid, sizeof(g_ps.guestUUID));
  cm.hasUUID = true;

  checkReady();
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainChannelsList(struct PSChannel * channel)
{
  SpiceMainChannelsList * msg = (SpiceMainChannelsList *)channel->buffer;

  for(int n = 0; n < PS_CHANNEL_MAX; ++n)
  {
    struct PSChannel * ch = &g_ps.channels[n];
    ch->available = false;
  }

  for(size_t i = 0; i < msg->num_of_channels; ++i)
    for(int n = 0; n < PS_CHANNEL_MAX; ++n)
    {
      struct PSChannel * ch = &g_ps.channels[n];
      if (ch->spiceType != msg->channels[i].type)
       continue;

      ch->available = true;
      if ((ch->enable && !*ch->enable) || (ch->autoConnect && !*ch->autoConnect))
        continue;

      if (ch->connected)
      {
        purespice_disconnect();
        PS_LOG_ERROR("Protocol error. The server asked us to reconnect an "
            "already connected channel (%s)", ch->name);
        return PS_STATUS_ERROR;
      }

      PS_STATUS status = ps_connectChannel(ch);
      if (status != PS_STATUS_OK)
      {
        purespice_disconnect();
        PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
        return status;
      }

      break;
    }

  cm.hasList = true;
  checkReady();
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainAgentConnected(struct PSChannel * channel)
{
  (void)channel;

  PS_STATUS status;
  if ((status = agent_connect()) != PS_STATUS_OK)
  {
    purespice_disconnect();
    return status;
  }

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainAgentConnectedTokens(struct PSChannel * channel)
{
  uint32_t num_tokens = *(uint32_t *)channel->buffer;

  agent_setServerTokens(num_tokens);
  return onMessage_mainAgentConnected(channel);
}

static PS_STATUS onMessage_mainAgentDisconnected(struct PSChannel * channel)
{
  uint32_t error = *(uint32_t *)channel->buffer;

  agent_disconnect();
  PS_LOG_WARN("Disconnected from the spice guest agent: %u", error);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_mainAgentData(struct PSChannel * channel)
{
  PS_STATUS status;
  if ((status = agent_process(channel)) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("Failed to process agent data");
    purespice_disconnect();
  }

  return status;
}

static PS_STATUS onMessage_mainAgentToken(struct PSChannel * channel)
{
  uint32_t num_tokens = *(uint32_t *)channel->buffer;

  agent_returnServerTokens(num_tokens);
  if (!agent_processQueue())
  {
    purespice_disconnect();
    PS_LOG_ERROR("Failed to process the agent queue");
    return PS_STATUS_ERROR;
  }

  return PS_STATUS_OK;
}

PSHandlerFn channelMain_onMessage(struct PSChannel * channel)
{
  if (!channel->initDone)
  {
    if (channel->header.type == SPICE_MSG_MAIN_INIT)
      return onMessage_mainInit;

    purespice_disconnect();
    PS_LOG_ERROR("Expected SPICE_MSG_MAIN_INIT but got %d", channel->header.type);
    return PS_HANDLER_ERROR;
  }

  switch(channel->header.type)
  {
    case SPICE_MSG_MAIN_INIT:
      purespice_disconnect();
      PS_LOG_ERROR("Unexpected SPICE_MSG_MAIN_INIT");
      return PS_HANDLER_ERROR;

    case SPICE_MSG_MAIN_NAME:
      return onMessage_mainName;

    case SPICE_MSG_MAIN_UUID:
      return onMessage_mainUUID;

    case SPICE_MSG_MAIN_CHANNELS_LIST:
      return onMessage_mainChannelsList;

    case SPICE_MSG_MAIN_MOUSE_MODE:
      return PS_HANDLER_DISCARD;

    case SPICE_MSG_MAIN_MULTI_MEDIA_TIME:
      return PS_HANDLER_DISCARD;

    case SPICE_MSG_MAIN_AGENT_CONNECTED:
      return onMessage_mainAgentConnected;

    case SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS:
      return onMessage_mainAgentConnectedTokens;

    case SPICE_MSG_MAIN_AGENT_DISCONNECTED:
      return onMessage_mainAgentDisconnected;

    case SPICE_MSG_MAIN_AGENT_DATA:
      if (!agent_present())
        return PS_HANDLER_DISCARD;
      return onMessage_mainAgentData;

    case SPICE_MSG_MAIN_AGENT_TOKEN:
      return onMessage_mainAgentToken;
  }

  return PS_HANDLER_ERROR;
}
