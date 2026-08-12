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

#include "ps.h"
#include "log.h"
#include "agent.h"
#include "channel.h"
#include "channel_main.h"
#include "channel_inputs.h"
#include "channel_playback.h"
#include "channel_record.h"
#include "channel_display.h"
#include "channel_cursor.h"
#include "channel_usbredir.h"

#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sched.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <spice/vd_agent.h>

static atomic_flag disconnectLock = ATOMIC_FLAG_INIT;
static atomic_flag initialConnectLock = ATOMIC_FLAG_INIT;
static PSChannel * getChannel(PSChannelType channel);

// globals
PS g_ps =
{
  .channels =
  {
    // PS_CHANNEL_MAIN
    {
      .spiceType       = SPICE_CHANNEL_MAIN,
      .name            = "MAIN",
      .clientCapsBytes = MAIN_CAPS_BYTES,
      .setClientCaps   = channelMain_setClientCaps,
      .setCaps         = channelMain_setCaps,
      .onMessage       = channelMain_onMessage
    },
    // PS_CHANNEL_INPUTS
    {
      .spiceType       = SPICE_CHANNEL_INPUTS,
      .name            = "INPUTS",
      .enable          = &g_ps.config.inputs.enable,
      .autoConnect     = &g_ps.config.inputs.autoConnect,
      .clientCapsBytes = INPUT_CAPS_BYTES,
      .onMessage       = channelInputs_onMessage
    },
    // PS_CHANNEL_PLAYBACK
    {
      .spiceType       = SPICE_CHANNEL_PLAYBACK,
      .name            = "PLAYBACK",
      .enable          = &g_ps.config.playback.enable,
      .autoConnect     = &g_ps.config.playback.autoConnect,
      .clientCapsBytes = PLAYBACK_CAPS_BYTES,
      .setClientCaps   = channelPlayback_setClientCaps,
      .onMessage       = channelPlayback_onMessage
    },
    // PS_CHANNEL_RECORD
    {
      .spiceType       = SPICE_CHANNEL_RECORD,
      .name            = "RECORD",
      .enable          = &g_ps.config.record.enable,
      .autoConnect     = &g_ps.config.record.autoConnect,
      .clientCapsBytes = RECORD_CAPS_BYTES,
      .setClientCaps   = channelRecord_setClientCaps,
      .onMessage       = channelRecord_onMessage,
    },
    // PS_CHANNEL_DISPLAY
    {
      .spiceType       = SPICE_CHANNEL_DISPLAY,
      .name            = "DISPLAY",
      .enable          = &g_ps.config.display.enable,
      .autoConnect     = &g_ps.config.display.autoConnect,
      .clientCapsBytes = DISPLAY_CAPS_BYTES,
      .setClientCaps   = channelDisplay_setClientCaps,
      .onConnect       = channelDisplay_onConnect,
      .onMessage       = channelDisplay_onMessage
    },
    // PS_CHANNEL_CURSOR
    {
      .spiceType       = SPICE_CHANNEL_CURSOR,
      .name            = "CURSOR",
      .enable          = &g_ps.config.cursor.enable,
      .autoConnect     = &g_ps.config.cursor.autoConnect,
      .clientCapsBytes = CURSOR_CAPS_BYTES,
      .onMessage       = channelCursor_onMessage
    }
  }
};

void purespice_init(const PSInit * init)
{
  if (init)
    memcpy(&g_ps.init, init, sizeof(*init));

  g_ps.epollfd = -1;
  atomic_store_explicit(
      &g_ps.connectCancelled, false, memory_order_release);
  atomic_store_explicit(
      &g_ps.channelCount, PS_CHANNEL_MAX, memory_order_release);
  for(int i = PS_CHANNEL_MAX - 1; i >= 0; --i)
  {
    g_ps.channels[i].socket = -1;
    atomic_store_explicit(
        &g_ps.channels[i].connectCancelled, false, memory_order_release);
    atomic_store_explicit(
        &g_ps.channels[i].writeBlocked, false, memory_order_release);
    SPICE_LOCK_INIT(g_ps.channels[i].lock);
    SPICE_LOCK_INIT(g_ps.channels[i].connectSocketLock);
    g_ps.channels[i].connectSocket = -1;
  }
  SPICE_LOCK_INIT(g_ps.mouse.lock);
  channelUSBRedir_init();

  log_init();
  g_ps.initialized = true;
}

void purespice_beginConnect(void)
{
  if (!g_ps.initialized)
    purespice_init(NULL);

  SPICE_LOCK(initialConnectLock);
  atomic_store_explicit(&g_ps.connectCancelled, false, memory_order_release);
  const unsigned int channelCount = atomic_load_explicit(
      &g_ps.channelCount, memory_order_acquire);
  for(unsigned int i = 0; i < channelCount; ++i)
    atomic_store_explicit(&g_ps.channels[i].connectCancelled,
        false, memory_order_release);
  SPICE_UNLOCK(initialConnectLock);
}

bool purespice_connect(const PSConfig * config)
{
  if (!g_ps.initialized)
    purespice_init(NULL);

  SPICE_LOCK(initialConnectLock);
  channelUSBRedir_sessionStart();
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

  if (g_ps.config.usbRedir.enable)
  {
    if (!g_ps.config.usbRedir.state)
    {
      PS_LOG_ERROR("usbRedir->state is mandatory");
      goto err_config;
    }

    if (!g_ps.config.usbRedir.data)
    {
      PS_LOG_ERROR("usbRedir->data is mandatory");
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

  for(int i = 0; i < PS_CHANNEL_MAX; ++i)
  {
    g_ps.channels[i].id = 0;
    atomic_store_explicit(&g_ps.channels[i].available,
        false, memory_order_release);
  }

  if (channel_connect(&g_ps.channels[0]) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("channel connect failed");
    goto err_connect;
  }

  PS_LOG_INFO("Connected");
  g_ps.connected = true;
  SPICE_UNLOCK(initialConnectLock);
  return true;

err_connect:
  close(g_ps.epollfd);
  g_ps.epollfd = -1;

err_config:
  free((char *)g_ps.config.host);
  g_ps.config.host = NULL;

err_password:
  free((char *)g_ps.config.password);
  g_ps.config.password = NULL;

err_host:
  SPICE_UNLOCK(initialConnectLock);
  return false;
}

void purespice_cancelConnect(void)
{
  if (!g_ps.initialized)
    return;

  atomic_store_explicit(
      &g_ps.connectCancelled, true, memory_order_release);
  const unsigned int channelCount = atomic_load_explicit(
      &g_ps.channelCount, memory_order_acquire);
  for(unsigned int i = 0; i < channelCount; ++i)
    channel_cancelConnect(&g_ps.channels[i]);
}

void purespice_beginChannelConnect(PSChannelType channel)
{
  if (!g_ps.initialized)
    purespice_init(NULL);

  PSChannel * ch = getChannel(channel);
  if (ch)
    atomic_store_explicit(
        &ch->connectCancelled, false, memory_order_release);
}

void purespice_cancelChannelConnect(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return;

  atomic_store_explicit(
      &ch->connectCancelled, true, memory_order_release);
  channel_cancelConnect(ch);
}

static void purespice_disconnectNow(bool wasConnected)
{
  channelUSBRedir_shutdownBegin();

  const unsigned int channelCount = atomic_load_explicit(
      &g_ps.channelCount, memory_order_acquire);
  for(int i = (int)channelCount - 1; i >= 0; --i)
  {
    channel_internal_disconnect(&g_ps.channels[i]);
    atomic_store_explicit(&g_ps.channels[i].available,
        false, memory_order_release);
  }

  if (g_ps.epollfd >= 0)
  {
    close(g_ps.epollfd);
    g_ps.epollfd = -1;
  }

  channelUSBRedir_shutdownEnd();
  atomic_store_explicit(
      &g_ps.channelCount, PS_CHANNEL_MAX, memory_order_release);

  if (g_ps.motionBuffer)
  {
    free(g_ps.motionBuffer);
    g_ps.motionBuffer = NULL;
  }
  g_ps.motionBufferSize = 0;

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

  agent_disconnect(false);

  if (wasConnected)
    PS_LOG_INFO("Disconnected");
}

static bool purespice_finishPendingDisconnectLocked(void)
{
  bool finished = false;
  while(atomic_exchange(&g_ps.disconnectPending, false))
  {
    const bool wasConnected =
      atomic_exchange(&g_ps.disconnectWasConnected, false);
    purespice_disconnectNow(wasConnected);
    finished = true;
  }

  return finished;
}

void purespice_disconnect(void)
{
  if (!g_ps.initialized)
    purespice_init(NULL);

  purespice_cancelConnect();
  SPICE_LOCK(initialConnectLock);

  /*
   * Handlers and application callbacks execute inside purespice_process().
   * Leave their channel buffers alive until the dispatcher has unwound.
   */
  SPICE_LOCK(disconnectLock);
  if (atomic_exchange(&g_ps.connected, false))
    atomic_store(&g_ps.disconnectWasConnected, true);
  atomic_store(&g_ps.disconnectPending, true);
  bool finished = false;
  if (!atomic_load(&g_ps.processing))
    finished = purespice_finishPendingDisconnectLocked();
  SPICE_UNLOCK(disconnectLock);

  if (finished)
    channelUSBRedir_finishShutdown();

  SPICE_UNLOCK(initialConnectLock);
}

typedef enum ChannelReadStatus
{
  CHANNEL_READ_MORE,
  CHANNEL_READ_DRAINED,
  CHANNEL_READ_DISCONNECTED,
  CHANNEL_READ_ERROR,
  CHANNEL_READ_ACK_ERROR,
  CHANNEL_READ_SHUTDOWN
}
ChannelReadStatus;

static size_t consumeReadAhead(
    PSChannel * channel, void * dst, size_t size)
{
  const size_t available =
    channel->readAheadSize - channel->readAheadOffset;
  const size_t length = size < available ? size : available;
  if (!length)
    return 0;

  if (dst)
    memcpy(dst, channel->readAhead + channel->readAheadOffset, length);

  channel->readAheadOffset += length;
  if (channel->readAheadOffset == channel->readAheadSize)
  {
    channel->readAheadOffset = 0;
    channel->readAheadSize   = 0;
  }

  return length;
}

static ChannelReadStatus receiveWithReadAhead(PSChannel * channel,
    void * dst, size_t size, size_t * received)
{
  assert(size > 0);
  assert(channel->readAheadSize == 0);

  struct iovec iov[] =
  {
    {
      .iov_base = dst,
      .iov_len  = size
    },
    {
      .iov_base = channel->readAhead,
      .iov_len  = SPICE_READ_AHEAD_SIZE
    }
  };
  struct msghdr message =
  {
    .msg_iov    = iov,
    .msg_iovlen = sizeof(iov) / sizeof(iov[0])
  };

  ssize_t length;
  do
    length = recvmsg(channel->socket, &message, MSG_DONTWAIT);
  while(length < 0 && errno == EINTR);

  if (length == 0)
    return CHANNEL_READ_DISCONNECTED;

  if (length < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return CHANNEL_READ_DRAINED;

    PS_LOG_ERROR("%s: Failed to read from the socket: %s",
        channel->name, strerror(errno));
    return CHANNEL_READ_ERROR;
  }

  *received = (size_t)length < size ? (size_t)length : size;
  const size_t readAhead = (size_t)length - *received;
  channel->readAheadOffset = 0;
  channel->readAheadSize   = (unsigned int)readAhead;
  return CHANNEL_READ_MORE;
}

static ChannelReadStatus receiveReadAhead(PSChannel * channel)
{
  assert(channel->readAheadSize == 0);

  ssize_t length;
  do
    length = recv(channel->socket, channel->readAhead,
        SPICE_READ_AHEAD_SIZE, MSG_DONTWAIT);
  while(length < 0 && errno == EINTR);

  if (length == 0)
    return CHANNEL_READ_DISCONNECTED;

  if (length < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return CHANNEL_READ_DRAINED;

    PS_LOG_ERROR("%s: Failed to read from the socket: %s",
        channel->name, strerror(errno));
    return CHANNEL_READ_ERROR;
  }

  channel->readAheadOffset = 0;
  channel->readAheadSize   = (unsigned int)length;
  return CHANNEL_READ_MORE;
}

static ChannelReadStatus beginMessage(PSChannel * channel)
{
  if (channel->header.size > SPICE_MAX_MESSAGE_SIZE)
  {
    PS_LOG_ERROR("%s: message payload is too large (%u bytes, maximum %u)",
        channel->name, channel->header.size, SPICE_MAX_MESSAGE_SIZE);
    return CHANNEL_READ_ERROR;
  }

  if (!channel_ack(channel))
  {
    PS_LOG_ERROR("%s: Failed to send message ack", channel->name);
    return CHANNEL_READ_ACK_ERROR;
  }

  channel->bufferRead = 0;
  if (channel->header.type < SPICE_MSG_BASE_LAST)
    channel->handlerFn = channel_onMessage(channel);
  else
    channel->handlerFn = channel->onMessage(channel);

  if (atomic_load(&g_ps.disconnectPending))
    return CHANNEL_READ_SHUTDOWN;

  if (channel->handlerFn == PS_HANDLER_ERROR)
  {
    PS_LOG_ERROR("%s: invalid message: %d",
        channel->name, channel->header.type);
    return CHANNEL_READ_ERROR;
  }

  if (channel->handlerFn == PS_HANDLER_DISCARD)
  {
    channel->discarding  = true;
    channel->discardSize = channel->header.size;
    return CHANNEL_READ_MORE;
  }

  if (channel->bufferSize >= channel->header.size)
    return CHANNEL_READ_MORE;

  uint8_t * buffer = realloc(channel->buffer, channel->header.size);
  if (!buffer)
  {
    PS_LOG_ERROR("out of memory");
    return CHANNEL_READ_ERROR;
  }

  channel->buffer     = buffer;
  channel->bufferSize = channel->header.size;
  return CHANNEL_READ_MORE;
}

static ChannelReadStatus finishMessage(PSChannel * channel)
{
  channel->headerRead = 0;

  const PS_STATUS status = channel->handlerFn(channel);
  if (atomic_load(&g_ps.disconnectPending))
    return CHANNEL_READ_SHUTDOWN;

  switch(status)
  {
    case PS_STATUS_OK:
    case PS_STATUS_HANDLED:
      return CHANNEL_READ_MORE;

    case PS_STATUS_NODATA:
      return CHANNEL_READ_DISCONNECTED;

    default:
      PS_LOG_ERROR("%s: Handler reported read error", channel->name);
      return CHANNEL_READ_ERROR;
  }
}

static ChannelReadStatus processChannel(PSChannel * channel)
{
  if (channel->headerRead < sizeof(SpiceMiniDataHeader))
  {
    const size_t copied = consumeReadAhead(channel,
        (uint8_t *)&channel->header + channel->headerRead,
        sizeof(SpiceMiniDataHeader) - channel->headerRead);
    channel->headerRead += copied;

    if (channel->headerRead < sizeof(SpiceMiniDataHeader))
    {
      size_t received;
      const ChannelReadStatus status = receiveWithReadAhead(channel,
          (uint8_t *)&channel->header + channel->headerRead,
          sizeof(SpiceMiniDataHeader) - channel->headerRead, &received);
      if (status != CHANNEL_READ_MORE)
        return status;

      channel->headerRead += received;
      if (channel->headerRead < sizeof(SpiceMiniDataHeader))
        return CHANNEL_READ_MORE;
    }

    const ChannelReadStatus status = beginMessage(channel);
    if (status != CHANNEL_READ_MORE)
      return status;
  }

  if (channel->discarding)
  {
    const size_t discarded =
      consumeReadAhead(channel, NULL, channel->discardSize);
    channel->discardSize -= (unsigned int)discarded;

    if (channel->discardSize)
    {
      const ChannelReadStatus status = receiveReadAhead(channel);
      if (status != CHANNEL_READ_MORE)
        return status;

      const size_t discarded =
        consumeReadAhead(channel, NULL, channel->discardSize);
      channel->discardSize -= (unsigned int)discarded;
    }

    if (channel->discardSize)
      return CHANNEL_READ_MORE;

    channel->discarding = false;
    channel->headerRead = 0;
    return CHANNEL_READ_MORE;
  }

  if (channel->bufferRead == channel->header.size)
    return finishMessage(channel);

  const size_t copied = consumeReadAhead(channel,
      channel->buffer + channel->bufferRead,
      channel->header.size - channel->bufferRead);
  channel->bufferRead += (unsigned int)copied;

  if (channel->bufferRead < channel->header.size)
  {
    size_t received;
    const ChannelReadStatus status = receiveWithReadAhead(channel,
        channel->buffer + channel->bufferRead,
        channel->header.size - channel->bufferRead, &received);
    if (status != CHANNEL_READ_MORE)
      return status;

    channel->bufferRead += (unsigned int)received;
  }

  if (channel->bufferRead < channel->header.size)
    return CHANNEL_READ_MORE;

  return finishMessage(channel);
}

static PSStatus purespice_processInternal(int timeout)
{
  static struct epoll_event events[PS_CHANNEL_CAPACITY];
  const unsigned int channelCount = atomic_load_explicit(
      &g_ps.channelCount, memory_order_acquire);

  // check for pending disconnects
  for(unsigned int i = 0; i < channelCount; ++i)
    if (g_ps.channels[i].initDone && g_ps.channels[i].doDisconnect)
    {
      channel_disconnectPending(&g_ps.channels[i]);
      if (atomic_load_explicit(&g_ps.disconnectPending, memory_order_acquire))
        return PS_STATUS_SHUTDOWN;
    }

  int nfds = epoll_wait(g_ps.epollfd, events, (int)channelCount, timeout);
  if (atomic_load(&g_ps.disconnectPending))
    return PS_STATUS_SHUTDOWN;

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
      if (events[i].events & (EPOLLERR | EPOLLHUP))
        goto done_disconnect;

      if ((events[i].events & EPOLLOUT) &&
          atomic_load_explicit(
            &channel->writeBlocked, memory_order_acquire) &&
          !channel_flushWrite(channel))
        goto done_disconnect;

      if (!(events[i].events & EPOLLIN))
      {
        ++done;
        events[i].data.ptr = NULL;
        continue;
      }

      switch(processChannel(channel))
      {
        case CHANNEL_READ_MORE:
          continue;

        case CHANNEL_READ_DRAINED:
          ++done;
          events[i].data.ptr = NULL;
          continue;

        case CHANNEL_READ_DISCONNECTED:
          goto done_disconnect;

        case CHANNEL_READ_ACK_ERROR:
          return PS_STATUS_ERR_ACK;

        case CHANNEL_READ_SHUTDOWN:
          return PS_STATUS_SHUTDOWN;

        case CHANNEL_READ_ERROR:
          return PS_STATUS_ERR_READ;
      }

done_disconnect:
      ++done;
      events[i].data.ptr = NULL;
      channel_internal_disconnect(channel);
      if (atomic_load_explicit(&g_ps.disconnectPending, memory_order_acquire))
        return PS_STATUS_SHUTDOWN;
    }
  }

  for(unsigned int i = 0; i < channelCount; ++i)
    if (g_ps.channels[i].connected)
      return PS_STATUS_RUN;

  g_ps.sessionID = 0;

  channelUSBRedir_shutdownBegin();

  for(int i = (int)channelCount - 1; i >= 0; --i)
    if (g_ps.channels[i].socket >= 0)
    {
      close(g_ps.channels[i].socket);
      g_ps.channels[i].socket = -1;
    }

  channelUSBRedir_shutdownEnd();
  atomic_store_explicit(
      &g_ps.channelCount, PS_CHANNEL_MAX, memory_order_release);
  channelUSBRedir_finishShutdown();
  PS_LOG_INFO("Shutdown");
  return PS_STATUS_SHUTDOWN;
}

PSStatus purespice_process(int timeout)
{
  SPICE_LOCK(disconnectLock);
  if (!atomic_load(&g_ps.connected) ||
      atomic_load(&g_ps.disconnectPending))
  {
    const bool finished = purespice_finishPendingDisconnectLocked();
    SPICE_UNLOCK(disconnectLock);
    if (finished)
      channelUSBRedir_finishShutdown();
    return PS_STATUS_SHUTDOWN;
  }
  atomic_store(&g_ps.processing, true);
  SPICE_UNLOCK(disconnectLock);

  PSStatus status = purespice_processInternal(timeout);

  SPICE_LOCK(disconnectLock);
  atomic_store(&g_ps.processing, false);

  if (atomic_load(&g_ps.disconnectPending))
  {
    const bool finished = purespice_finishPendingDisconnectLocked();
    status = PS_STATUS_SHUTDOWN;
    SPICE_UNLOCK(disconnectLock);
    if (finished)
      channelUSBRedir_finishShutdown();
    return status;
  }
  SPICE_UNLOCK(disconnectLock);

  return status;
}

bool purespice_getServerInfo(PSServerInfo * info)
{
  if (!g_ps.guestName)
    return false;

  char * name = strdup(g_ps.guestName);
  if (!name)
  {
    PS_LOG_ERROR("Failed to allocate the server name");
    return false;
  }

  memcpy(info->uuid, g_ps.guestUUID, sizeof(g_ps.guestUUID));
  info->name = name;

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

    case PS_CHANNEL_CURSOR:
      return SPICE_CHANNEL_CURSOR;

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

  return atomic_load_explicit(&ch->available, memory_order_acquire);
}

bool purespice_channelConnected(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;
  return ch->connected;
}

static void lockChannelConnection(PSChannel * ch)
{
  bool expected = false;
  while (!atomic_compare_exchange_weak_explicit(&ch->connecting,
      &expected, true, memory_order_acquire, memory_order_relaxed))
  {
    while (atomic_load_explicit(&ch->connecting, memory_order_relaxed))
      sched_yield();
    expected = false;
  }
}

static void unlockChannelConnection(PSChannel * ch)
{
  atomic_store_explicit(&ch->connecting, false, memory_order_release);
}

static PS_STATUS connectChannel(PSChannel * ch, bool sessionFatal)
{
  PS_STATUS status;

  if (channel_cancelDisconnect(ch))
    return PS_STATUS_OK;

  if ((status = channel_connect(ch)) != PS_STATUS_OK)
  {
    if (sessionFatal && !atomic_load_explicit(
          &g_ps.connectCancelled, memory_order_acquire))
      purespice_disconnect();
    PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
    return status;
  }

  PS_LOG_INFO("%s channel connected", ch->name);
  if (ch->onConnect && (status = ch->onConnect(ch)) != PS_STATUS_OK)
  {
    if (sessionFatal)
      purespice_disconnect();
    else
      channel_internal_disconnect(ch);
    PS_LOG_ERROR("Failed to connect to the %s channel", ch->name);
    return status;
  }

  return PS_STATUS_OK;
}

PS_STATUS ps_connectChannel(PSChannel * ch)
{
  lockChannelConnection(ch);

  if (!atomic_load_explicit(&ch->available, memory_order_acquire))
  {
    PS_LOG_ERROR("%s: Channel is not availble", ch->name);
    unlockChannelConnection(ch);
    return PS_STATUS_ERROR;
  }

  const PS_STATUS status = connectChannel(ch, false);
  unlockChannelConnection(ch);
  return status;
}

PS_STATUS ps_bindChannel(PSChannel * ch, uint8_t id)
{
  lockChannelConnection(ch);

  const bool wasAvailable =
    atomic_load_explicit(&ch->available, memory_order_acquire);
  const bool connected =
    atomic_load_explicit(&ch->connected, memory_order_acquire);

  if (connected && ch->id != id)
  {
    PS_LOG_ERROR("Protocol error. The server replaced the connected %s "
        "channel (%u -> %u)", ch->name, ch->id, id);
    unlockChannelConnection(ch);
    return PS_STATUS_ERROR;
  }

  const bool newIdentity = !wasAvailable || ch->id != id;
  if (!connected && newIdentity)
  {
    atomic_store_explicit(&ch->available, false, memory_order_release);
    ch->id = id;
  }

  atomic_store_explicit(&ch->available, true, memory_order_release);

  PS_STATUS status = PS_STATUS_OK;
  if (newIdentity &&
      (!ch->enable || *ch->enable) &&
      (!ch->autoConnect || *ch->autoConnect) &&
      !connected)
    status = connectChannel(ch, true);

  unlockChannelConnection(ch);
  return status;
}

void ps_unpublishChannel(PSChannel * ch)
{
  lockChannelConnection(ch);
  atomic_store_explicit(&ch->available, false, memory_order_release);
  unlockChannelConnection(ch);
}

PSChannel * ps_allocChannel(void)
{
  const unsigned int channelCount = atomic_load_explicit(
      &g_ps.channelCount, memory_order_relaxed);
  if (channelCount >= PS_CHANNEL_CAPACITY)
  {
    PS_LOG_ERROR("The channel instance limit has been reached");
    return NULL;
  }

  PSChannel * ch = &g_ps.channels[channelCount];
  memset(ch, 0, sizeof(*ch));
  atomic_init(&ch->available   , false);
  atomic_init(&ch->connected   , false);
  atomic_init(&ch->ready       , false);
  atomic_init(&ch->doDisconnect, false);
  atomic_init(&ch->writeBlocked, false);
  atomic_init(&ch->initDone    , false);
  atomic_init(&ch->connecting  , false);
  atomic_init(&ch->connectCancelled, false);
  atomic_init(&ch->socket      , -1);
  SPICE_LOCK_INIT(ch->lock);
  SPICE_LOCK_INIT(ch->connectSocketLock);
  ch->connectSocket = -1;
  atomic_store_explicit(
      &g_ps.channelCount, channelCount + 1, memory_order_release);
  return ch;
}

bool purespice_connectChannel(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;

  return ps_connectChannel(ch) == PS_STATUS_OK;
}

bool ps_disconnectChannel(PSChannel * ch)
{
  lockChannelConnection(ch);
  if (!atomic_load_explicit(&ch->available, memory_order_acquire))
  {
    PS_LOG_ERROR("%s: Channel is not availble", ch->name);
    unlockChannelConnection(ch);
    return false;
  }

  if (atomic_load_explicit(&ch->connected, memory_order_acquire))
    channel_disconnect(ch);

  unlockChannelConnection(ch);
  return true;
}

bool purespice_disconnectChannel(PSChannelType channel)
{
  PSChannel * ch = getChannel(channel);
  if (!ch)
    return false;

  return ps_disconnectChannel(ch);
}
