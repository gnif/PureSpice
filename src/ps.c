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

#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <sys/ioctl.h>
#include <sys/epoll.h>

#include <spice/vd_agent.h>

// globals
struct PS g_ps =
{
  .channels =
  {
    // PS_CHANNEL_MAIN
    {
      .spiceType        = SPICE_CHANNEL_MAIN,
      .name             = "MAIN",
      .getConnectPacket = channelMain_getConnectPacket,
      .setCaps          = channelMain_setCaps,
      .read             = channelMain_onRead
    },
    // PS_CHANNEL_INPUTS
    {
      .spiceType        = SPICE_CHANNEL_INPUTS,
      .name             = "INPUTS",
      .getConnectPacket = channelInputs_getConnectPacket,
      .read             = channelInputs_onRead,
    },
    // PS_CHANNEL_PLAYBACK
    {
      .spiceType        = SPICE_CHANNEL_PLAYBACK,
      .name             = "PLAYBACK",
      .enable           = &g_ps.config.playback.enable,
      .getConnectPacket = channelPlayback_getConnectPacket,
      .read             = channelPlayback_onRead
    },
    // PS_CHANNEL_RECORD
    {
      .spiceType        = SPICE_CHANNEL_RECORD,
      .name             = "RECORD",
      .enable           = &g_ps.config.record.enable,
      .getConnectPacket = channelRecord_getConnectPacket,
      .read             = channelRecord_onRead
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
    channel_disconnect(&g_ps.channels[i]);

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

  int nfds = epoll_wait(g_ps.epollfd, events, PS_CHANNEL_MAX, timeout);
  if (nfds == 0)
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

  for(int i = 0; i < nfds; ++i)
  {
    struct PSChannel * channel = (struct PSChannel *)events[i].data.ptr;

    int dataAvailable;
    ioctl(channel->socket, FIONREAD, &dataAvailable);

    if (!dataAvailable)
    {
      channel->connected = false;
      continue;
    }

    do
    {
      switch(channel->read(channel, &dataAvailable))
      {
        case PS_STATUS_OK:
        case PS_STATUS_HANDLED:
          // if dataAvailable has gone negative then refresh it
          if (dataAvailable < 0)
            ioctl(channel->socket, FIONREAD, &dataAvailable);
          break;

        case PS_STATUS_NODATA:
          channel->connected = false;
          close(channel->socket);
          dataAvailable = 0;
          break;

        default:
          return PS_STATUS_ERR_READ;
      }

      if (channel->connected && !channel_ack(channel))
      {
        PS_LOG_ERROR("Failed to send message ack");
        return PS_STATUS_ERR_ACK;
      }
    }
    while(dataAvailable > 0);
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
