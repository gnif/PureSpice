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
        if (ch->spiceType != msg->channels[i].type)
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
