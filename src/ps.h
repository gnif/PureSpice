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

#ifndef _H_I_PURESPICE_
#define _H_I_PURESPICE_

#include "purespice.h"

#include "locking.h"

#include <stdbool.h>
#include <stdint.h>
#include <alloca.h>

#include <sys/un.h>
#include <arpa/inet.h>
#include <spice/protocol.h>

// we don't really need flow control because we are all local
// instead do what the spice-gtk library does and provide the largest
// possible number
#define SPICE_AGENT_TOKENS_MAX ~0

#define _SPICE_RAW_PACKET(htype, dataSize, extraData, _alloc) \
({ \
  uint8_t * packet = _alloc(sizeof(ssize_t) + \
      sizeof(SpiceMiniDataHeader) + dataSize); \
  ssize_t * sz = (ssize_t*)packet; \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(sz + 1); \
  *sz          = sizeof(SpiceMiniDataHeader) + dataSize; \
  header->type = (htype); \
  header->size = dataSize + extraData; \
  (header + 1); \
})

#define SPICE_RAW_PACKET(htype, dataSize, extraData) \
  _SPICE_RAW_PACKET(htype, dataSize, extraData, alloca)

#define SPICE_RAW_PACKET_MALLOC(htype, dataSize, extraData) \
  _SPICE_RAW_PACKET(htype, dataSize, extraData, malloc)

#define SPICE_RAW_PACKET_FREE(packet) \
{ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  free(sz); \
}

#define SPICE_SET_PACKET_SIZE(packet, sz) \
{ \
   SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
   header->size = sz; \
}

#define SPICE_PACKET(htype, payloadType, extraData) \
  ((payloadType *)SPICE_RAW_PACKET(htype, sizeof(payloadType), extraData))

#define SPICE_PACKET_MALLOC(htype, payloadType, extraData) \
  ((payloadType *)SPICE_RAW_PACKET_MALLOC(htype, sizeof(payloadType), extraData))

#define SPICE_SEND_PACKET(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  SPICE_LOCK((channel)->lock); \
  const ssize_t wrote = send((channel)->socket, header, *sz, 0); \
  SPICE_UNLOCK((channel)->lock); \
  wrote == *sz; \
})

#define SPICE_SEND_PACKET_NL(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  const ssize_t wrote = send((channel)->socket, header, *sz, 0); \
  wrote == *sz; \
})

// currently (2020) these defines are not yet availble for most distros, so we
// just define them ourselfs for now
#define _SPICE_MOUSE_BUTTON_SIDE        6
#define _SPICE_MOUSE_BUTTON_EXTRA       7
#define _SPICE_MOUSE_BUTTON_MASK_SIDE  (1 << 5)
#define _SPICE_MOUSE_BUTTON_MASK_EXTRA (1 << 6)

typedef enum
{
  PS_STATUS_OK,
  PS_STATUS_HANDLED,
  PS_STATUS_NODATA,
  PS_STATUS_ERROR
}
PS_STATUS;

// internal structures
struct PSChannel
{
  bool        connected;
  bool        ready;
  bool        initDone;
  uint8_t     channelType;
  int         socket;
  uint32_t    ackFrequency;
  uint32_t    ackCount;
  atomic_flag lock;

  PS_STATUS (*read)(int * dataAvailable);
};

struct PS
{
  PSConfig config;

  short        family;
  union
  {
    struct sockaddr     addr;
    struct sockaddr_in  in;
    struct sockaddr_in6 in6;
    struct sockaddr_un  un;
  }
  addr;

  uint32_t sessionID;
  uint32_t channelID;

  int      epollfd;
  struct   PSChannel scMain;
  struct   PSChannel scInputs;
  struct   PSChannel scPlayback;

  struct
  {
    uint32_t modifiers;
  }
  kb;

  struct
  {
    atomic_flag lock;
    uint32_t buttonState;

    atomic_int sentCount;
    int rpos, wpos;
  }
  mouse;

  uint8_t * motionBuffer;
  size_t    motionBufferSize;
};

extern struct PS g_ps;

PS_STATUS purespice_onCommonRead(struct PSChannel * channel,
    SpiceMiniDataHeader * header, int * dataAvailable);

#endif
