/**
 * PureSpice - A pure C implementation of the SPICE client protocol
 * Copyright © 2017-2026 Geoffrey McRae <geoff@hostfission.com>
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

#include "channel_usbredir.h"
#include "channel.h"
#include "log.h"

#include <stdio.h>

#define USB_REDIR_CHANNEL_COUNT (UINT8_MAX + 1)
#define USB_REDIR_EVENT(state)   (1U << (state))

struct PSUSBRedirChannel
{
  uint8_t id;
  char    name[16];

  _Atomic(PSChannel *) channel;
  _Atomic(void *)      opaque;
  atomic_bool          usable;
  atomic_bool          available;
  atomic_bool          connected;
  atomic_bool          removalPending;
  atomic_uint          events;
};

static struct PSUSBRedirChannel usbRedir[USB_REDIR_CHANNEL_COUNT];
static atomic_bool deferEvents;
static atomic_bool resetPending;
static atomic_flag dispatching = ATOMIC_FLAG_INIT;

static bool hasEvents(void)
{
  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
    if (atomic_load_explicit(&usbRedir[i].events, memory_order_acquire))
      return true;

  return false;
}

static void resetChannels(void)
{
  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
  {
    struct PSUSBRedirChannel * state = &usbRedir[i];
    atomic_store_explicit(&state->channel, NULL, memory_order_release);
    atomic_store_explicit(&state->opaque, NULL, memory_order_release);
    atomic_store_explicit(&state->usable, false, memory_order_release);
    atomic_store_explicit(&state->available, false, memory_order_release);
    atomic_store_explicit(&state->connected, false, memory_order_release);
    atomic_store_explicit(
        &state->removalPending, false, memory_order_release);
    atomic_store_explicit(&state->events, 0, memory_order_release);
  }
}

static void dispatchEvents(void)
{
  if (atomic_load_explicit(&deferEvents, memory_order_acquire) ||
      atomic_flag_test_and_set_explicit(&dispatching, memory_order_acquire))
    return;

  for(;;)
  {
    bool dispatched = false;
    for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
    {
      struct PSUSBRedirChannel * state = &usbRedir[i];
      const unsigned int events =
        atomic_exchange_explicit(&state->events, 0, memory_order_acq_rel);
      if (!events)
        continue;

      dispatched = true;
      if (!g_ps.config.usbRedir.state)
        continue;

      for(unsigned int event = PS_USB_REDIR_AVAILABLE;
          event <= PS_USB_REDIR_UNAVAILABLE; ++event)
        if (events & USB_REDIR_EVENT(event))
          g_ps.config.usbRedir.state(state, (PSUSBRedirState)event,
              g_ps.config.usbRedir.opaque);
    }

    if (!dispatched ||
        atomic_load_explicit(&deferEvents, memory_order_acquire))
      break;
  }

  if (!atomic_load_explicit(&deferEvents, memory_order_acquire) &&
      atomic_load_explicit(&resetPending, memory_order_acquire) &&
      !hasEvents())
  {
    resetChannels();
    atomic_store_explicit(&resetPending, false, memory_order_release);
  }

  atomic_flag_clear_explicit(&dispatching, memory_order_release);

  if (!atomic_load_explicit(&deferEvents, memory_order_acquire) && hasEvents())
    dispatchEvents();
}

static void queueEvents(struct PSUSBRedirChannel * state,
    unsigned int events)
{
  if (!events)
    return;

  atomic_fetch_or_explicit(&state->events, events, memory_order_release);
  dispatchEvents();
}

static void disconnected(struct PSUSBRedirChannel * state)
{
  unsigned int events = 0;
  if (atomic_exchange_explicit(
        &state->connected, false, memory_order_acq_rel))
    events |= USB_REDIR_EVENT(PS_USB_REDIR_DISCONNECTED);

  if (atomic_exchange_explicit(
        &state->removalPending, false, memory_order_acq_rel) &&
      atomic_exchange_explicit(
        &state->available, false, memory_order_acq_rel))
    events |= USB_REDIR_EVENT(PS_USB_REDIR_UNAVAILABLE);

  queueEvents(state, events);
}

static void available(struct PSUSBRedirChannel * state)
{
  if (!atomic_exchange_explicit(
        &state->available, true, memory_order_acq_rel))
    queueEvents(state, USB_REDIR_EVENT(PS_USB_REDIR_AVAILABLE));
}

static void connected(struct PSUSBRedirChannel * state)
{
  if (!atomic_exchange_explicit(
        &state->connected, true, memory_order_acq_rel))
    queueEvents(state, USB_REDIR_EVENT(PS_USB_REDIR_CONNECTED));
}

static PS_STATUS onConnect(PSChannel * channel)
{
  channel->initDone = true;
  return PS_STATUS_OK;
}

static void onDisconnect(PSChannel * channel)
{
  disconnected((struct PSUSBRedirChannel *)channel->opaque);
}

static PS_STATUS onData(PSChannel * channel)
{
  struct PSUSBRedirChannel * state =
    (struct PSUSBRedirChannel *)channel->opaque;
  if (!g_ps.config.usbRedir.data(state, channel->buffer,
        channel->header.size, g_ps.config.usbRedir.opaque))
    return PS_STATUS_NODATA;

  return PS_STATUS_OK;
}

static PSHandlerFn onMessage(PSChannel * channel)
{
  switch(channel->header.type)
  {
    case SPICE_MSG_SPICEVMC_DATA:
      return onData;

    case SPICE_MSG_SPICEVMC_COMPRESSED_DATA:
      PS_LOG_ERROR("%s: compressed VMC data was not negotiated", channel->name);
      return PS_HANDLER_ERROR;

    default:
      return PS_HANDLER_ERROR;
  }
}

static PSChannel * getChannel(struct PSUSBRedirChannel * state)
{
  PSChannel * channel =
    atomic_load_explicit(&state->channel, memory_order_acquire);
  if (channel)
    return channel;

  channel = ps_allocChannel();
  if (!channel)
    return NULL;

  channel->spiceType        = SPICE_CHANNEL_USBREDIR;
  channel->name             = state->name;
  channel->enable           = &g_ps.config.usbRedir.enable;
  channel->autoConnect      = &g_ps.config.usbRedir.autoConnect;
  channel->clientCapsBytes  = 0;
  channel->onConnect        = onConnect;
  channel->onDisconnect     = onDisconnect;
  channel->onMessage        = onMessage;
  channel->opaque           = state;
  channel->nonblockingWrite = true;

  atomic_store_explicit(&state->channel, channel, memory_order_release);
  return channel;
}

static void removeChannel(struct PSUSBRedirChannel * state)
{
  if (!atomic_exchange_explicit(
        &state->usable, false, memory_order_acq_rel) ||
      atomic_exchange_explicit(
        &state->removalPending, true, memory_order_acq_rel))
    return;

  PSChannel * channel =
    atomic_load_explicit(&state->channel, memory_order_acquire);
  ps_unpublishChannel(channel);

  if (atomic_load_explicit(&channel->connected, memory_order_acquire))
    channel_disconnect(channel);
  else
    disconnected(state);
}

void channelUSBRedir_init(void)
{
  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
  {
    usbRedir[i].id = (uint8_t)i;
    snprintf(usbRedir[i].name, sizeof(usbRedir[i].name),
        "USBREDIR %u", i);
  }

  atomic_flag_clear_explicit(&dispatching, memory_order_release);
}

void channelUSBRedir_sessionStart(void)
{
  atomic_store_explicit(&deferEvents, true, memory_order_release);
  resetChannels();
  atomic_store_explicit(&resetPending, false, memory_order_release);
  atomic_store_explicit(&deferEvents, false, memory_order_release);
}

PS_STATUS channelUSBRedir_reconcile(
    const SpiceChannelID * channels, size_t count)
{
  bool seen[USB_REDIR_CHANNEL_COUNT] = { false };

  if (g_ps.config.usbRedir.enable)
    for(size_t i = 0; i < count; ++i)
    {
      if (atomic_load_explicit(&g_ps.disconnectPending, memory_order_acquire))
        return PS_STATUS_OK;

      const SpiceChannelID * id = &channels[i];
      if (id->type != SPICE_CHANNEL_USBREDIR || seen[id->channel_id])
        continue;

      seen[id->channel_id] = true;
      struct PSUSBRedirChannel * state = &usbRedir[id->channel_id];
      PSChannel * channel = getChannel(state);
      if (!channel)
        return PS_STATUS_ERROR;

      if (atomic_exchange_explicit(
            &state->removalPending, false, memory_order_acq_rel))
        channel_cancelDisconnect(channel);

      const PS_STATUS status = ps_bindChannel(channel, id->channel_id);
      if (status != PS_STATUS_OK)
        return status;

      atomic_store_explicit(&state->usable, true, memory_order_release);
      available(state);
      if (atomic_load_explicit(&g_ps.disconnectPending, memory_order_acquire))
        return PS_STATUS_OK;

      if (atomic_load_explicit(&channel->connected, memory_order_acquire))
      {
        connected(state);
        if (atomic_load_explicit(
              &g_ps.disconnectPending, memory_order_acquire))
          return PS_STATUS_OK;
      }
    }

  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
    if (!seen[i])
      removeChannel(&usbRedir[i]);

  return PS_STATUS_OK;
}

void channelUSBRedir_shutdownBegin(void)
{
  atomic_store_explicit(&deferEvents, true, memory_order_release);

  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
  {
    struct PSUSBRedirChannel * state = &usbRedir[i];
    PSChannel * channel =
      atomic_load_explicit(&state->channel, memory_order_acquire);
    if (!channel)
      continue;

    if (atomic_load_explicit(&state->available, memory_order_acquire))
    {
      atomic_store_explicit(&state->usable, false, memory_order_release);
      atomic_store_explicit(
          &state->removalPending, true, memory_order_release);
      atomic_store_explicit(
          &channel->available, false, memory_order_release);
    }

    if (!atomic_load_explicit(&channel->connected, memory_order_acquire))
      disconnected(state);
  }
}

void channelUSBRedir_shutdownEnd(void)
{
  for(unsigned int i = 0; i < USB_REDIR_CHANNEL_COUNT; ++i)
  {
    struct PSUSBRedirChannel * state = &usbRedir[i];
    if (!atomic_load_explicit(&state->channel, memory_order_acquire))
      continue;

    disconnected(state);
    atomic_store_explicit(&state->channel, NULL, memory_order_release);
  }
}

void channelUSBRedir_finishShutdown(void)
{
  atomic_store_explicit(&resetPending, true, memory_order_release);
  atomic_store_explicit(&deferEvents, false, memory_order_release);
  dispatchEvents();
}

uint8_t purespice_usbRedirChannelId(
    const PSUSBRedirChannel * channel)
{
  return channel ? channel->id : 0;
}

bool purespice_usbRedirAvailable(
    const PSUSBRedirChannel * channel)
{
  return channel && atomic_load_explicit(
      &channel->usable, memory_order_acquire);
}

bool purespice_usbRedirConnected(
    const PSUSBRedirChannel * channel)
{
  return channel && atomic_load_explicit(
      &channel->connected, memory_order_acquire);
}

bool purespice_usbRedirConnect(
    PSUSBRedirChannel * channel)
{
  if (!purespice_usbRedirAvailable(channel))
    return false;

  PSChannel * spiceChannel =
    atomic_load_explicit(&channel->channel, memory_order_acquire);
  if (!spiceChannel || ps_connectChannel(spiceChannel) != PS_STATUS_OK)
    return false;

  connected(channel);
  return true;
}

bool purespice_usbRedirDisconnect(
    PSUSBRedirChannel * channel)
{
  if (!purespice_usbRedirAvailable(channel))
    return false;

  PSChannel * spiceChannel =
    atomic_load_explicit(&channel->channel, memory_order_acquire);
  return spiceChannel && ps_disconnectChannel(spiceChannel);
}

void purespice_usbRedirSetOpaque(
    PSUSBRedirChannel * channel, void * opaque)
{
  if (channel)
    atomic_store_explicit(&channel->opaque, opaque, memory_order_release);
}

void * purespice_usbRedirGetOpaque(
    const PSUSBRedirChannel * channel)
{
  return channel ?
    atomic_load_explicit(&channel->opaque, memory_order_acquire) : NULL;
}

bool purespice_usbRedirWrite(
    PSUSBRedirChannel * channel, const void * data, size_t size)
{
  return purespice_usbRedirWriteNonblocking(
      channel, data, size) == (ssize_t)size;
}

ssize_t purespice_usbRedirWriteNonblocking(
    PSUSBRedirChannel * channel, const void * data, size_t size)
{
  if (!purespice_usbRedirConnected(channel))
    return -1;

  PSChannel * spiceChannel =
    atomic_load_explicit(&channel->channel, memory_order_acquire);
  return spiceChannel ? channel_sendMessageNonblocking(spiceChannel,
      SPICE_MSGC_SPICEVMC_DATA, data, size) : -1;
}

bool purespice_usbRedirWritable(
    const PSUSBRedirChannel * channel)
{
  if (!purespice_usbRedirConnected(channel))
    return false;

  const PSChannel * spiceChannel =
    atomic_load_explicit(&channel->channel, memory_order_acquire);
  return spiceChannel && !atomic_load_explicit(
      &spiceChannel->writeBlocked, memory_order_acquire);
}
