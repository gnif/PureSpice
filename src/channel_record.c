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

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  RECORD_SET_CAPABILITY(p.channelCaps, SPICE_RECORD_CAP_VOLUME);

  return &p.header;
}

PS_STATUS channelRecord_onRead(struct PSChannel * channel, int * dataAvailable)
{
  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = channel_onRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
  {
    PS_LOG_ERROR("Failed to read SpiceMiniDataHeader");
    return status;
  }

  switch(header.type)
  {
    case SPICE_MSG_RECORD_START:
    {
      SpiceMsgRecordStart in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgRecordStart");
        return status;
      }

      PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
      if (in.format == SPICE_AUDIO_FMT_S16)
        fmt = PS_AUDIO_FMT_S16;

      g_ps.config.record.start(in.channels, in.frequency, fmt);
      return PS_STATUS_OK;
    }

    case SPICE_MSG_RECORD_STOP:
      g_ps.config.record.stop();
      return PS_STATUS_OK;

    case SPICE_MSG_RECORD_VOLUME:
    {
      SpiceMsgAudioVolume * in =
        (SpiceMsgAudioVolume *)alloca(header.size);
      if ((status = channel_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgAudioVolume");
        return status;
      }

      if (g_ps.config.record.volume)
        g_ps.config.record.volume(in->nchannels, in->volume);

      return PS_STATUS_OK;
    }

    case SPICE_MSG_RECORD_MUTE:
    {
      SpiceMsgAudioMute in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgAudioMute");
        return status;
      }

      if (g_ps.config.record.mute)
        g_ps.config.record.mute(in.mute);

      return PS_STATUS_OK;
    }
  }

  if ((status = channel_discardNL(channel, header.size,
          dataAvailable) != PS_STATUS_OK))
  {
    PS_LOG_ERROR("Failed to discard %d bytes", header.size);
    return status;
  }

  return PS_STATUS_OK;
}

bool purespice_writeAudio(void * data, size_t size, uint32_t time)
{
  struct PSChannel * channel = &g_ps.channels[PS_CHANNEL_RECORD];
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

  if (wrote != size)
  {
    PS_LOG_ERROR("Failed to write the audio data");
    return false;
  }

  return true;
}
