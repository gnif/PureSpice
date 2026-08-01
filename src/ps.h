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

#ifndef _H_I_PURESPICE_
#define _H_I_PURESPICE_

#include "purespice.h"

#include "locking.h"
#include "messages.h"

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

/* Raw display bitmaps can be large, especially at 8K. Keep enough headroom for
 * those while preventing a wire-provided length from requesting an effectively
 * unbounded allocation. */
#define SPICE_MAX_MESSAGE_SIZE (256U * 1024U * 1024U)

static inline void * spice_initPacket(void * packet, uint16_t type,
    size_t dataSize, size_t extraData)
{
  ssize_t * size = packet;
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(size + 1);
  *size        = sizeof(*header) + dataSize;
  header->type = type;
  header->size = dataSize + extraData;
  return header + 1;
}

#define SPICE_RAW_PACKET(htype, dataSize, extraData) \
({ \
  const size_t packetDataSize = (dataSize); \
  void * packet = alloca(sizeof(ssize_t) + \
      sizeof(SpiceMiniDataHeader) + packetDataSize); \
  spice_initPacket(packet, (htype), packetDataSize, (extraData)); \
})

#define SPICE_RAW_PACKET_MALLOC(htype, dataSize, extraData) \
({ \
  const size_t packetDataSize = (dataSize); \
  const size_t packetOverhead = sizeof(ssize_t) + \
    sizeof(SpiceMiniDataHeader); \
  void * packet = packetDataSize > SIZE_MAX - packetOverhead ? NULL : \
    malloc(packetOverhead + packetDataSize); \
  packet ? spice_initPacket( \
      packet, (htype), packetDataSize, (extraData)) : NULL; \
})

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
  const ssize_t wrote = channel_writeNL((channel), header, *sz); \
  SPICE_UNLOCK((channel)->lock); \
  wrote == *sz; \
})

#define SPICE_SEND_PACKET_NL(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  const ssize_t wrote = channel_writeNL((channel), header, *sz); \
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

typedef struct PS        PS;
typedef struct PSChannel PSChannel;

typedef PS_STATUS (*PSHandlerFn)(PSChannel * channel);
#define PS_HANDLER_DISCARD (PSHandlerFn)( 0)
#define PS_HANDLER_ERROR   (PSHandlerFn)(-1)

// internal structures
struct PSChannel
{
  uint8_t      spiceType;
  const char * name;
  atomic_bool  available;
  bool       * enable;
  bool       * autoConnect;

  SpiceMiniDataHeader header;
  unsigned int headerRead;
  PSHandlerFn  handlerFn;
  uint8_t    * buffer;
  unsigned int bufferSize;
  unsigned int bufferRead;
  bool         discarding;
  unsigned int discardSize;

  const SpiceLinkHeader * (*getConnectPacket)(void);
  void (*setCaps)(
      const uint32_t * common , int numCommon,
      const uint32_t * channel, int numChannel);
  PS_STATUS   (*onConnect)(PSChannel * channel);
  PSHandlerFn (*onMessage)(PSChannel * channel);

  atomic_bool connected;
  atomic_bool ready;
  atomic_bool doDisconnect;
  atomic_bool initDone;
  atomic_bool connecting;
  atomic_int  socket;
  uint32_t    ackFrequency;
  uint32_t    ackCount;
  atomic_flag lock;
};

struct PSCursorImage
{
  struct PSCursorImage * next;
  bool                   cached;
  SpiceCursorHeader      header;
  uint8_t                buffer[];
};

struct PS
{
  bool     initialized;
  atomic_bool processing;
  atomic_bool disconnectPending;
  atomic_bool disconnectWasConnected;
  PSInit   init;
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
  char   * guestName;
  uint8_t  guestUUID[16];

  atomic_bool connected;
  int    epollfd;
  PSChannel channels[PS_CHANNEL_MAX];
  bool   channelsReady;

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

  struct
  {
    int16_t                 x, y;
    uint16_t                trailLen, trailFreq;
    bool                    visible;
    struct PSCursorImage  * cache;
    struct PSCursorImage ** cacheLast;
    struct PSCursorImage  * current;
  }
  cursor;

  uint8_t * motionBuffer;
  size_t    motionBufferSize;
};

extern PS g_ps;

PS_STATUS ps_connectChannel(PSChannel * ch);

PS_STATUS purespice_onCommonRead(PSChannel * channel,
    SpiceMiniDataHeader * header, int * dataAvailable);

#endif
