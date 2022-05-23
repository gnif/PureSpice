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
#include <stdlib.h>

#include <sys/epoll.h>
#include <netinet/tcp.h>

static uint64_t get_timestamp(void)
{
  struct timespec time;
  const int result = clock_gettime(CLOCK_MONOTONIC, &time);
  if (result != 0)
    perror("clock_gettime failed! this should never happen!\n");
  return (uint64_t)time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

PS_STATUS channel_connect(PSChannel * channel)
{
  PS_STATUS status;

  channel->doDisconnect = false;
  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;

  if (channel->spiceType == SPICE_CHANNEL_INPUTS)
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

  const SpiceLinkHeader * p = channel->getConnectPacket();
  if ((size_t)channel_writeNL(channel, p, p->size + sizeof(*p)) != p->size + sizeof(*p))
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to write the connect packet");
    return PS_STATUS_ERROR;
  }

  SpiceLinkHeader header;
  if ((status = channel_readNL(channel, &header, sizeof(header),
          NULL)) != PS_STATUS_OK)
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to read the reply to the connect packet");
    return status;
  }

  if (header.magic         != SPICE_MAGIC ||
      header.major_version != SPICE_VERSION_MAJOR)
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Invalid spice magic and or version");
    return PS_STATUS_ERROR;
  }

  if (header.size < sizeof(SpiceLinkReply))
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("First message < sizeof(SpiceLinkReply)");
    return PS_STATUS_ERROR;
  }

  SpiceLinkReply * reply = alloca(header.size);
  if ((status = channel_readNL(channel, reply, header.size,
          NULL)) != PS_STATUS_OK)
  {
    channel_internal_disconnect(channel);
    return status;
  }

  if (reply->error != SPICE_LINK_ERR_OK)
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Server reported link error: %d", reply->error);
    return PS_STATUS_ERROR;
  }

  const uint32_t * capsCommon =
    (uint32_t *)((uint8_t *)reply + reply->caps_offset);
  const uint32_t * capsChannel =
    capsCommon + reply->num_common_caps;

  if (channel->setCaps)
    channel->setCaps(
      capsCommon , reply->num_common_caps,
      capsChannel, reply->num_channel_caps);

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (channel_writeNL(channel, &auth, sizeof(auth)) != sizeof(auth))
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to write the auth mechanisim packet");
    return PS_STATUS_ERROR;
  }

  PSPassword pass;
  if (!rsa_encryptPassword(reply->pub_key, g_ps.config.password, &pass))
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to encrypt the password");
    return PS_STATUS_ERROR;
  }

  if (channel_writeNL(channel, pass.data, pass.size) != pass.size)
  {
    rsa_freePassword(&pass);
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to write the encrypted password");
    return PS_STATUS_ERROR;
  }

  rsa_freePassword(&pass);

  uint32_t linkResult;
  if ((status = channel_readNL(channel, &linkResult, sizeof(linkResult),
          NULL)) != PS_STATUS_OK)
  {
    channel_internal_disconnect(channel);
    PS_LOG_ERROR("Failed to read the authentication response");
    return status;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    channel_internal_disconnect(channel);
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

void channel_internal_disconnect(PSChannel * channel)
{
  if (!channel->connected)
    return;

  if (channel->ready)
  {
    channel->ready = false;

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

  channel->bufferRead = 0;
  channel->headerRead = 0;
  channel->bufferSize = 0;
  free(channel->buffer);
  channel->buffer = NULL;
  channel->connected = false;
  channel->doDisconnect = false;

  PS_LOG_INFO("%s channel disconnected", channel->name);
}

void channel_disconnect(PSChannel * channel)
{
  if (!channel->connected)
    return;

  channel->doDisconnect = true;
}

static PS_STATUS onMessage_setAck(PSChannel * channel)
{
  SpiceMsgSetAck * msg = (SpiceMsgSetAck *)channel->buffer;

  channel->ackFrequency = msg->window;

  SpiceMsgcAckSync * out =
    SPICE_PACKET(SPICE_MSGC_ACK_SYNC, SpiceMsgcAckSync, 0);

  out->generation = msg->generation;
  return SPICE_SEND_PACKET(channel, out) ?
    PS_STATUS_OK : PS_STATUS_ERROR;
}

static PS_STATUS onMessage_ping(PSChannel * channel)
{
  SpiceMsgPing * msg = (SpiceMsgPing *)channel->buffer;

  SpiceMsgcPong * out =
    SPICE_PACKET(SPICE_MSGC_PONG, SpiceMsgcPong, 0);

  out->id        = msg->id;
  out->timestamp = msg->timestamp;
  if (!SPICE_SEND_PACKET(channel, out))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcPong");
    return PS_STATUS_ERROR;
  }

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_disconnecting(PSChannel * channel)
{
  shutdown(channel->socket, SHUT_WR);
  PS_LOG_INFO("Server sent disconnect message");
  return PS_STATUS_HANDLED;
}

static PS_STATUS onMessage_notify(PSChannel * channel)
{
  SpiceMsgNotify * msg = (SpiceMsgNotify *)channel->buffer;

  PS_LOG_INFO("[notify] %s", msg->message);
  return PS_STATUS_OK;
}

PSHandlerFn channel_onMessage(PSChannel * channel)
{
  switch(channel->header.type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
      return PS_HANDLER_DISCARD;

    case SPICE_MSG_SET_ACK:
      return onMessage_setAck;

    case SPICE_MSG_PING:
      return onMessage_ping;

    case SPICE_MSG_WAIT_FOR_CHANNELS:
      return PS_HANDLER_DISCARD;

    case SPICE_MSG_DISCONNECTING:
      return onMessage_disconnecting;

    case SPICE_MSG_NOTIFY:
      return onMessage_notify;
  }

  return PS_HANDLER_ERROR;
}

bool channel_ack(PSChannel * channel)
{
  if (channel->ackFrequency == 0)
    return true;

  if (++channel->ackCount != channel->ackFrequency)
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

ssize_t channel_writeNL(const PSChannel * channel,
    const void * buffer, size_t size)
{
  if (!channel->connected)
    return -1;

  if (!buffer)
    return -1;

  return send(channel->socket, buffer, size, 0);
}

PS_STATUS channel_readNL(PSChannel * channel, void * buffer,
    size_t size, int * dataAvailable)
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

PS_STATUS channel_discardNL(PSChannel * channel,
    size_t size, int * dataAvailable)
{
  uint8_t c[1024];
  size_t left = size;
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
