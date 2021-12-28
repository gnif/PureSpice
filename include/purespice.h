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
#ifndef PURE_SPICE_H__
#define PURE_SPICE_H__

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum PSDataType
{
  SPICE_DATA_TEXT,
  SPICE_DATA_PNG,
  SPICE_DATA_BMP,
  SPICE_DATA_TIFF,
  SPICE_DATA_JPEG,

  SPICE_DATA_NONE
}
PSDataType;

typedef enum PSAudioFormat
{
  PS_AUDIO_FMT_INVALID,
  PS_AUDIO_FMT_S16
}
PSAudioFormat;

typedef void (*PSClipboardNotice )(const PSDataType type);
typedef void (*PSClipboardData   )(const PSDataType type,
    uint8_t * buffer, uint32_t size);
typedef void (*PSClipboardRelease)();
typedef void (*PSClipboardRequest)(const PSDataType type);


#ifdef __cplusplus
extern "C" {
#endif

bool purespice_connect(const char * host, const unsigned short port,
    const char * password, bool playback);
void purespice_disconnect();
bool purespice_process(int timeout);
bool purespice_ready();

bool purespice_keyDown      (uint32_t code);
bool purespice_keyUp        (uint32_t code);
bool purespice_keyModifiers (uint32_t modifiers);
bool purespice_mouseMode    (bool     server);
bool purespice_mousePosition(uint32_t x, uint32_t y);
bool purespice_mouseMotion  ( int32_t x,  int32_t y);
bool purespice_mousePress   (uint32_t button);
bool purespice_mouseRelease (uint32_t button);

bool purespice_clipboardRequest(PSDataType type);
bool purespice_clipboardGrab(PSDataType types[], int count);
bool purespice_clipboardRelease();

bool purespice_clipboardDataStart(PSDataType type, size_t size);
bool purespice_clipboardData(PSDataType type, uint8_t * data, size_t size);

/* events */
bool purespice_setClipboardCb(
    PSClipboardNotice  cbNoticeFn,
    PSClipboardData    cbDataFn,
    PSClipboardRelease cbReleaseFn,
    PSClipboardRequest cbRequestFn);

bool purespice_setAudioCb(
  void (*start)(int channels, int sampleRate, PSAudioFormat format,
    uint32_t time),
  void (*volume)(int channels, const uint16_t volume[]),
  void (*mute)(bool mute),
  void (*stop)(void),
  void (*data)(uint8_t * data, size_t size)
);

#ifdef __cplusplus
}
#endif

#endif /* PURE_SPICE_H__ */
