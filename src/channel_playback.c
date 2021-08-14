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
#include "channel.h"
#include "channel_playback.h"

#include "messages.h"

struct PSPlayback
{
  void (*start)(int channels, int sampleRate, PSAudioFormat format,
    uint32_t time);
  void (*volume)(int channels, const uint16_t volume[]);
  void (*mute)(bool mute);
  void (*stop)(void);
  void (*data)(uint8_t * data, size_t size);
};

static struct PSPlayback pb = { 0 };

PS_STATUS channelPlayback_onRead(int * dataAvailable)
{
  struct PSChannel *channel = &g_ps.scPlayback;

  SpiceMiniDataHeader header;

  PS_STATUS status;
  if ((status = channel_onRead(channel, &header,
          dataAvailable)) != PS_STATUS_OK)
    return status;

  switch(header.type)
  {
    case SPICE_MSG_PLAYBACK_START:
    {
      SpiceMsgPlaybackStart in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (pb.start)
      {
        PSAudioFormat fmt = PS_AUDIO_FMT_INVALID;
        if (in.format == SPICE_AUDIO_FMT_S16)
          fmt = PS_AUDIO_FMT_S16;

        pb.start(in.channels, in.frequency, fmt, in.time);
      }
      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_DATA:
    {
      SpiceMsgPlaybackPacket * in =
        (SpiceMsgPlaybackPacket *)alloca(header.size);
      if ((status = channel_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (pb.data)
        pb.data(in->data, header.size - sizeof(*in));

      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_STOP:
      if (pb.stop)
        pb.stop();
      return PS_STATUS_OK;

    case SPICE_MSG_PLAYBACK_VOLUME:
    {
      SpiceMsgAudioVolume * in =
        (SpiceMsgAudioVolume *)alloca(header.size);
      if ((status = channel_readNL(channel, in, header.size,
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (pb.volume)
        pb.volume(in->nchannels, in->volume);

      return PS_STATUS_OK;
    }

    case SPICE_MSG_PLAYBACK_MUTE:
    {
      SpiceMsgAudioMute in;
      if ((status = channel_readNL(channel, &in, sizeof(in),
              dataAvailable)) != PS_STATUS_OK)
        return status;

      if (pb.mute)
        pb.mute(in.mute);

      return PS_STATUS_OK;
    }
  }

  return channel_discardNL(channel, header.size, dataAvailable);
}

bool purespice_setAudioCb(
  void (*start)(int channels, int sampleRate, PSAudioFormat format,
    uint32_t time),
  void (*volume)(int channels, const uint16_t volume[]),
  void (*mute)(bool mute),
  void (*stop)(void),
  void (*data)(uint8_t * data, size_t size)
)
{
  if (!start || !stop || !data)
    return false;

  pb.start  = start;
  pb.volume = volume;
  pb.mute   = mute;
  pb.stop   = stop;
  pb.data   = data;

  return true;
}