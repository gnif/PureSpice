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

#include <adl/adl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <purespice.h>

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
  puts(")\n");
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

int main(int argc, char * argv[])
{
  char * host;
  int port = 5900;

  if (argc < 2)
  {
    printf("Usage: %s host [port]\n", argv[0]);
    return -1;
  }

  host = argv[1];
  if (argc > 2)
    port = atoi(argv[2]);

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
      .enable = true,
      .start  = playback_start,
      .volume = playback_volume,
      .mute   = playback_mute,
      .stop   = playback_stop,
      .data   = playback_data
    }
  };

  if (!purespice_connect(&config))
  {
    printf("spice connect failed\n");
    retval = -1;
    goto err_exit;
  }

  /* wait for purespice to be ready */
  while(!purespice_ready())
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
        purespice_process(1);
        continue;

      case ADL_EVENT_CLOSE:
      case ADL_EVENT_QUIT:
        goto exit;

      case ADL_EVENT_KEY_DOWN:
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
  printf("shutdown...");
  fflush(stdout);
  purespice_disconnect();
  while(purespice_process(1)) {}
  printf("done.\n");

err_shutdown:
  adlShutdown();
err_exit:
  return retval;
}
