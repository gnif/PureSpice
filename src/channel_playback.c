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

  memset(p.supportCaps, 0, sizeof(p.supportCaps));
  memset(p.channelCaps, 0, sizeof(p.channelCaps));

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (g_ps.config.playback.volume || g_ps.config.playback.mute)
    PLAYBACK_SET_CAPABILITY(p.channelCaps, SPICE_PLAYBACK_CAP_VOLUME);

  return &p.header;
}

static PS_STATUS onMessage_playbackStart(struct PSChannel * channel)
{
  SpiceMsgPlaybackStart * msg = (SpiceMsgPlaybackStart *)channel->buffer;

  PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
  if (msg->format == SPICE_AUDIO_FMT_S16)
    fmt = PS_AUDIO_FMT_S16;

  g_ps.config.playback.start(msg->channels, msg->frequency, fmt, msg->time);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_playbackData(struct PSChannel * channel)
{
  SpiceMsgPlaybackPacket * msg = (SpiceMsgPlaybackPacket *)channel->buffer;

  g_ps.config.playback.data(msg->data, channel->header.size - sizeof(*msg));
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_playbackStop(struct PSChannel * channel)
{
  (void)channel;
  g_ps.config.playback.stop();
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_playbackVolume(struct PSChannel * channel)
{
  SpiceMsgAudioVolume * msg = (SpiceMsgAudioVolume *)channel->buffer;

  uint16_t volume[msg->nchannels];
  memcpy(&volume, msg->volume, sizeof(volume));

  g_ps.config.playback.volume(msg->nchannels, volume);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_playbackMute(struct PSChannel * channel)
{
  SpiceMsgAudioMute * msg = (SpiceMsgAudioMute *)channel->buffer;

  g_ps.config.playback.mute(msg->mute);
  return PS_STATUS_OK;
}

PSHandlerFn channelPlayback_onMessage(struct PSChannel * channel)
{
  channel->initDone = true;
  switch(channel->header.type)
  {
    case SPICE_MSG_PLAYBACK_START:
      return onMessage_playbackStart;

    //TODO: Lookup this message and see what it's for
    case SPICE_MSG_PLAYBACK_MODE:
      return PS_HANDLER_DISCARD;

    case SPICE_MSG_PLAYBACK_DATA:
      return onMessage_playbackData;

    case SPICE_MSG_PLAYBACK_STOP:
      return onMessage_playbackStop;

    case SPICE_MSG_PLAYBACK_VOLUME:
      if (!g_ps.config.playback.volume)
        return PS_HANDLER_DISCARD;
      return onMessage_playbackVolume;

    case SPICE_MSG_PLAYBACK_MUTE:
      if (!g_ps.config.playback.mute)
        return PS_HANDLER_DISCARD;
      return onMessage_playbackMute;
  }

  return PS_HANDLER_ERROR;
}
