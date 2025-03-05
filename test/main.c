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

#include <adl/adl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <purespice.h>

bool       connectionReady = false;
bool       record = false;
int        recordChannels;
int        recordSampleRate;
double     recordVolume[2];
int16_t  * recordAudio;
int        recordAudioSize;

static void clipboard_notice(const PSDataType type)
{
  printf("clipboard_notice(type: %d)\n", type);
}

static void clipboard_data(const PSDataType type, uint8_t * buffer,
    uint32_t size)
{
  printf("clipboard_data(type: %d, buffer: %p, size: %u)\n",
      type, buffer, size);
}

static void clipboard_release(void)
{
  printf("clipboard_release\n");
}

static void clipboard_request(const PSDataType type)
{
  printf("clipboard_request\n");
}

static void playback_start(int channels, int sampleRate, PSAudioFormat format,
        uint32_t time)
{
  printf("playback_start(ch: %d, sampleRate: %d, format: %d, time: %u)\n",
      channels, sampleRate, format, time);
}

static void playback_volume(int channels, const uint16_t volume[])
{
  printf("playback_volume(ch: %d ", channels);
  for(int i = 0; i < channels; ++i)
    printf(", %d: %u", i, volume[i]);
  puts(")");
}

static void playback_mute(bool mute)
{
  printf("playback_mute(%d)\n", mute);
}

static void playback_stop(void)
{
  printf("playback_stop\n");
}

static void playback_data(uint8_t * data, size_t size)
{
  printf("playback_data(%p, %lu)\n", data, size);
}

static void genSine()
{
  if (recordAudio)
    free(recordAudio);

  #define FREQ 200

  recordAudioSize = recordSampleRate * sizeof(*recordAudio) * recordChannels;
  recordAudio = malloc(recordAudioSize);

  const double delta = 2.0 * M_PI * FREQ/(double)recordSampleRate;
  double acc = 0.0;
  for(int i = 0; i < recordSampleRate; ++i, acc += delta)
  {
    for(int c = 0; c < recordChannels; ++c)
    {
      double v = recordVolume[c] * sin(acc) * 32768.0;
      if (v < -32768) v = 32768;
      else if (v > 32767) v = 32767;
      recordAudio[i * recordChannels + c] = (int16_t)v;
    }
  }
}

static void record_start(int channels, int sampleRate, PSAudioFormat format)
{
  printf("record_start(ch: %d, sampleRate: %d, format: %d)\n",
      channels, sampleRate, format);
  record           = true;
  recordChannels   = channels;
  recordSampleRate = sampleRate;
  genSine();
}

static void record_volume(int channels, const uint16_t volume[])
{
  printf("record_volume(ch: %d ", channels);
  for(int i = 0; i < channels; ++i)
  {
    printf(", %d: %u", i, volume[i]);
    recordVolume[i] = 9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787;
  }
  puts(")");
  genSine();
}

static void record_mute(bool mute)
{
  printf("record_mute(%d)\n", mute);
  record = !mute;
}

static void record_stop(void)
{
  printf("record_stop\n");
  record = false;
}

static void connection_ready(void)
{
  printf("ready\n");
  connectionReady = true;
}

FILE * fp = NULL;
int dispWidth, dispHeight;

typedef struct __attribute((packed))__
{
  uint16_t  type;             // Magic identifier: 0x4d42
  uint32_t  size;             // File size in bytes
  uint16_t  reserved1;        // Not used
  uint16_t  reserved2;        // Not used
  uint32_t  offset;           // Offset to image data in bytes from beginning of file (54 bytes)
  uint32_t  dib_header_size;  // DIB Header size in bytes (40 bytes)
  int32_t   width_px;         // Width of the image
  int32_t   height_px;        // Height of image
  uint16_t  num_planes;       // Number of color planes
  uint16_t  bits_per_pixel;   // Bits per pixel
  uint32_t  compression;      // Compression type
  uint32_t  image_size_bytes; // Image size in bytes
  int32_t   x_resolution_ppm; // Pixels per meter
  int32_t   y_resolution_ppm; // Pixels per meter
  uint32_t  num_colors;       // Number of colors
  uint32_t  important_colors; // Important colors
}
BMPHeader;


static void display_surfaceCreate(unsigned int surfaceId, PSSurfaceFormat format,
    unsigned int width, unsigned int height)
{
  printf("display_surfaceCreate(%u, %d, %u, %u)\n",
      surfaceId, format, width, height);

  dispWidth  = width;
  dispHeight = height;
  fp = fopen("/tmp/dump.bmp", "wb");
  fseek(fp, 0, SEEK_SET);

  BMPHeader h =
  {
    .type             = 0x4d42,
    .size             = sizeof(BMPHeader) + height * width * 4,
    .offset           = sizeof(BMPHeader),
    .dib_header_size  = 40,
    .width_px         = width,
    .height_px        = height,
    .num_planes       = 1,
    .bits_per_pixel   = 32,
    .image_size_bytes = height * width * 4,
    .x_resolution_ppm = 0,
    .y_resolution_ppm = 0,
  };

  fwrite(&h, sizeof(h), 1, fp);
}

static void display_surfaceDestroy(unsigned int surfaceId)
{
  printf("display_surfaceDestroy(%u)\n", surfaceId);

  if (fp)
  {
    fclose(fp);
    fp = NULL;
  }
}

static void display_drawFill(unsigned int surfaceId,
    int x, int y,
    int width, int height,
    uint32_t color)
{
  printf("display_drawFill(%d, %d, %d, %d, 0x%08x)\n",
      x, y,
      width, height,
      color);
}

static void display_drawBitmap(unsigned int surfaceId,
    PSBitmapFormat format,
    bool topDown,
    int x, int y,
    int width, int height,
    int stride,
    void * data)
{
  if (topDown)
  {
    uint8_t * src = (uint8_t *)data;
    for(int i = 0; i < height; ++i)
    {
      int dst = (dispWidth * 4 * (dispHeight-(y+i))) + x * 4;
      fseek(fp, sizeof(BMPHeader) + dst, SEEK_SET);
      fwrite(src, stride, 1, fp);
      src += stride;
    }
  }
  else
  {
    uint8_t * src = (uint8_t *)data + height * stride;
    for(int i = 0; i < height; ++i)
    {
      int dst = (dispWidth * 4 * (dispHeight-(y+i))) + x * 4;
      fseek(fp, sizeof(BMPHeader) + dst, SEEK_SET);
      src -= stride;
      fwrite(src, stride, 1, fp);
    }
  }
  fflush(fp);
}

int main(int argc, char * argv[])
{
  char * host;
  int port = 5900;
#if 0
  if (argc < 2)
  {
    printf("Usage: %s host [port]\n", argv[0]);
    return -1;
  }

  host = argv[1];
  if (argc > 2)
    port = atoi(argv[2]);
#endif
  host = "/opt/PVM/vms/Windows/windows.sock";
  port = 0;

  int retval = 0;

  if (adlInitialize() != ADL_OK)
  {
    retval = -1;
    goto err_exit;
  }

  {
    int count;
    adlGetPlatformList(&count, NULL);

    const char * platforms[count];
    adlGetPlatformList(&count, platforms);

    if (adlUsePlatform(platforms[0]) != ADL_OK)
    {
      retval = -1;
      goto err_exit;
    }
  }

  const PSConfig config =
  {
    .host      = host,
    .port      = port,
    .password  = "",
    .ready     = connection_ready,
    .inputs    =
    {
      .enable      = true,
      .autoConnect = true
    },
    .clipboard =
    {
      .enable  = true,
      .notice  = clipboard_notice,
      .data    = clipboard_data,
      .release = clipboard_release,
      .request = clipboard_request
    },
    .playback =
    {
      .enable      = true,
      .autoConnect = true,
      .start       = playback_start,
      .volume      = playback_volume,
      .mute        = playback_mute,
      .stop        = playback_stop,
      .data        = playback_data
    },
    .record = {
      .enable      = true,
      .autoConnect = true,
      .start       = record_start,
      .mute        = record_mute,
      .volume      = record_volume,
      .stop        = record_stop
    },
    .display = {
      .enable         = true,
      .autoConnect    = false,
      .surfaceCreate  = display_surfaceCreate,
      .surfaceDestroy = display_surfaceDestroy,
      .drawFill       = display_drawFill,
      .drawBitmap     = display_drawBitmap
    }
  };

  if (!purespice_connect(&config))
  {
    printf("spice connect failed\n");
    retval = -1;
    goto err_exit;
  }

  /* wait for purespice to be ready */
  while(!connectionReady)
    if (purespice_process(1) != PS_STATUS_RUN)
    {
      retval = -1;
      goto err_exit;
    }

  /* Create the parent window */
  ADLWindowDef winDef =
  {
    .title       = "PureSpice Test",
    .className   = "purespice-test",
    .type        = ADL_WINDOW_TYPE_DIALOG,
    .flags       = 0,
    .borderless  = false,
    .x           = 0  , .y = 0  ,
    .w           = 200, .h = 200
  };
  ADLWindow * parent;
  if (adlWindowCreate(winDef, &parent) != ADL_OK)
  {
    retval = -1;
    goto err_shutdown;
  }

  /* show the windows */
  adlWindowShow(parent);
  adlFlush();

  /* Process events */
  ADLEvent event;
  ADL_STATUS status;
  while((status = adlProcessEvent(1, &event)) == ADL_OK)
  {
    switch(event.type)
    {
      case ADL_EVENT_NONE:
        if (purespice_process(1) != PS_STATUS_RUN)
          goto err_shutdown;

        if (record)
          purespice_writeAudio((uint8_t*)recordAudio, recordAudioSize, 0);
        continue;

      case ADL_EVENT_CLOSE:
      case ADL_EVENT_QUIT:
        goto exit;

      case ADL_EVENT_KEY_DOWN:
        if (purespice_channelConnected(PS_CHANNEL_DISPLAY))
        {
          printf("Disconnect display\n");
          purespice_disconnectChannel(PS_CHANNEL_DISPLAY);
        }
        else
        {
          printf("Connect display\n");
          purespice_connectChannel(PS_CHANNEL_DISPLAY);
        }
        break;

      case ADL_EVENT_KEY_UP:
        break;

      case ADL_EVENT_MOUSE_DOWN:
        break;

      case ADL_EVENT_MOUSE_UP:
        break;

      case ADL_EVENT_MOUSE_MOVE:
        purespice_mouseMotion(event.u.mouse.relX, event.u.mouse.relY);
        break;

      default:
        break;
    }
  }

exit:
  purespice_disconnect();

err_shutdown:
  adlShutdown();
err_exit:
  return retval;
}
