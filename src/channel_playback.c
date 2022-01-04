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
#include "channel_playback.h"

#include "messages.h"

const SpiceLinkHeader * channelPlayback_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES   / sizeof(uint32_t)];
    uint32_t        channelCaps[PLAYBACK_CAPS_BYTES / sizeof(uint32_t)];
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
      .channel_type     = SPICE_CHANNEL_PLAYBACK,
      .num_common_caps  = COMMON_CAPS_BYTES   / sizeof(uint32_t),
      .num_channel_caps = PLAYBACK_CAPS_BYTES / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  p.message.connection_id = g_ps.sessionID;
  p.message.channel_id    = g_ps.channelID;

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (g_ps.config.playback.volume || g_ps.config.playback.mute)
    PLAYBACK_SET_CAPABILITY(p.channelCaps, SPICE_PLAYBACK_CAP_VOLUME);

  return &p.header;
}

PS_STATUS channelPlayback_onRead(struct PSChannel * channel, int * dataAvailable)
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
    case SPICE_MSG_PLAYBACK_START:
    {
      SpiceMsgPlaybackStart in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgPlaybackStart");
        return status;
      }

      PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
      if (in.format == SPICE_AUDIO_FMT_S16)
        fmt = PS_AUDIO_FMT_S16;

      g_ps.config.playback.start(in.channels, in.frequency, fmt, in.time);
      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_DATA:
    {
      SpiceMsgPlaybackPacket * in =
        (SpiceMsgPlaybackPacket *)alloca(header.size);
      if ((status = channel_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgPlaybackPacket");
        return status;
      }

      g_ps.config.playback.data(in->data, header.size - sizeof(*in));
      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_STOP:
      g_ps.config.playback.stop();
      return PS_STATUS_OK;

    case SPICE_MSG_PLAYBACK_VOLUME:
    {
      SpiceMsgAudioVolume * in =
        (SpiceMsgAudioVolume *)alloca(header.size);
      if ((status = channel_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgAudioVolume");
        return status;
      }

      if (g_ps.config.playback.volume)
        g_ps.config.playback.volume(in->nchannels, in->volume);

      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_MUTE:
    {
      SpiceMsgAudioMute in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
      {
        PS_LOG_ERROR("Failed to read SpiceMsgAudioMute");
        return status;
      }

      if (g_ps.config.playback.mute)
        g_ps.config.playback.mute(in.mute);

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
