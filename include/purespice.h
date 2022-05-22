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

typedef enum PSStatus
{
  PS_STATUS_RUN,
  PS_STATUS_SHUTDOWN,
  PS_STATUS_ERR_POLL,
  PS_STATUS_ERR_READ,
  PS_STATUS_ERR_ACK
}
PSStatus;

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

typedef struct PSServerInfo
{
  char  * name;
  uint8_t uuid[16];
}
PSServerInfo;

typedef enum PSChannelType
{
  PS_CHANNEL_MAIN,
  PS_CHANNEL_INPUTS,
  PS_CHANNEL_PLAYBACK,
  PS_CHANNEL_RECORD,
  PS_CHANNEL_DISPLAY,

  PS_CHANNEL_MAX
}
PSChannelType;

typedef enum PSSurfaceFormat
{
  PS_SURFACE_FMT_1_A,
  PS_SURFACE_FMT_8_A,
  PS_SURFACE_FMT_16_555,
  PS_SURFACE_FMT_32_xRGB,
  PS_SURFACE_FMT_16_565,
  PS_SURFACE_FMT_32_ARGB
}
PSSurfaceFormat;

typedef enum PSBitmapFormat
{
  PS_BITMAP_FMT_1BIT_LE,
  PS_BITMAP_FMT_1BIT_BE,
  PS_BITMAP_FMT_4BIT_LE,
  PS_BITMAP_FMT_4BIT_BE,
  PS_BITMAP_FMT_8BIT,
  PS_BITMAP_FMT_16BIT,
  PS_BITMAP_FMT_24BIT,
  PS_BITMAP_FMT_32BIT,
  PS_BITMAP_FMT_RGBA,
  PS_BITMAP_FMT_8BIT_A
}
PSBitmapFormat;

typedef enum PSRopd
{
  PS_ROPD_INVERS_SRC,
  PS_ROPD_INVERS_BRUSH,
  PS_ROPD_INVERS_DEST,
  PS_ROPD_OP_PUT,
  PS_ROPD_OP_OR,
  PS_RPOD_OP_AND,
  PS_ROPD_OP_XOR,
  PS_ROPD_OP_BLACKNESS,
  PS_ROPD_OP_WHITENESS,
  PS_ROPD_OP_INVERS,
  PS_ROPD_INVERS_RES
}
PSRopd;

typedef struct PSInit
{
  struct
  {
    void (*info)(const char * file, unsigned int line, const char * function,
        const char * format, ...) __attribute__((format (printf, 4, 5)));

    void (*warn)(const char * file, unsigned int line, const char * function,
        const char * format, ...) __attribute__((format (printf, 4, 5)));

    void (*error)(const char * file, unsigned int line, const char * function,
        const char * format, ...) __attribute__((format (printf, 4, 5)));
  }
  log;
}
PSInit;

typedef struct PSConfig
{
  const char * host;
  unsigned     port;
  const char * password;

  /* [optional] called once the connection is ready (all channels connected) */
  void (*ready)(void);

  struct
  {
    /* enable input support if available */
    bool enable;

    /* automatically connect to the channel as soon as it's available */
    bool autoConnect;
  }
  inputs;

  struct
  {
    /* enable clipboard support if available */
    bool enable;

    /* called with the data type available by the agent */
    void (*notice)(const PSDataType type);

    /* called with the clipboard data */
    void (*data)(const PSDataType type, uint8_t * buffer, uint32_t size);

    /* called to notify that there is no longer any clipboard data available */
    void (*release)(void);

    /* called to request clipboard data of the specified type */
    void (*request)(const PSDataType type);
  }
  clipboard;

  struct
  {
    /* enable the playback channel if available */
    bool enable;

    /* automatically connect to the channel as soon as it's available */
    bool autoConnect;

    /* called with the details of the stream to open */
    void (*start)(int channels, int sampleRate, PSAudioFormat format,
        uint32_t time);

    /* [optional] called with the volume of each channel to set */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to mute/unmute the stream */
    void (*mute)(bool mute);

    /* called when the guest stops the audio stream */
    void (*stop)(void);

    /* called when there are audio samples */
    void (*data)(uint8_t * data, size_t size);
  }
  playback;

  struct
  {
    /* enable the playback channel if available */
    bool enable;

    /* automatically connect to the channel as soon as it's available */
    bool autoConnect;

    /* called with the details of the stream to open */
    void (*start)(int channels, int sampleRate, PSAudioFormat format);

    /* [optional] called with the volume of each channel to set */
    void (*volume)(int channels, const uint16_t volume[]);

    /* [optional] called to mute/unmute the stream */
    void (*mute)(bool mute);

    /* called when the guest stops the audio stream */
    void (*stop)(void);
  }
  record;

  struct
  {
    /* enable the display channel if available */
    bool enable;

    /* automatically connect to the channel as soon as it's available */
    bool autoConnect;

    /* called to create a new surface */
    void (*surfaceCreate)(unsigned int surfaceId, PSSurfaceFormat format,
        unsigned int width, unsigned int height);

    /* called to destroy a surface */
    void (*surfaceDestroy)(unsigned int surfaceId);

    /* called to draw a bitmap to a surface */
    void (*drawBitmap)(unsigned int surfaceId,
        PSBitmapFormat format,
        bool topDown,
        int x    , int y,
        int width, int height,
        int stride,
        void * data);

    /* called to fill an area with a color */
    void (*drawFill)(unsigned int surfaceId,
        int x    , int y,
        int width, int height,
        uint32_t color);
  }
  display;
}
PSConfig;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the library for use, this may be called before any other methods
 * to setup the library. If not initialization will be automatically performed
 * by `purespice_connect` with default parameters.
 *
 * `init` is optional and may be NULL
 */
void purespice_init(const PSInit * init);

bool purespice_connect(const PSConfig * config);
void purespice_disconnect(void);
PSStatus purespice_process(int timeout);

bool purespice_getServerInfo(PSServerInfo * info);
void purespice_freeServerInfo(PSServerInfo * info);

bool purespice_hasChannel       (PSChannelType channel);
bool purespice_channelConnected (PSChannelType channel);
bool purespice_connectChannel   (PSChannelType channel);
bool purespice_disconnectChannel(PSChannelType channel);

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
bool purespice_clipboardRelease(void);

bool purespice_clipboardDataStart(PSDataType type, size_t size);
bool purespice_clipboardData(PSDataType type, uint8_t * data, size_t size);

bool purespice_writeAudio(void * data, size_t size, uint32_t time);

#ifdef __cplusplus
}
#endif

#endif /* PURE_SPICE_H__ */
