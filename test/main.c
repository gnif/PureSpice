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

#include <spice/spice.h>

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


  printf("attempting to connect to %s:%d...", host, port);
  fflush(stdout);
  if (!spice_connect(host, port, ""))
  {
    printf("spice connect failed\n");
    retval = -1;
    goto err_exit;
  }
  printf("done.\n");

  printf("waiting for comms setup...");
  fflush(stdout);
  while(!spice_ready())
    if (!spice_process(1))
    {
      printf("fail\n");
      retval = -1;
      goto err_exit;
    }
  printf("done.\n");

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
        spice_process(1);
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
        spice_mouse_motion(event.u.mouse.relX, event.u.mouse.relY);
        break;

      default:
        break;
    }
  }

exit:
  printf("shutdown...");
  fflush(stdout);
  spice_disconnect();
  while(spice_process(1)) {}
  printf("done.\n");

err_shutdown:
  adlShutdown();
err_exit:
  return retval;
}
