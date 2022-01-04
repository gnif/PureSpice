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

  cm.ready = true;
  if (g_ps.config.ready)
    g_ps.config.ready();
}

PS_STATUS channelMain_onRead(struct PSChannel * channel, int * dataAvailable)
{
  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = channel_onRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  if (!channel->initDone)
  {
    if (header.type != SPICE_MSG_MAIN_INIT)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Expected SPICE_MSG_MAIN_INIT but got %d", header.type);
      return PS_STATUS_ERROR;
    }

    channel->initDone = true;
    SpiceMsgMainInit msg;
    if ((status = channel_readNL(channel, &msg, sizeof(msg),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read SpiceMsgMainInit");
      return status;
    }

    g_ps.sessionID = msg.session_id;
    agent_setServerTokens(msg.agent_tokens);

    if (msg.agent_connected)
    {
      if ((status = agent_connect()) != PS_STATUS_OK)
      {
        purespice_disconnect();
        return status;
      }
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT &&
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

  if (header.type == SPICE_MSG_MAIN_NAME)
  {
    SpiceMsgMainName *msg = (SpiceMsgMainName*)alloca(header.size);
    if ((status = channel_readNL(channel, msg, header.size,
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read SpiceMsgMainName");
      return status;
    }

    PS_LOG_INFO("Guest name: %s", msg->name);

    if (g_ps.guestName)
      free(g_ps.guestName);

    g_ps.guestName = strdup((char *)msg->name);
    cm.hasName = true;

    checkReady();
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_UUID)
  {
    SpiceMsgMainUUID msg;
    if ((status = channel_readNL(channel, &msg, sizeof(msg),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read SpiceMsgMainUUID");
      return status;
    }

    PS_LOG_INFO("Guest UUID: "
        "%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
        *(uint32_t*)&msg.uuid[0],
        *(uint16_t*)&msg.uuid[4],
        *(uint16_t*)&msg.uuid[6],
        *(uint16_t*)&msg.uuid[8],
        msg.uuid[10], msg.uuid[11], msg.uuid[12],
        msg.uuid[13], msg.uuid[14], msg.uuid[15]);

    memcpy(g_ps.guestUUID, msg.uuid, sizeof(g_ps.guestUUID));
    cm.hasUUID = true;

    checkReady();
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_CHANNELS_LIST)
  {
    SpiceMainChannelsList *msg = (SpiceMainChannelsList*)alloca(header.size);
    if ((status = channel_readNL(channel, msg, header.size,
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read SpiceMainChannelsList");
      return status;
    }

    for(int i = 0; i < msg->num_of_channels; ++i)
      for(int n = 0; n < PS_CHANNEL_MAX; ++n)
      {
        struct PSChannel * ch = &g_ps.channels[n];
        if (ch->spiceType != msg->channels[i].type ||
            (ch->enable && !*ch->enable))
          continue;

        if (ch->connected)
        {
          purespice_disconnect();
          PS_LOG_ERROR("Protocol error. The server asked us to reconnect an "
              "already connected channel (%s)", ch->name);
          return PS_STATUS_ERROR;
        }

        if ((status = channel_connect(ch)) != PS_STATUS_OK)
        {
          purespice_disconnect();
          PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
          return PS_STATUS_ERROR;
        }

        PS_LOG_INFO("%s channel connected", ch->name);
        break;
      }

    checkReady();
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED)
  {
    if ((status = agent_connect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS)
  {
    uint32_t num_tokens;
    if ((status = channel_readNL(channel, &num_tokens, sizeof(num_tokens),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read the number of agent tokens");
      return status;
    }

    agent_setServerTokens(num_tokens);
    if ((status = agent_connect()) != PS_STATUS_OK)
    {
      purespice_disconnect();
      return status;
    }

    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DISCONNECTED)
  {
    uint32_t error;
    if ((status = channel_readNL(channel, &error, sizeof(error),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read SPICE_MSG_MAIN_AGENT_DISCONNECTED");
      return status;
    }

    agent_disconnect();
    PS_LOG_WARN("Disconnected from the spice guest agent: %u", error);
    return PS_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DATA)
  {
    if (!agent_present())
      if ((status = channel_discardNL(channel, header.size,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to discard agent data");
        return status;;
      }

    if ((status = agent_process(header.size,
            dataAvailable)) != PS_STATUS_OK)
    {
      PS_LOG_ERROR("Failed to process agent data");
      purespice_disconnect();
    }

    return status;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_TOKEN)
  {
    uint32_t num_tokens;
    if ((status = channel_readNL(channel, &num_tokens, sizeof(num_tokens),
            dataAvailable)) != PS_STATUS_OK)
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to read the number of agent tokens");
      return status;
    }

    agent_returnServerTokens(num_tokens);
    if (!agent_processQueue())
    {
      purespice_disconnect();
      PS_LOG_ERROR("Failed to process the agent queue");
      return PS_STATUS_ERROR;
    }

    return PS_STATUS_OK;
  }

  return channel_discardNL(channel, header.size, dataAvailable);
}
