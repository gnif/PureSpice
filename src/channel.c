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
#include "log.h"
#include "channel.h"
#include "locking.h"
#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <alloca.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <netinet/tcp.h>

static uint64_t get_timestamp()
{
  struct timespec time;
  const int result = clock_gettime(CLOCK_MONOTONIC, &time);
  if (result != 0)
    perror("clock_gettime failed! this should never happen!\n");
  return (uint64_t)time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

PS_STATUS channel_connect(struct PSChannel * channel)
{
  PS_STATUS status;

  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;

  if (channel == &g_ps.scInputs)
    SPICE_LOCK_INIT(g_ps.mouse.lock);

  SPICE_LOCK_INIT(channel->lock);

  size_t addrSize;
  switch(g_ps.family)
  {
    case AF_UNIX:
      addrSize = sizeof(g_ps.addr.un);
      break;

    case AF_INET:
      addrSize = sizeof(g_ps.addr.in);
      break;

    case AF_INET6:
      addrSize = sizeof(g_ps.addr.in6);
      break;

    default:
      PS_LOG_ERROR("BUG: invalid address family");
      return PS_STATUS_ERROR;
  }

  channel->socket = socket(g_ps.family, SOCK_STREAM, 0);
  if (channel->socket == -1)
  {
    PS_LOG_ERROR("Socket creation failed");
    return PS_STATUS_ERROR;
  }

  if (g_ps.family != AF_UNIX)
  {
    const int flag = 1;
    setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY , &flag, sizeof(int));
    setsockopt(channel->socket, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(int));
  }

  if (connect(channel->socket, &g_ps.addr.addr, addrSize) == -1)
  {
    close(channel->socket);
    PS_LOG_ERROR("Socket connect failed");
    return PS_STATUS_ERROR;
  }

  channel->connected = true;

  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[MAIN_CAPS_BYTES   / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .connection_id    = g_ps.sessionID,
      .channel_type     = channel->channelType,
      .channel_id       = g_ps.channelID,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = MAIN_CAPS_BYTES   / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (channel == &g_ps.scMain)
    MAIN_SET_CAPABILITY(p.channelCaps, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);

  if (channel == &g_ps.scPlayback)
    PLAYBACK_SET_CAPABILITY(p.channelCaps, SPICE_PLAYBACK_CAP_VOLUME);

  if (channel_writeNL(channel, &p, sizeof(p)) != sizeof(p))
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to write the connect packet");
    return PS_STATUS_ERROR;
  }

  if ((status = channel_readNL(channel, &p.header, sizeof(p.header),
          NULL)) != PS_STATUS_OK)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to read the reply to the connect packet");
    return status;
  }

  if (p.header.magic         != SPICE_MAGIC ||
      p.header.major_version != SPICE_VERSION_MAJOR)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Invalid spice magic and or version");
    return PS_STATUS_ERROR;
  }

  if (p.header.size < sizeof(SpiceLinkReply))
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("First message < sizeof(SpiceLinkReply)");
    return PS_STATUS_ERROR;
  }

  SpiceLinkReply reply;
  if ((status = channel_readNL(channel, &reply, sizeof(reply),
          NULL)) != PS_STATUS_OK)
  {
    channel_disconnect(channel);
    return status;
  }

  if (reply.error != SPICE_LINK_ERR_OK)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Server reported link error: %d", reply.error);
    return PS_STATUS_ERROR;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  if ((status = channel_readNL(channel,
          &capsCommon , sizeof(capsCommon ), NULL)) != PS_STATUS_OK ||
      (status = channel_readNL(channel,
          &capsChannel, sizeof(capsChannel), NULL)) != PS_STATUS_OK)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to read the channel capabillities");
    return status;
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (channel_writeNL(channel, &auth, sizeof(auth)) != sizeof(auth))
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to write the auth mechanisim packet");
    return PS_STATUS_ERROR;
  }

  PSPassword pass;
  if (!purespice_rsaEncryptPassword(reply.pub_key, g_ps.config.password, &pass))
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to encrypt the password");
    return PS_STATUS_ERROR;
  }

  if (channel_writeNL(channel, pass.data, pass.size) != pass.size)
  {
    purespice_rsaFreePassword(&pass);
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to write the encrypted password");
    return PS_STATUS_ERROR;
  }

  purespice_rsaFreePassword(&pass);

  uint32_t linkResult;
  if ((status = channel_readNL(channel, &linkResult, sizeof(linkResult),
          NULL)) != PS_STATUS_OK)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Failed to read the authentication response");
    return status;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    channel_disconnect(channel);
    PS_LOG_ERROR("Server reported link error: %u", linkResult);
    return PS_STATUS_ERROR;
  }

  struct epoll_event ev =
  {
    .events   = EPOLLIN,
    .data.ptr = channel
  };
  epoll_ctl(g_ps.epollfd, EPOLL_CTL_ADD, channel->socket, &ev);

  channel->ready = true;
  return PS_STATUS_OK;
}

void channel_disconnect(struct PSChannel * channel)
{
  if (!channel->connected)
    return;

  if (channel->ready)
  {
    /* disable nodelay so we can trigger a flush after this message */
    int flag;
    if (g_ps.family != AF_UNIX)
    {
      flag = 0;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY,
          (char *)&flag, sizeof(int));
    }

    SpiceMsgcDisconnecting * packet = SPICE_PACKET(SPICE_MSGC_DISCONNECTING,
        SpiceMsgcDisconnecting, 0);
    packet->time_stamp = get_timestamp();
    packet->reason     = SPICE_LINK_ERR_OK;
    SPICE_SEND_PACKET(channel, packet);

    /* re-enable nodelay as this triggers a flush according to the man page */
    if (g_ps.family != AF_UNIX)
    {
      flag = 1;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY,
          (char *)&flag, sizeof(int));
    }
  }

  epoll_ctl(g_ps.epollfd, EPOLL_CTL_DEL, channel->socket, NULL);
  shutdown(channel->socket, SHUT_WR);
}

PS_STATUS channel_onRead(struct PSChannel * channel, SpiceMiniDataHeader * header,
    int * dataAvailable)
{
  PS_STATUS status;
  if ((status = channel_readNL(channel, header, sizeof(SpiceMiniDataHeader),
          dataAvailable)) != PS_STATUS_OK)
    return status;

  if (!channel->connected)
    return PS_STATUS_HANDLED;

  if (!channel->initDone)
    return PS_STATUS_OK;

  switch(header->type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
      return PS_STATUS_HANDLED;

    case SPICE_MSG_SET_ACK:
    {
      SpiceMsgSetAck in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgSetAck");
        return status;
      }

      channel->ackFrequency = in.window;

      SpiceMsgcAckSync * out =
        SPICE_PACKET(SPICE_MSGC_ACK_SYNC, SpiceMsgcAckSync, 0);

      out->generation = in.generation;
      return SPICE_SEND_PACKET(channel, out) ?
        PS_STATUS_HANDLED : PS_STATUS_ERROR;
    }

    case SPICE_MSG_PING:
    {
      SpiceMsgPing in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgPing");
        return status;
      }

      const int discard = header->size - sizeof(in);
      if ((status = channel_discardNL(channel, discard,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to discard the ping data");
        return status;
      }

      SpiceMsgcPong * out =
        SPICE_PACKET(SPICE_MSGC_PONG, SpiceMsgcPong, 0);

      out->id        = in.id;
      out->timestamp = in.timestamp;
      if (!SPICE_SEND_PACKET(channel, out))
      {
        PS_LOG_ERROR("Failed to send SpiceMsgcPong");
        return PS_STATUS_ERROR;
      }

      return PS_STATUS_HANDLED;
    }

    case SPICE_MSG_WAIT_FOR_CHANNELS:
      return PS_STATUS_HANDLED;

    case SPICE_MSG_DISCONNECTING:
    {
      shutdown(channel->socket, SHUT_WR);
      PS_LOG_INFO("Server sent disconnect message");
      return PS_STATUS_HANDLED;
    }

    case SPICE_MSG_NOTIFY:
    {
      SpiceMsgNotify * in = (SpiceMsgNotify *)alloca(header->size);
      if ((status = channel_readNL(channel, in, header->size,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgNotify");
        return status;
      }

      PS_LOG_INFO("[notify] %s", in->message);
      return PS_STATUS_HANDLED;
    }
  }

  return PS_STATUS_OK;
}

bool channel_ack(struct PSChannel * channel)
{
  if (channel->ackFrequency == 0)
    return true;

  if (channel->ackCount++ != channel->ackFrequency)
    return true;

  channel->ackCount = 0;

  char * ack = SPICE_PACKET(SPICE_MSGC_ACK, char, 0);
  *ack = 0;
  if (!SPICE_SEND_PACKET(channel, ack))
  {
    PS_LOG_ERROR("Failed to write ack packet");
    return false;
  }

  return true;
}

ssize_t channel_writeNL(const struct PSChannel * channel,
    const void * buffer, const ssize_t size)
{
  if (!channel->connected)
    return -1;

  if (!buffer)
    return -1;

  return send(channel->socket, buffer, size, 0);
}

PS_STATUS channel_readNL(struct PSChannel * channel, void * buffer,
    const ssize_t size, int * dataAvailable)
{
  if (!channel->connected)
  {
    PS_LOG_ERROR("BUG: attempted to read from a closed channel");
    return PS_STATUS_ERROR;
  }

  if (!buffer)
  {
    PS_LOG_ERROR("BUG: attempted to read into a NULL buffer");
    return PS_STATUS_ERROR;
  }

  size_t    left = size;
  uint8_t * buf  = (uint8_t *)buffer;
  while(left)
  {
    ssize_t len = read(channel->socket, buf, left);
    if (len == 0)
      return PS_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      PS_LOG_ERROR("Failed to read from the socket: %ld", len);
      return PS_STATUS_ERROR;
    }
    left -= len;
    buf  += len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return PS_STATUS_OK;
}

PS_STATUS channel_discardNL(struct PSChannel * channel,
    ssize_t size, int * dataAvailable)
{
  uint8_t c[1024];
  ssize_t left = size;
  while(left)
  {
    ssize_t len = read(channel->socket, c, left > sizeof(c) ? sizeof(c) : left);
    if (len == 0)
      return PS_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      PS_LOG_ERROR("Failed to read from the socket: %ld", len);
      return PS_STATUS_ERROR;
    }

    left -= len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return PS_STATUS_OK;
}
