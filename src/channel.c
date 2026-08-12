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
#include "log.h"
#include "channel.h"
#include "locking.h"
#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <alloca.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>

static uint64_t get_timestamp(void)
{
  struct timespec time;
  const int result = clock_gettime(CLOCK_MONOTONIC, &time);
  if (result != 0)
    perror("clock_gettime failed! this should never happen!\n");
  return (uint64_t)time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

static bool channel_internal_disconnectNL(PSChannel * channel);

#ifdef PURESPICE_TESTING
static PSTestConnectPublishedFn testConnectPublishedHook;
static void * testConnectPublishedOpaque;

void purespice_testSetConnectPublishedHook(
    PSTestConnectPublishedFn hook, void * opaque)
{
  testConnectPublishedOpaque = opaque;
  testConnectPublishedHook   = hook;
}
#endif

static bool channel_connectCancelled(const PSChannel * channel)
{
  return atomic_load_explicit(
      &g_ps.connectCancelled, memory_order_acquire) ||
    atomic_load_explicit(
      &channel->connectCancelled, memory_order_acquire);
}

static bool channel_publishConnectSocket(PSChannel * channel, int socketFd)
{
  SPICE_LOCK(channel->connectSocketLock);
  const bool cancelled = channel_connectCancelled(channel);
  if (!cancelled)
  {
    channel->connectSocket = socketFd;
    atomic_store_explicit(&channel->socket, socketFd, memory_order_release);
  }
  SPICE_UNLOCK(channel->connectSocketLock);
  return !cancelled;
}

static void channel_closeConnectSocket(PSChannel * channel, int socketFd)
{
  SPICE_LOCK(channel->connectSocketLock);
  if (channel->connectSocket == socketFd)
    channel->connectSocket = -1;
  atomic_store_explicit(&channel->socket, -1, memory_order_release);
  close(socketFd);
  SPICE_UNLOCK(channel->connectSocketLock);
}

static bool channel_finishConnectSocket(PSChannel * channel, int socketFd)
{
  SPICE_LOCK(channel->connectSocketLock);
  const bool cancelled = channel_connectCancelled(channel);
  if (!cancelled && channel->connectSocket == socketFd)
    channel->connectSocket = -1;
  SPICE_UNLOCK(channel->connectSocketLock);
  return !cancelled;
}

static void channel_failConnect(PSChannel * channel)
{
  const int socketFd = atomic_load_explicit(
      &channel->socket, memory_order_acquire);
  SPICE_LOCK(channel->connectSocketLock);
  if (channel->connectSocket == socketFd)
    channel->connectSocket = -1;
  SPICE_UNLOCK(channel->connectSocketLock);
  channel_internal_disconnectNL(channel);
}

#define CHANNEL_CONNECT_FAIL(channel) channel_failConnect(channel)

static bool channel_connectSocket(const PSChannel * channel, int socketFd,
    const struct sockaddr * address, socklen_t addressSize)
{
  enum { CANCEL_POLL_MS = 25 };

  const int flags = fcntl(socketFd, F_GETFL, 0);
  if (flags < 0 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) != 0)
    return false;

  int result;
  do
    result = connect(socketFd, address, addressSize);
  while(result != 0 && errno == EINTR &&
      !channel_connectCancelled(channel));

  if (result != 0 && errno != EINPROGRESS && errno != EALREADY)
    return false;

  while(result != 0)
  {
    if (channel_connectCancelled(channel))
      return false;

    struct pollfd pollFd =
    {
      .fd     = socketFd,
      .events = POLLOUT,
    };
    do
      result = poll(&pollFd, 1, CANCEL_POLL_MS);
    while(result < 0 && errno == EINTR &&
        !channel_connectCancelled(channel));
    if (result < 0 || channel_connectCancelled(channel))
      return false;
    if (result == 0)
      continue;

    int error = 0;
    socklen_t errorSize = sizeof(error);
    if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR,
          &error, &errorSize) != 0 || error != 0)
      return false;
    break;
  }

  return !channel_connectCancelled(channel) &&
    fcntl(socketFd, F_SETFL, flags) == 0;
}

static void channel_writeErrorNL(PSChannel * channel)
{
  const int socket =
    atomic_load_explicit(&channel->socket, memory_order_acquire);
  if (socket >= 0)
    shutdown(socket, SHUT_RDWR);
  atomic_store_explicit(
      &channel->doDisconnect, true, memory_order_release);
}

static bool channel_setWriteBlockedNL(PSChannel * channel, bool blocked)
{
  if (atomic_load_explicit(
        &channel->writeBlocked, memory_order_acquire) == blocked)
    return true;

  if (atomic_load_explicit(&channel->ready, memory_order_acquire))
  {
    const int socket =
      atomic_load_explicit(&channel->socket, memory_order_acquire);
    struct epoll_event event =
    {
      .events   = EPOLLIN | (blocked ? EPOLLOUT : 0),
      .data.ptr = channel
    };
    if (socket < 0 ||
        epoll_ctl(g_ps.epollfd, EPOLL_CTL_MOD, socket, &event) != 0)
    {
      channel_writeErrorNL(channel);
      return false;
    }
  }

  atomic_store_explicit(
      &channel->writeBlocked, blocked, memory_order_release);
  return true;
}

static bool channel_appendWriteNL(PSChannel * channel,
    const void * buffer, size_t size)
{
  if (!size)
    return true;

  const size_t remaining =
    channel->pendingWriteSize - channel->pendingWriteOffset;
  if (size > SIZE_MAX - remaining)
  {
    channel_writeErrorNL(channel);
    return false;
  }

  const size_t needed = remaining + size;
  if (needed > SPICE_MAX_PENDING_WRITE_SIZE)
  {
    PS_LOG_ERROR("%s: pending output exceeded %zu bytes",
        channel->name, (size_t)SPICE_MAX_PENDING_WRITE_SIZE);
    channel_writeErrorNL(channel);
    return false;
  }

  uint8_t * pending = channel->pendingWrite;
  if (channel->pendingWriteOffset && remaining)
    memmove(pending, pending + channel->pendingWriteOffset, remaining);

  if (channel->pendingWriteCapacity < needed)
  {
    pending = realloc(pending, needed);
    if (pending)
      channel->pendingWriteCapacity = needed;
  }
  if (!pending)
  {
    channel_writeErrorNL(channel);
    return false;
  }

  memcpy(pending + remaining, buffer, size);
  channel->pendingWrite       = pending;
  channel->pendingWriteOffset = 0;
  channel->pendingWriteSize   = needed;
  return channel_setWriteBlockedNL(channel, true);
}

static bool channel_flushWriteNL(PSChannel * channel)
{
  if (channel->pendingWriteOffset == channel->pendingWriteSize)
    return channel_setWriteBlockedNL(channel, false);

  const int socket =
    atomic_load_explicit(&channel->socket, memory_order_acquire);
  ssize_t result;
  do
  {
    result = send(socket,
        channel->pendingWrite + channel->pendingWriteOffset,
        channel->pendingWriteSize - channel->pendingWriteOffset,
        MSG_NOSIGNAL | MSG_DONTWAIT);
  }
  while(result < 0 && errno == EINTR);

  if (result < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return true;

    channel_writeErrorNL(channel);
    return false;
  }

  if (result == 0)
  {
    channel_writeErrorNL(channel);
    return false;
  }

  channel->pendingWriteOffset += (size_t)result;
  if (channel->pendingWriteOffset < channel->pendingWriteSize)
    return true;

  channel->pendingWriteOffset = 0;
  channel->pendingWriteSize   = 0;
  return channel_setWriteBlockedNL(channel, false);
}

static bool channel_storeWriteRemainderNL(PSChannel * channel,
    const SpiceMiniDataHeader * header, const void * data, size_t size,
    size_t sent)
{
  const size_t headerSize = sizeof(*header);
  uint8_t stackHeader[sizeof(*header)];
  if (sent < headerSize)
  {
    const size_t headerRemaining = headerSize - sent;
    memcpy(stackHeader, (const uint8_t *)header + sent, headerRemaining);
    if (!channel_appendWriteNL(channel, stackHeader, headerRemaining))
      return false;
    sent = headerSize;
  }

  const size_t dataOffset = sent - headerSize;
  return dataOffset >= size || channel_appendWriteNL(channel,
      (const uint8_t *)data + dataOffset, size - dataOffset);
}

static bool channel_sendConnectPacketNL(PSChannel * channel)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint8_t         caps[];
  }
  __attribute__((packed)) ConnectPacket;

  const size_t packetSize = sizeof(ConnectPacket) +
    COMMON_CAPS_BYTES + channel->clientCapsBytes;
  ConnectPacket * packet = alloca(packetSize);

  memset(packet, 0, packetSize);
  packet->header.magic         = SPICE_MAGIC;
  packet->header.major_version = SPICE_VERSION_MAJOR;
  packet->header.minor_version = SPICE_VERSION_MINOR;
  packet->header.size          = packetSize - sizeof(packet->header);

  packet->message.connection_id    = g_ps.sessionID;
  packet->message.channel_type     = channel->spiceType;
  packet->message.channel_id       = channel->id;
  packet->message.num_common_caps  =
    COMMON_CAPS_BYTES / sizeof(uint32_t);
  packet->message.num_channel_caps =
    channel->clientCapsBytes / sizeof(uint32_t);
  packet->message.caps_offset      = sizeof(packet->message);

  uint32_t commonCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)] = { 0 };
  COMMON_SET_CAPABILITY(commonCaps,
      SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(commonCaps, SPICE_COMMON_CAP_AUTH_SPICE);
  COMMON_SET_CAPABILITY(commonCaps, SPICE_COMMON_CAP_MINI_HEADER);
  memcpy(packet->caps, commonCaps, sizeof(commonCaps));

  if (channel->clientCapsBytes)
  {
    uint32_t * channelCaps = alloca(channel->clientCapsBytes);
    memset(channelCaps, 0, channel->clientCapsBytes);
    if (channel->setClientCaps)
      channel->setClientCaps(channelCaps);

    memcpy(packet->caps + sizeof(commonCaps),
        channelCaps, channel->clientCapsBytes);
  }

  return (size_t)channel_writeNL(channel, packet, packetSize) == packetSize;
}

static PS_STATUS channel_connectNL(PSChannel * channel)
{
  PS_STATUS status;

  if (channel_connectCancelled(channel))
    return PS_STATUS_ERROR;

  channel->doDisconnect = false;
  channel->writeBlocked = false;
  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;
  channel->discarding   = false;
  channel->discardSize  = 0;

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

  const int socketFd = socket(g_ps.family, SOCK_STREAM, 0);
  if (socketFd == -1)
  {
    PS_LOG_ERROR("Socket creation failed");
    return PS_STATUS_ERROR;
  }

  if (!channel_publishConnectSocket(channel, socketFd))
  {
    close(socketFd);
    return PS_STATUS_ERROR;
  }

#ifdef PURESPICE_TESTING
  if (testConnectPublishedHook)
    testConnectPublishedHook(testConnectPublishedOpaque);
#endif

  if (g_ps.family != AF_UNIX)
  {
    const int flag = 1;
    (void)setsockopt(socketFd, IPPROTO_TCP,
        TCP_NODELAY , &flag, sizeof(int));
    (void)setsockopt(socketFd, IPPROTO_TCP,
        TCP_QUICKACK, &flag, sizeof(int));
  }

  if (!channel_connectSocket(
        channel, socketFd, &g_ps.addr.addr, (socklen_t)addrSize))
  {
    channel_closeConnectSocket(channel, socketFd);
    PS_LOG_ERROR("Socket connect failed");
    return PS_STATUS_ERROR;
  }

  if (channel_connectCancelled(channel))
  {
    shutdown(socketFd, SHUT_RDWR);
    channel_closeConnectSocket(channel, socketFd);
    return PS_STATUS_ERROR;
  }

  channel->connected = true;

  if (!channel_sendConnectPacketNL(channel))
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to write the connect packet");
    return PS_STATUS_ERROR;
  }

  SpiceLinkHeader header;
  if ((status = channel_readNL(channel, &header, sizeof(header),
          NULL)) != PS_STATUS_OK)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to read the reply to the connect packet");
    return status;
  }

  if (header.magic         != SPICE_MAGIC ||
      header.major_version != SPICE_VERSION_MAJOR)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Invalid spice magic and or version");
    return PS_STATUS_ERROR;
  }

  if (header.size < sizeof(SpiceLinkReply))
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("First message < sizeof(SpiceLinkReply)");
    return PS_STATUS_ERROR;
  }

  // in practice I have not seen this exceed 186, but it might depending on
  // future protocol changes, so put a reaonable upper bound on it
  if (header.size > 200)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("SpiceLinkReply header size seems too large");
    return PS_STATUS_ERROR;
  }

  SpiceLinkReply * reply = alloca(header.size);
  if ((status = channel_readNL(channel, reply, header.size,
          NULL)) != PS_STATUS_OK)
  {
    CHANNEL_CONNECT_FAIL(channel);
    return status;
  }

  if (reply->error != SPICE_LINK_ERR_OK)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Server reported link error: %d", reply->error);
    return PS_STATUS_ERROR;
  }

  const size_t capsOffset = reply->caps_offset;
  if (capsOffset < sizeof(*reply) || capsOffset > header.size)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("SpiceLinkReply capability offset is outside the reply");
    return PS_STATUS_ERROR;
  }

  const size_t capWordsAvailable =
    (header.size - capsOffset) / sizeof(uint32_t);
  const size_t numCommon  = reply->num_common_caps;
  const size_t numChannel = reply->num_channel_caps;

  if (numCommon > capWordsAvailable ||
      numChannel > capWordsAvailable - numCommon)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("SpiceLinkReply capabilities exceed the reply");
    return PS_STATUS_ERROR;
  }

  if (channel->setCaps)
  {
    const size_t capWords = numCommon + numChannel;
    uint32_t * caps = NULL;
    if (capWords)
    {
      caps = alloca(capWords * sizeof(*caps));
      memcpy(caps, (const uint8_t *)reply + capsOffset,
          capWords * sizeof(*caps));
    }

    channel->setCaps(
      caps                           , (int)numCommon,
      caps ? caps + numCommon : NULL, (int)numChannel);
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (channel_writeNL(channel, &auth, sizeof(auth)) != sizeof(auth))
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to write the auth mechanisim packet");
    return PS_STATUS_ERROR;
  }

  PSPassword pass;
  if (!rsa_encryptPassword(reply->pub_key, g_ps.config.password, &pass))
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to encrypt the password");
    return PS_STATUS_ERROR;
  }

  if (channel_writeNL(channel, pass.data, pass.size) != pass.size)
  {
    rsa_freePassword(&pass);
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to write the encrypted password");
    return PS_STATUS_ERROR;
  }

  rsa_freePassword(&pass);

  uint32_t linkResult;
  if ((status = channel_readNL(channel, &linkResult, sizeof(linkResult),
          NULL)) != PS_STATUS_OK)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to read the authentication response");
    return status;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Server reported link error: %u", linkResult);
    return PS_STATUS_ERROR;
  }

  channel->readAhead = malloc(SPICE_READ_AHEAD_SIZE);
  if (!channel->readAhead)
  {
    CHANNEL_CONNECT_FAIL(channel);
    PS_LOG_ERROR("Failed to allocate the channel read-ahead buffer");
    return PS_STATUS_ERROR;
  }

  channel->readAheadOffset = 0;
  channel->readAheadSize   = 0;

  struct epoll_event ev =
  {
    .events   = EPOLLIN,
    .data.ptr = channel
  };
  epoll_ctl(g_ps.epollfd, EPOLL_CTL_ADD, channel->socket, &ev);

  if (!channel_finishConnectSocket(channel, socketFd))
  {
    CHANNEL_CONNECT_FAIL(channel);
    return PS_STATUS_ERROR;
  }
  channel->ready = true;
  return PS_STATUS_OK;
}

#undef CHANNEL_CONNECT_FAIL

PS_STATUS channel_connect(PSChannel * channel)
{
  SPICE_LOCK(channel->lock);
  const PS_STATUS status = channel_connectNL(channel);
  SPICE_UNLOCK(channel->lock);
  return status;
}

void channel_cancelConnect(PSChannel * channel)
{
  SPICE_LOCK(channel->connectSocketLock);
  if (channel->connectSocket >= 0)
    shutdown(channel->connectSocket, SHUT_RDWR);
  SPICE_UNLOCK(channel->connectSocketLock);
}

static bool channel_internal_disconnectNL(PSChannel * channel)
{
  if (!channel->connected)
    return false;

  bool blocked = atomic_load_explicit(
      &channel->writeBlocked, memory_order_acquire);
  if (channel->ready)
  {
    channel->ready = false;

    /* disable nodelay so we can trigger a flush after this message */
    int flag;
    if (g_ps.family != AF_UNIX)
    {
      flag = 0;
      (void)setsockopt(channel->socket, IPPROTO_TCP,
          TCP_NODELAY, (char *)&flag, sizeof(int));
    }

    SpiceMsgcDisconnecting * packet = SPICE_PACKET(SPICE_MSGC_DISCONNECTING,
        SpiceMsgcDisconnecting, 0);
    packet->time_stamp = get_timestamp();
    packet->reason     = SPICE_LINK_ERR_OK;
    if (!blocked)
    {
      SpiceMiniDataHeader * header =
        (SpiceMiniDataHeader *)((uint8_t *)packet - sizeof(*header));
      ssize_t * size =
        (ssize_t *)((uint8_t *)header - sizeof(*size));
      const ssize_t sent = send(channel->socket, header, *size,
          MSG_NOSIGNAL | MSG_DONTWAIT);
      blocked = sent != *size;
    }

    /* re-enable nodelay as this triggers a flush according to the man page */
    if (g_ps.family != AF_UNIX)
    {
      flag = 1;
      (void)setsockopt(channel->socket, IPPROTO_TCP,
          TCP_NODELAY, (char *)&flag, sizeof(int));
    }
  }

  epoll_ctl(g_ps.epollfd, EPOLL_CTL_DEL, channel->socket, NULL);
  shutdown(channel->socket, blocked ? SHUT_RDWR : SHUT_WR);
  const int socketFd = atomic_exchange_explicit(
      &channel->socket, -1, memory_order_acq_rel);
  close(socketFd);

  channel->bufferRead = 0;
  channel->headerRead = 0;
  channel->bufferSize = 0;
  channel->readAheadOffset = 0;
  channel->readAheadSize   = 0;
  channel->discarding = false;
  channel->discardSize = 0;
  free(channel->buffer);
  channel->buffer = NULL;
  free(channel->readAhead);
  channel->readAhead = NULL;
  free(channel->pendingWrite);
  channel->pendingWrite         = NULL;
  channel->pendingWriteOffset   = 0;
  channel->pendingWriteSize     = 0;
  channel->pendingWriteCapacity = 0;
  channel->writeBlocked         = false;
  channel->connected            = false;
  channel->doDisconnect         = false;

  PS_LOG_INFO("%s channel disconnected", channel->name);
  return true;
}

void channel_internal_disconnect(PSChannel * channel)
{
  SPICE_LOCK(channel->lock);
  const bool disconnected = channel_internal_disconnectNL(channel);
  SPICE_UNLOCK(channel->lock);

  if (disconnected && channel->onDisconnect)
    channel->onDisconnect(channel);
}

void channel_disconnectPending(PSChannel * channel)
{
  SPICE_LOCK(channel->lock);
  bool disconnected = false;
  if (channel->doDisconnect)
    disconnected = channel_internal_disconnectNL(channel);
  SPICE_UNLOCK(channel->lock);

  if (disconnected && channel->onDisconnect)
    channel->onDisconnect(channel);
}

bool channel_cancelDisconnect(PSChannel * channel)
{
  SPICE_LOCK(channel->lock);
  const bool connected = channel->connected;
  if (connected)
    channel->doDisconnect = false;
  SPICE_UNLOCK(channel->lock);
  return connected;
}

void channel_disconnect(PSChannel * channel)
{
  if (!channel->connected)
    return;

  channel->doDisconnect = true;
}

bool channel_validatePayload(const PSChannel * channel, size_t minimum,
    const char * message)
{
  if (channel->header.size >= minimum)
    return true;

  PS_LOG_ERROR("%s: %s payload is too small (%u bytes, expected at least %zu)",
      channel->name, message, channel->header.size, minimum);
  return false;
}

bool channel_validateRange(const PSChannel * channel, size_t offset,
    size_t length, const char * field)
{
  if (offset <= channel->header.size &&
      length <= channel->header.size - offset)
    return true;

  PS_LOG_ERROR("%s: %s range is outside the message "
      "(offset: %zu, length: %zu, payload: %u)",
      channel->name, field, offset, length, channel->header.size);
  return false;
}

static PS_STATUS onMessage_setAck(PSChannel * channel)
{
  if (!channel_validatePayload(channel, sizeof(SpiceMsgSetAck), "SET_ACK"))
    return PS_STATUS_ERROR;

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
  if (!channel_validatePayload(channel, sizeof(SpiceMsgPing), "PING"))
    return PS_STATUS_ERROR;

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
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgcDisconnecting), "DISCONNECTING"))
    return PS_STATUS_ERROR;

  shutdown(channel->socket, SHUT_WR);
  PS_LOG_INFO("Server sent disconnect message");
  return PS_STATUS_HANDLED;
}

static PS_STATUS onMessage_notify(PSChannel * channel)
{
  if (!channel_validatePayload(channel, sizeof(SpiceMsgNotify), "NOTIFY"))
    return PS_STATUS_ERROR;

  SpiceMsgNotify * msg = (SpiceMsgNotify *)channel->buffer;
  const size_t available = channel->header.size - sizeof(*msg);
  if (msg->message_len >= available ||
      msg->message[msg->message_len] != '\0')
  {
    PS_LOG_ERROR("%s: NOTIFY message string is not contained in the payload",
        channel->name);
    return PS_STATUS_ERROR;
  }

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

  void * ack = SPICE_RAW_PACKET(SPICE_MSGC_ACK, 0, 0);
  if (!SPICE_SEND_PACKET(channel, ack))
  {
    PS_LOG_ERROR("Failed to write ack packet");
    return false;
  }

  return true;
}

ssize_t channel_writeNL(PSChannel * channel,
    const void * buffer, size_t size)
{
  if (!channel->connected)
    return -1;

  if (!buffer)
    return -1;

  if (channel->ready && atomic_load_explicit(
        &channel->writeBlocked, memory_order_acquire))
    return channel_appendWriteNL(channel, buffer, size) ? (ssize_t)size : -1;

  const bool asyncWrite = channel->nonblockingWrite && channel->ready;
  ssize_t result;
  do
    result = send(channel->socket, buffer, size, MSG_NOSIGNAL |
        (asyncWrite ? MSG_DONTWAIT : 0));
  while(result < 0 && errno == EINTR);

  if (asyncWrite)
  {
    if (result < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return channel_appendWriteNL(
            channel, buffer, size) ? (ssize_t)size : -1;

      channel_writeErrorNL(channel);
      return -1;
    }

    if ((size_t)result < size)
      return channel_appendWriteNL(channel,
          (const uint8_t *)buffer + result,
          size - (size_t)result) ? (ssize_t)size : -1;

    return result;
  }

  if (result < 0 || (size_t)result != size)
  {
    /*
     * Retrying a partial send would allow a concurrent sender to interleave
     * another message before the remainder and corrupt the stream framing.
     * Fail the channel instead.
     */
    shutdown(channel->socket, SHUT_RDWR);
    channel->doDisconnect = true;
  }

  return result;
}

bool channel_sendMessagePartsNL(PSChannel * channel, uint16_t type,
    const void * prefix, size_t prefixSize,
    const void * data, size_t dataSize, bool nonblocking)
{
  if ((!prefix && prefixSize) || (!data && dataSize) ||
      prefixSize > UINT32_MAX ||
      dataSize > UINT32_MAX - prefixSize)
    return false;

  const size_t size = prefixSize + dataSize;
  if (size > (size_t)SSIZE_MAX - sizeof(SpiceMiniDataHeader))
    return false;

  if (!atomic_load_explicit(&channel->connected, memory_order_acquire) ||
      !atomic_load_explicit(&channel->ready, memory_order_acquire))
    return false;

  const SpiceMiniDataHeader header =
  {
    .type = type,
    .size = (uint32_t)size
  };

  if (atomic_load_explicit(
        &channel->writeBlocked, memory_order_acquire))
  {
    if (nonblocking)
      return false;

    const size_t packetSize = sizeof(header) + size;
    uint8_t * packet = malloc(packetSize);
    if (!packet)
    {
      channel_writeErrorNL(channel);
      return false;
    }

    memcpy(packet, &header, sizeof(header));
    size_t offset = sizeof(header);
    if (prefixSize)
    {
      memcpy(packet + offset, prefix, prefixSize);
      offset += prefixSize;
    }
    if (dataSize)
      memcpy(packet + offset, data, dataSize);

    const bool result = channel_appendWriteNL(
        channel, packet, packetSize);
    free(packet);
    return result;
  }

  struct iovec iov[3] =
  {
    {
      .iov_base = (void *)&header,
      .iov_len  = sizeof(header)
    }
  };

  size_t iovCount = 1;
  if (prefixSize)
  {
    iov[iovCount++] = (struct iovec)
    {
      .iov_base = (void *)prefix,
      .iov_len  = prefixSize
    };
  }
  if (dataSize)
  {
    iov[iovCount++] = (struct iovec)
    {
      .iov_base = (void *)data,
      .iov_len  = dataSize
    };
  }

  const struct msghdr message =
  {
    .msg_iov    = iov,
    .msg_iovlen = iovCount
  };

  const int socket =
    atomic_load_explicit(&channel->socket, memory_order_acquire);
  const int flags = MSG_NOSIGNAL |
    (nonblocking ? MSG_DONTWAIT : 0);
  const ssize_t result = sendmsg(socket, &message, flags);
  const size_t expected = sizeof(header) + size;
  const bool success = result >= 0 && (size_t)result == expected;
  if (!success)
  {
    if (nonblocking && result < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK))
      return false;

    shutdown(socket, SHUT_RDWR);
    atomic_store_explicit(
        &channel->doDisconnect, true, memory_order_release);
  }

  return success;
}

bool channel_sendMessage(PSChannel * channel, uint16_t type,
    const void * buffer, size_t size)
{
  return channel_sendMessagePartsNL(
      channel, type, buffer, size, NULL, 0, false);
}

ssize_t channel_sendMessageNonblocking(PSChannel * channel, uint16_t type,
    const void * buffer, size_t size)
{
  if ((!buffer && size) || size > UINT32_MAX ||
      size > (size_t)SSIZE_MAX - sizeof(SpiceMiniDataHeader))
    return -1;

  SPICE_LOCK(channel->lock);
  if (!atomic_load_explicit(&channel->connected, memory_order_acquire) ||
      !atomic_load_explicit(&channel->ready, memory_order_acquire))
  {
    SPICE_UNLOCK(channel->lock);
    return -1;
  }

  if (channel->pendingWriteOffset < channel->pendingWriteSize &&
      !channel_flushWriteNL(channel))
  {
    SPICE_UNLOCK(channel->lock);
    return -1;
  }
  if (channel->pendingWriteOffset < channel->pendingWriteSize)
  {
    SPICE_UNLOCK(channel->lock);
    return 0;
  }

  const SpiceMiniDataHeader header =
  {
    .type = type,
    .size = (uint32_t)size
  };
  struct iovec iov[2] =
  {
    {
      .iov_base = (void *)&header,
      .iov_len  = sizeof(header)
    },
    {
      .iov_base = (void *)buffer,
      .iov_len  = size
    }
  };
  const struct msghdr message =
  {
    .msg_iov    = iov,
    .msg_iovlen = size ? 2 : 1
  };

  const int socket =
    atomic_load_explicit(&channel->socket, memory_order_acquire);
  ssize_t result;
  do
    result = sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
  while(result < 0 && errno == EINTR);

  if (result < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      const size_t packetSize = sizeof(header) + size;
      uint8_t * packet = malloc(packetSize);
      if (!packet)
      {
        channel_writeErrorNL(channel);
        SPICE_UNLOCK(channel->lock);
        return -1;
      }

      memcpy(packet, &header, sizeof(header));
      if (size)
        memcpy(packet + sizeof(header), buffer, size);
      const bool blocked = channel_appendWriteNL(
          channel, packet, packetSize);
      free(packet);
      SPICE_UNLOCK(channel->lock);
      return blocked ? (ssize_t)size : -1;
    }

    channel_writeErrorNL(channel);
    SPICE_UNLOCK(channel->lock);
    return -1;
  }

  if (result == 0)
  {
    const size_t packetSize = sizeof(header) + size;
    uint8_t * packet = malloc(packetSize);
    if (!packet)
    {
      channel_writeErrorNL(channel);
      SPICE_UNLOCK(channel->lock);
      return -1;
    }

    memcpy(packet, &header, sizeof(header));
    if (size)
      memcpy(packet + sizeof(header), buffer, size);
    const bool blocked = channel_appendWriteNL(
        channel, packet, packetSize);
    free(packet);
    SPICE_UNLOCK(channel->lock);
    return blocked ? (ssize_t)size : -1;
  }

  const size_t expected = sizeof(header) + size;
  if ((size_t)result < expected &&
      !channel_storeWriteRemainderNL(
        channel, &header, buffer, size, (size_t)result))
  {
    SPICE_UNLOCK(channel->lock);
    return -1;
  }
  else if ((size_t)result == expected &&
      !channel_setWriteBlockedNL(channel, false))
    atomic_store_explicit(
        &channel->doDisconnect, true, memory_order_release);
  else if ((size_t)result > expected)
  {
    channel_writeErrorNL(channel);
    SPICE_UNLOCK(channel->lock);
    return -1;
  }

  SPICE_UNLOCK(channel->lock);
  return (ssize_t)size;
}

bool channel_flushWrite(PSChannel * channel)
{
  SPICE_LOCK(channel->lock);
  const bool result = channel_flushWriteNL(channel);
  SPICE_UNLOCK(channel->lock);
  return result;
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
