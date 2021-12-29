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
  .scMain    .channelType = SPICE_CHANNEL_MAIN,
  .scMain    .read        = channelMain_onRead,
  .scInputs  .channelType = SPICE_CHANNEL_INPUTS,
  .scInputs  .read        = channelInputs_onRead,
  .scPlayback.channelType = SPICE_CHANNEL_PLAYBACK,
  .scPlayback.read        = channelPlayback_onRead
};

bool purespice_connect(const PSConfig * config)
{
  memcpy(&g_ps.config, config, sizeof(*config));
  log_init();

  g_ps.config.host = strdup(config->host);
  if (!g_ps.config.host)
  {
    PS_LOG_ERROR("Failed to malloc");
    goto err_host;
  }

  g_ps.config.password = strdup(config->password);
  if (!g_ps.config.password)
  {
    PS_LOG_ERROR("Failed to malloc");
    goto err_password;
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
    goto err_epoll;
  }

  g_ps.channelID = 0;
  if (channel_connect(&g_ps.scMain) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("channel connect failed");
    goto err_connect;
  }

  PS_LOG_INFO("Connected");
  return true;

err_connect:
  close(g_ps.epollfd);

err_epoll:
  free(g_ps.config.host);
  g_ps.config.host = NULL;

err_password:
  free(g_ps.config.password);
  g_ps.config.host = NULL;

err_host:
  return false;
}

void purespice_disconnect()
{
  channel_disconnect(&g_ps.scInputs);
  channel_disconnect(&g_ps.scMain  );
  close(g_ps.epollfd);

  if (g_ps.motionBuffer)
  {
    free(g_ps.motionBuffer);
    g_ps.motionBuffer = NULL;
  }

  if (g_ps.config.host)
  {
    free(g_ps.config.host);
    g_ps.config.host = NULL;
  }

  if (g_ps.config.password)
  {
    free(g_ps.config.password);
    g_ps.config.password = NULL;
  }

  agent_disconnect();
  PS_LOG_INFO("Disconnected");
}

bool purespice_ready()
{
  return g_ps.scMain.connected &&
         g_ps.scInputs.connected;
}

bool purespice_process(int timeout)
{
  #define MAX_EVENTS 4
  static struct epoll_event events[MAX_EVENTS];

  int nfds = epoll_wait(g_ps.epollfd, events, MAX_EVENTS, timeout);
  if (nfds == 0)
    return true;

  if (nfds < 0)
  {
    PS_LOG_ERROR("epoll_err returned %d", nfds);
    return false;
  }

  for(int i = 0; i < nfds; ++i)
  {
    struct PSChannel * channel = (struct PSChannel *)events[i].data.ptr;

    int dataAvailable;
    ioctl(channel->socket, FIONREAD, &dataAvailable);

    if (!dataAvailable)
      channel->connected = false;
    else
      while(dataAvailable > 0)
      {
        switch(channel->read(&dataAvailable))
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
            return false;
        }

        if (channel->connected && !channel_ack(channel))
        {
          PS_LOG_ERROR("Failed to send message ack");
          return false;
        }
      }
  }

  if (g_ps.scMain.connected || g_ps.scInputs.connected)
    return true;

  g_ps.sessionID = 0;

  if (g_ps.scInputs.connected)
    close(g_ps.scInputs.socket);

  if (g_ps.scMain.connected)
    close(g_ps.scMain.socket);

  PS_LOG_INFO("Shutdown");
  return false;
}
