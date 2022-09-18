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

#ifndef _H_I_MESSAGES_
#define _H_I_MESSAGES_

#include <stdint.h>
#include <spice/enums.h>
#include "draw.h"

#pragma pack(push,1)

typedef struct SpicePoint16
{
  int16_t x, y;
}
SpicePoint16;

typedef struct SpiceMsgMainInit
{
  uint32_t session_id;
  uint32_t display_channels_hint;
  uint32_t supported_mouse_modes;
  uint32_t current_mouse_mode;
  uint32_t agent_connected;
  uint32_t agent_tokens;
  uint32_t multi_media_time;
  uint32_t ram_hint;
}
SpiceMsgMainInit;

typedef struct SpiceChannelID
{
  uint8_t type;
  uint8_t channel_id;
}
SpiceChannelID;

typedef struct SpiceMsgMainName
{
  uint32_t name_len;
  uint8_t  name[]; //name_len
}
SpiceMsgMainName;

typedef struct SpiceMsgMainUUID
{
  uint8_t uuid[16];
}
SpiceMsgMainUUID;

typedef struct SpiceMsgMainChannelsList
{
  uint32_t       num_of_channels;
  SpiceChannelID channels[];
}
SpiceMainChannelsList;

typedef struct SpiceMsgcMainMouseModeRequest
{
  uint16_t mouse_mode;
}
SpiceMsgcMainMouseModeRequest;

typedef struct SpiceMsgPing
{
  uint32_t id;
  uint64_t timestamp;
}
SpiceMsgPing,
SpiceMsgcPong;

typedef struct SpiceMsgSetAck
{
  uint32_t generation;
  uint32_t window;
}
SpiceMsgSetAck;

typedef struct SpiceMsgcAckSync
{
  uint32_t generation;
}
SpiceMsgcAckSync;

typedef struct SpiceMsgNotify
{
  uint64_t time_stamp;
  uint32_t severity;
  uint32_t visibility;
  uint32_t what;
  uint32_t message_len;
  char     message[]; //message_len+1
}
SpiceMsgNotify;

typedef struct SpiceMsgInputsInit
{
  uint16_t modifiers;
}
SpiceMsgInputsInit,
SpiceMsgInputsKeyModifiers,
SpiceMsgcInputsKeyModifiers;

typedef struct SpiceMsgcKeyDown
{
  uint32_t code;
}
SpiceMsgcKeyDown,
SpiceMsgcKeyUp;

typedef struct SpiceMsgcMousePosition
{
  uint32_t x;
  uint32_t y;
  uint16_t button_state;
  uint8_t  display_id;
}
SpiceMsgcMousePosition;

typedef struct SpiceMsgcMouseMotion
{
  int32_t  x;
  int32_t  y;
  uint16_t button_state;
}
SpiceMsgcMouseMotion;

typedef struct SpiceMsgcMousePress
{
  uint8_t  button;
  uint16_t button_state;
}
SpiceMsgcMousePress,
SpiceMsgcMouseRelease;

typedef struct SpiceMsgcDisconnecting
{
  uint64_t time_stamp;
  uint32_t reason;
}
SpiceMsgcDisconnecting;

typedef struct SpiceMsgPlaybackStart
{
  uint32_t      channels;
  SpiceAudioFmt format:16;
  uint32_t      frequency;
  uint32_t      time;
}
SpiceMsgPlaybackStart;

typedef struct SpiceMsgRecordStart
{
  uint32_t channels;
  uint16_t format;
  uint32_t frequency;
}
SpiceMsgRecordStart;

typedef struct SpiceMsgPlaybackPacket
{
  uint32_t time;
  uint8_t  data[];
}
SpiceMsgPlaybackPacket,
SpiceMsgcRecordPacket;

typedef struct SpiceMsgAudioVolume
{
  uint8_t  nchannels;
  uint16_t volume[];
}
SpiceMsgAudioVolume;

typedef struct SpiceMsgAudioMute
{
  uint8_t mute;
}
SpiceMsgAudioMute;

typedef struct SpiceMsgcPlaybackMode
{
  uint32_t           time;
  SpiceAudioDataMode mode:16;
  uint8_t            data[];
}
SpiceMsgPlaybackMode,
SpiceMsgcRecordMode;

typedef struct SpiceMsgcDisplayInit
{
  uint8_t  pixmap_cache_id;
  int64_t  pixmap_cache_size;
  uint8_t  glz_dictionary_id;
  uint32_t glz_dictionary_window_size;
}
SpiceMsgcDisplayInit;

typedef struct SpiceMsgcPreferredCompression
{
  uint8_t image_compression;
}
SpiceMsgcPreferredCompression;

typedef struct SpiceMsgSurfaceCreate
{
  uint32_t surface_id;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t flags;
}
SpiceMsgSurfaceCreate;

typedef struct SpiceMsgSurfaceDestroy
{
  uint32_t surface_id;
}
SpiceMsgSurfaceDestroy;

typedef struct SpiceMsgDisplayBase
{
  uint32_t surface_id;
  SpiceRect box;
  SpiceClip clip;
}
SpiceMsgDisplayBase;

typedef struct SpiceMsgDisplayDrawFill
{
  SpiceMsgDisplayBase base;
  SpiceFill data;
}
SpiceMsgDisplayDrawFill;

typedef struct SpiceMsgDisplayDrawCopy
{
  SpiceMsgDisplayBase base;
  SpiceCopy data;
}
SpiceMsgDisplayDrawCopy;

typedef struct SpiceCursorHeader
{
  uint64_t unique;
  uint8_t type;
  uint16_t width;
  uint16_t height;
  uint16_t hot_spot_x;
  uint16_t hot_spot_y;
}
SpiceCursorHeader;

typedef struct SpiceCursor
{
  uint16_t          flags;
  SpiceCursorHeader header;
  uint8_t           data[];
}
SpiceCursor;

typedef struct SpiceMsgCursorInit
{
  SpicePoint16 position;
  uint16_t     trail_length;
  uint16_t     trail_frequency;
  uint8_t      visible;
  SpiceCursor  cursor;
}
SpiceMsgCursorInit;

typedef struct SpiceMsgCursorSet
{
  SpicePoint16 position;
  uint8_t      visible;
  SpiceCursor  cursor;
}
SpiceMsgCursorSet;

typedef struct SpiceMsgCursorMove
{
  SpicePoint16 position;
}
SpiceMsgCursorMove;

typedef struct SpiceMsgCursorTrail
{
  uint16_t     length;
  uint16_t     frequency;
}
SpiceMsgCursorTrail;

typedef struct SpiceMsgCursorInvalOne
{
  uint64_t cursor_id;
}
SpiceMsgCursorInvalOne;

// spice is missing these defines, the offical reference library incorrectly
// uses the VD defines

#define HAS_CAPABILITY(caps, caps_size, index) \
  ((index) < (caps_size * 32) && ((caps)[(index) / 32] & (1 << ((index) % 32))))

#define _SET_CAPABILITY(caps, index) \
    { (caps)[(index) / 32] |= (1 << ((index) % 32)); }

#define COMMON_CAPS_BYTES (((SPICE_COMMON_CAP_MINI_HEADER + 32) / 8) & ~3)
#define COMMON_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define MAIN_CAPS_BYTES (((SPICE_MAIN_CAP_SEAMLESS_MIGRATE + 32) / 8) & ~3)
#define MAIN_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define INPUT_CAPS_BYTES (((SPICE_INPUTS_CAP_KEY_SCANCODE + 32) / 8) & ~3)
#define INPUT_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define PLAYBACK_CAPS_BYTES (((SPICE_PLAYBACK_CAP_OPUS + 32) / 8) & ~3)
#define PLAYBACK_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define RECORD_CAPS_BYTES (((SPICE_RECORD_CAP_OPUS + 32) / 8) & ~3)
#define RECORD_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define DISPLAY_CAPS_BYTES (((SPICE_DISPLAY_CAP_CODEC_H265 + 32) / 8) & ~3)
#define DISPLAY_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#define CURSOR_CAPS_BYTES (0)
#define CUSROR_SET_CAPABILITY(caps, index) _SET_CAPABILITY(caps, index)

#pragma pack(pop)

#endif
