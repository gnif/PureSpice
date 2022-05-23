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

#include "purespice.h"

#include "ps.h"
#include "log.h"
#include "agent.h"
#include "channel.h"
#include "channel_main.h"
#include "channel_inputs.h"
#include "channel_playback.h"
#include "channel_record.h"
#include "channel_display.h"

#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>

#include <sys/ioctl.h>
#include <sys/epoll.h>

#include <spice/vd_agent.h>

// globals
PS g_ps =
{
  .channels =
  {
    // PS_CHANNEL_MAIN
    {
      .spiceType        = SPICE_CHANNEL_MAIN,
      .name             = "MAIN",
      .getConnectPacket = channelMain_getConnectPacket,
      .setCaps          = channelMain_setCaps,
      .onMessage        = channelMain_onMessage
    },
    // PS_CHANNEL_INPUTS
    {
      .spiceType        = SPICE_CHANNEL_INPUTS,
      .name             = "INPUTS",
      .enable           = &g_ps.config.inputs.enable,
      .autoConnect      = &g_ps.config.inputs.autoConnect,
      .getConnectPacket = channelInputs_getConnectPacket,
      .onMessage        = channelInputs_onMessage
    },
    // PS_CHANNEL_PLAYBACK
    {
      .spiceType        = SPICE_CHANNEL_PLAYBACK,
      .name             = "PLAYBACK",
      .enable           = &g_ps.config.playback.enable,
      .autoConnect      = &g_ps.config.playback.autoConnect,
      .getConnectPacket = channelPlayback_getConnectPacket,
      .onMessage        = channelPlayback_onMessage
    },
    // PS_CHANNEL_RECORD
    {
      .spiceType        = SPICE_CHANNEL_RECORD,
      .name             = "RECORD",
      .enable           = &g_ps.config.record.enable,
      .autoConnect      = &g_ps.config.record.autoConnect,
      .getConnectPacket = channelRecord_getConnectPacket,
      .onMessage        = channelRecord_onMessage,
    },
    {
      .spiceType        = SPICE_CHANNEL_DISPLAY,
      .name             = "DISPLAY",
      .enable           = &g_ps.config.display.enable,
      .autoConnect      = &g_ps.config.display.autoConnect,
      .getConnectPacket = channelDisplay_getConnectPacket,
      .onConnect        = channelDisplay_onConnect,
      .onMessage        = channelDisplay_onMessage
    }
  }
};

void purespice_init(const PSInit * init)
{
  if (init)
    memcpy(&g_ps.init, init, sizeof(*init));
  log_init();
  g_ps.initialized = true;
}

bool purespice_connect(const PSConfig * config)
{
  if (!g_ps.initialized)
  {
    log_init();
    g_ps.initialized = true;
  }

  memcpy(&g_ps.config, config, sizeof(*config));

  g_ps.config.host = (const char *)strdup(config->host);
  if (!g_ps.config.host)
  {
    PS_LOG_ERROR("Failed to malloc");
    goto err_host;
  }

  g_ps.config.password = (const char *)strdup(config->password);
  if (!g_ps.config.password)
  {
    PS_LOG_ERROR("Failed to malloc");
    goto err_password;
  }

  if (g_ps.config.clipboard.enable)
  {
    if (!g_ps.config.clipboard.notice)
    {
      PS_LOG_ERROR("clipboard->notice is mandatory");
      goto err_config;
    }

    if (!g_ps.config.clipboard.data)
    {
      PS_LOG_ERROR("clipboard->data is mandatory");
      goto err_config;
    }

    if (!g_ps.config.clipboard.release)
    {
      PS_LOG_ERROR("clipboard->release is mandatory");
      goto err_config;
    }

    if (!g_ps.config.clipboard.request)
    {
      PS_LOG_ERROR("clipboard->request is mandatory");
      goto err_config;
    }
  }

  if (g_ps.config.playback.enable)
  {
    if (!g_ps.config.playback.start)
    {
      PS_LOG_ERROR("playback->start is mandatory");
      goto err_config;
    }

    if (!g_ps.config.playback.stop)
    {
      PS_LOG_ERROR("playback->stop is mandatory");
      goto err_config;
    }

    if (!g_ps.config.playback.data)
    {
      PS_LOG_ERROR("playback->data is mandatory");
      goto err_config;
    }
  }

  if (g_ps.config.record.enable)
  {
    if (!g_ps.config.record.start)
    {
      PS_LOG_ERROR("record->start is mandatory");
      goto err_config;
    }

    if (!g_ps.config.record.stop)
    {
      PS_LOG_ERROR("record->stop is mandatory");
      goto err_config;
    }
  }

  if (g_ps.config.display.enable)
  {
    if (!g_ps.config.display.surfaceCreate)
    {
      PS_LOG_ERROR("display->surfaceCreate is mandatory");
      goto err_config;
    }

    if (!g_ps.config.display.surfaceDestroy)
    {
      PS_LOG_ERROR("display->surfaceDestroy is mandatory");
      goto err_config;
    }

    if (!g_ps.config.display.drawBitmap)
    {
      PS_LOG_ERROR("display->drawBitmap is mandatory");
      goto err_config;
    }

    if (!g_ps.config.display.drawFill)
    {
      PS_LOG_ERROR("display->drawFill is mandatory");
      goto err_config;
    }
  }

  memset(&g_ps.addr, 0, sizeof(g_ps.addr));

  if (g_ps.config.port == 0)
  {
    PS_LOG_INFO("Connecting to unix socket %s", g_ps.config.host);

    g_ps.family = AF_UNIX;
    g_ps.addr.un.sun_family = g_ps.family;
    strncpy(g_ps.addr.un.sun_path, g_ps.config.host,
        sizeof(g_ps.addr.un.sun_path) - 1);
  }
  else
  {
    PS_LOG_INFO("Connecting to socket %s:%u",
        g_ps.config.host, g_ps.config.port);

    g_ps.family = AF_INET;
    inet_pton(g_ps.family, g_ps.config.host, &g_ps.addr.in.sin_addr);
    g_ps.addr.in.sin_family = g_ps.family;
    g_ps.addr.in.sin_port   = htons(g_ps.config.port);
  }

  g_ps.epollfd = epoll_create1(0);
  if (g_ps.epollfd < 0)
  {
    PS_LOG_ERROR("epoll_create1 failed");
    goto err_config;
  }

  g_ps.channelID = 0;
  if (channel_connect(&g_ps.channels[0]) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("channel connect failed");
    goto err_connect;
  }

  PS_LOG_INFO("Connected");
  g_ps.connected = true;
  return true;

err_connect:
  close(g_ps.epollfd);

err_config:
  free((char *)g_ps.config.host);
  g_ps.config.host = NULL;

err_password:
  free((char *)g_ps.config.password);
  g_ps.config.password = NULL;

err_host:
  return false;
}

void purespice_disconnect()
{
  if (!g_ps.initialized)
  {
    log_init();
    g_ps.initialized = true;
  }

  const bool wasConnected = g_ps.connected;
  g_ps.connected = false;

  for(int i = PS_CHANNEL_MAX - 1; i >= 0; --i)
    channel_internal_disconnect(&g_ps.channels[i]);

  close(g_ps.epollfd);

  if (g_ps.motionBuffer)
  {
    free(g_ps.motionBuffer);
    g_ps.motionBuffer = NULL;
  }

  if (g_ps.config.host)
  {
    free((char *)g_ps.config.host);
    g_ps.config.host = NULL;
  }

  if (g_ps.config.password)
  {
    free((char *)g_ps.config.password);
    g_ps.config.password = NULL;
  }

  if (g_ps.guestName)
  {
    free(g_ps.guestName);
    g_ps.guestName = NULL;
  }

  agent_disconnect();

  if (wasConnected)
    PS_LOG_INFO("Disconnected");
}

PSStatus purespice_process(int timeout)
{
  static struct epoll_event events[PS_CHANNEL_MAX];

  // check for pending disconnects
  for(int i = 0; i < PS_CHANNEL_MAX; ++i)
    if (g_ps.channels[i].initDone && g_ps.channels[i].doDisconnect)
      channel_internal_disconnect(&g_ps.channels[i]);

  int nfds = epoll_wait(g_ps.epollfd, events, PS_CHANNEL_MAX, timeout);
  if (nfds == 0 || (nfds < 0 && errno == EINTR))
    return PS_STATUS_RUN;

  if (nfds < 0)
  {
    if (!g_ps.connected)
    {
      PS_LOG_INFO("Shutdown");
      return PS_STATUS_SHUTDOWN;
    }

    PS_LOG_ERROR("epoll_err returned %d", nfds);
    return PS_STATUS_ERR_POLL;
  }

  // process each channel one message at a time to avoid stalling a channel

  int done = 0;
  while(done < nfds)
  {
    for(int i = 0; i < nfds; ++i)
    {
      if (!events[i].data.ptr)
        continue;

      PSChannel * channel = (PSChannel *)events[i].data.ptr;

      int dataAvailable;
      ioctl(channel->socket, FIONREAD, &dataAvailable);

      // check if the socket has been disconnected
      if (!dataAvailable)
        goto done_disconnect;

      // if we don't have a header yet, read it
      if (channel->headerRead < sizeof(SpiceMiniDataHeader))
      {
        int       size = sizeof(SpiceMiniDataHeader)   - channel->headerRead;
        uint8_t * dst  = ((uint8_t *)&channel->header) + channel->headerRead;

        if (size > dataAvailable)
          size = dataAvailable;

        ssize_t len = read(channel->socket, dst, size);
        if (len == 0)
          goto done_disconnect;

        if (len < 0)
        {
          PS_LOG_ERROR("%s: Failed to read from the socket: %ld",
              channel->name, len);
          return PS_STATUS_ERR_READ;
        }

        // check if we have a complete header
        channel->headerRead += len;
        if (channel->headerRead < sizeof(SpiceMiniDataHeader))
          continue;

        // ack that we got the message
        if (!channel_ack(channel))
        {
          PS_LOG_ERROR("%s: Failed to send message ack", channel->name);
          return PS_STATUS_ERR_ACK;
        }

        dataAvailable -= len;
        channel->bufferRead = 0;
        if (channel->header.type < SPICE_MSG_BASE_LAST)
          channel->handlerFn = channel_onMessage(channel);
        else
          channel->handlerFn = channel->onMessage(channel);

        if (channel->handlerFn == PS_HANDLER_ERROR)
        {
          PS_LOG_ERROR("%s: invalid message: %d",
              channel->name, channel->header.type);
          return PS_STATUS_ERR_READ;
        }

        if (channel->handlerFn == PS_HANDLER_DISCARD)
        {
          channel->discarding  = true;
          channel->discardSize = channel->header.size;
        }
        else
        {
          // ensure we have a large enough buffer to read the entire message
          if (channel->bufferSize < channel->header.size)
          {
            free(channel->buffer);
            channel->buffer = malloc(channel->header.size);
            if (!channel->buffer)
            {
              PS_LOG_ERROR("out of memory");
              return PS_STATUS_ERR_READ;
            }
            channel->bufferSize = channel->header.size;
          }
        }
      }

      // check if we are discarding data
      if (channel->discarding)
      {
        while(channel->discardSize && dataAvailable)
        {
          char temp[8192];
          unsigned int discard =
            channel->discardSize > (unsigned int)dataAvailable ?
            (unsigned int)dataAvailable : channel->discardSize;
          if (discard > sizeof(temp))
            discard = sizeof(temp);

          ssize_t len = read(channel->socket, temp, discard);
          if (len == 0)
            goto done_disconnect;

          if (len < 0)
          {
            PS_LOG_ERROR("%s: Failed to discard from the socket: %ld",
                channel->name, len);
            return PS_STATUS_ERR_READ;
          }

          dataAvailable        -= len;
          channel->discardSize -= len;
        }

        if (!channel->discardSize)
        {
          channel->discarding = false;
          channel->headerRead = 0;
        }
      }
      else
      {
        // read the payload into the buffer
        int size = channel->header.size - channel->bufferRead;
        if (size)
        {
          if (size > dataAvailable)
            size = dataAvailable;
          ssize_t len = read(channel->socket,
              channel->buffer + channel->bufferRead, size);

          if (len == 0)
            goto done_disconnect;

          if (len < 0)
          {
            PS_LOG_ERROR("%s: Failed to read the message payload: %ld",
                channel->name, len);
            return PS_STATUS_ERR_READ;
          }

          dataAvailable       -= len;
          channel->bufferRead += len;
        }

        // if we have the full payload call the channel handler to process it
        if (channel->bufferRead == channel->header.size)
        {
          channel->headerRead = 0;

          // process the data
          switch(channel->handlerFn(channel))
          {
            case PS_STATUS_OK:
            case PS_STATUS_HANDLED:
              break;

            case PS_STATUS_NODATA:
              goto done_disconnect;

            default:
              PS_LOG_ERROR("%s: Handler reported read error", channel->name);
              return PS_STATUS_ERR_READ;
          }
        }
      }

      // if there is no more data, we are finished processing this channel
      if (dataAvailable == 0)
      {
        ++done;
        events[i].data.ptr = NULL;
      }

      continue;

done_disconnect:
      ++done;
      events[i].data.ptr = NULL;
      channel_internal_disconnect(channel);
    }
  }

  for(int i = 0; i < PS_CHANNEL_MAX; ++i)
    if (g_ps.channels[i].connected)
      return PS_STATUS_RUN;

  g_ps.sessionID = 0;

  for(int i = PS_CHANNEL_MAX - 1; i >= 0; --i)
    close(g_ps.channels[i].socket);

  PS_LOG_INFO("Shutdown");
  return PS_STATUS_SHUTDOWN;
}

bool purespice_getServerInfo(PSServerInfo * info)
{
  if (!g_ps.guestName)
    return false;

  memcpy(info->uuid, g_ps.guestUUID, sizeof(g_ps.guestUUID));
  info->name = strdup(g_ps.guestName);

  return true;
}

void purespice_freeServerInfo(PSServerInfo * info)
{
  if (!info)
    return;

  if (info->name)
    free(info->name);
}

static uint8_t channelTypeToSpiceType(PSChannelType channel)
{
  switch(channel)
  {
    case PS_CHANNEL_MAIN:
      return SPICE_CHANNEL_MAIN;

    case PS_CHANNEL_INPUTS:
      return SPICE_CHANNEL_INPUTS;

    case PS_CHANNEL_PLAYBACK:
      return SPICE_CHANNEL_PLAYBACK;

    case PS_CHANNEL_RECORD:
      return SPICE_CHANNEL_RECORD;

    case PS_CHANNEL_DISPLAY:
      return SPICE_CHANNEL_DISPLAY;

    default:
      PS_LOG_ERROR("Invalid channel");
      return 255;
  };

  __builtin_unreachable();
}

static PSChannel * getChannel(PSChannelType channel)
{
  const uint8_t spiceType = channelTypeToSpiceType(channel);
  if (spiceType == 255)
    return NULL;

  for(int i = 0; i < PS_CHANNEL_MAX; ++i)
    if (g_ps.channels[i].spiceType == spiceType)
      return &g_ps.channels[i];

  __builtin_unreachable();
}

bool purespice_hasChannel(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;

  return ch->available;
}

bool purespice_channelConnected(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;
  return ch->connected;
}

PS_STATUS ps_connectChannel(PSChannel * ch)
{
  PS_STATUS status;
  if ((status = channel_connect(ch)) != PS_STATUS_OK)
  {
    purespice_disconnect();
    PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
    return status;
  }

  PS_LOG_INFO("%s channel connected", ch->name);
  if (ch->onConnect && (status = ch->onConnect(ch)) != PS_STATUS_OK)
  {
    purespice_disconnect();
    PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
    return status;
  }

  return PS_STATUS_OK;
}

bool purespice_connectChannel(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;

  if (!ch->available)
  {
    PS_LOG_ERROR("%s: Channel is not availble", ch->name);
    return false;
  }

  if (ch->connected)
    return true;

  return ps_connectChannel(ch) == PS_STATUS_OK;
}

bool purespice_disconnectChannel(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;

  if (!ch->available)
  {
    PS_LOG_ERROR("%s: Channel is not availble", ch->name);
    return false;
  }

  if (!ch->connected)
    return true;

  channel_disconnect(ch);
  return true;
}
