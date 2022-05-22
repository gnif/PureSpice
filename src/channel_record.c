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
#include "channel.h"
#include "channel_record.h"

#include "messages.h"

const SpiceLinkHeader * channelRecord_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[RECORD_CAPS_BYTES / sizeof(uint32_t)];
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
      .channel_type     = SPICE_CHANNEL_RECORD,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = RECORD_CAPS_BYTES / sizeof(uint32_t),
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

  if (g_ps.config.record.volume || g_ps.config.record.mute)
    RECORD_SET_CAPABILITY(p.channelCaps, SPICE_RECORD_CAP_VOLUME);

  return &p.header;
}

static PS_STATUS onMessage_recordStart(PSChannel * channel)
{
  SpiceMsgRecordStart * msg = (SpiceMsgRecordStart *)channel->buffer;

  PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
  if (msg->format == SPICE_AUDIO_FMT_S16)
    fmt = PS_AUDIO_FMT_S16;

  g_ps.config.record.start(msg->channels, msg->frequency, fmt);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_recordStop(PSChannel * channel)
{
  (void)channel;

  g_ps.config.record.stop();
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_recordVolume(PSChannel * channel)
{
  SpiceMsgAudioVolume * msg = (SpiceMsgAudioVolume *)channel->buffer;

  uint16_t volume[msg->nchannels];
  memcpy(&volume, msg->volume, sizeof(volume));

  g_ps.config.record.volume(msg->nchannels, volume);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_recordMute(PSChannel * channel)
{
  SpiceMsgAudioMute * msg = (SpiceMsgAudioMute *)channel->buffer;

  g_ps.config.record.mute(msg->mute);
  return PS_STATUS_OK;
}

PSHandlerFn channelRecord_onMessage(PSChannel * channel)
{
  channel->initDone = true;
  switch(channel->header.type)
  {
    case SPICE_MSG_RECORD_START:
      return onMessage_recordStart;

    case SPICE_MSG_RECORD_STOP:
      return onMessage_recordStop;

    case SPICE_MSG_RECORD_VOLUME:
      if (!g_ps.config.record.volume)
        return PS_HANDLER_DISCARD;
      return onMessage_recordVolume;

    case SPICE_MSG_RECORD_MUTE:
      if (!g_ps.config.record.mute)
        return PS_HANDLER_DISCARD;
      return onMessage_recordMute;
  }

  return PS_HANDLER_ERROR;
}

bool purespice_writeAudio(void * data, size_t size, uint32_t time)
{
  PSChannel * channel = &g_ps.channels[PS_CHANNEL_RECORD];
  if (!channel->connected)
    return false;

  SpiceMsgcRecordPacket * msg =
    SPICE_PACKET(SPICE_MSGC_RECORD_DATA, SpiceMsgcRecordPacket, size);

  msg->time = time;

  SPICE_LOCK(channel->lock);
  if (!SPICE_SEND_PACKET_NL(channel, msg))
  {
    SPICE_UNLOCK(channel->lock);
    PS_LOG_ERROR("Failed to write SpiceMsgcRecordPacket");
    return false;
  }
  const ssize_t wrote = send(channel->socket, data, size, 0);
  SPICE_UNLOCK(channel->lock);

  if ((size_t)wrote != size)
  {
    PS_LOG_ERROR("Failed to write the audio data");
    return false;
  }

  return true;
}
