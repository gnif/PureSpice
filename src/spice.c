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

#include "spice/spice.h"

#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <spice/protocol.h>
#include <spice/vd_agent.h>

#include "messages.h"
#include "rsa.h"

#define SPICE_LOCK_INIT(x) \
  atomic_flag_clear(&(x))

#define SPICE_LOCK(x) \
  while(atomic_flag_test_and_set_explicit(&(x), memory_order_acquire)) { ; }

#define SPICE_UNLOCK(x) \
  atomic_flag_clear_explicit(&(x), memory_order_release);

// we don't really need flow control because we are all local
// instead do what the spice-gtk library does and provide the largest
// possible number
#define SPICE_AGENT_TOKENS_MAX ~0

#define SPICE_RAW_PACKET(htype, dataSize, extraData) \
({ \
  uint8_t * packet = alloca(sizeof(ssize_t) + sizeof(SpiceMiniDataHeader) + dataSize); \
  ssize_t * sz = (ssize_t*)packet; \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(sz + 1); \
  *sz          = sizeof(SpiceMiniDataHeader) + dataSize; \
  header->type = (htype); \
  header->size = dataSize + extraData; \
  (header + 1); \
})

#define SPICE_PACKET(htype, payloadType, extraData) \
  ((payloadType *)SPICE_RAW_PACKET(htype, sizeof(payloadType), extraData))

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

// ============================================================================

typedef enum
{
  SPICE_STATUS_OK,
  SPICE_STATUS_HANDLED,
  SPICE_STATUS_NODATA,
  SPICE_STATUS_ERROR
}
SPICE_STATUS;

// internal structures
struct SpiceChannel
{
  bool        connected;
  bool        ready;
  bool        initDone;
  uint8_t     channelType;
  int         socket;
  uint32_t    ackFrequency;
  uint32_t    ackCount;
  atomic_flag lock;
};

struct SpiceKeyboard
{
  uint32_t modifiers;
};

struct SpiceMouse
{
  uint32_t buttonState;

  atomic_int sentCount;
  int rpos, wpos;
};

union SpiceAddr
{
  struct sockaddr     addr;
  struct sockaddr_in  in;
  struct sockaddr_in6 in6;
  struct sockaddr_un  un;
};

struct Spice
{
  char            password[32];
  short           family;
  union SpiceAddr addr;

  bool     hasAgent;
  uint32_t serverTokens;
  uint32_t sessionID;
  uint32_t channelID;
  ssize_t  agentMsg;

  struct   SpiceChannel scMain;
  struct   SpiceChannel scInputs;

  struct SpiceKeyboard kb;
  struct SpiceMouse    mouse;

  bool cbSupported;
  bool cbSelection;

  // clipboard variables
  bool                  cbAgentGrabbed;
  bool                  cbClientGrabbed;
  SpiceDataType         cbType;
  uint8_t *             cbBuffer;
  uint32_t              cbRemain;
  uint32_t              cbSize;
  SpiceClipboardNotice  cbNoticeFn;
  SpiceClipboardData    cbDataFn;
  SpiceClipboardRelease cbReleaseFn;
  SpiceClipboardRequest cbRequestFn;

  uint8_t * motionBuffer;
  size_t    motionBufferSize;
};

// globals
struct Spice spice =
{
  .sessionID            = 0,
  .scMain  .connected   = false,
  .scMain  .channelType = SPICE_CHANNEL_MAIN,
  .scInputs.connected   = false,
  .scInputs.channelType = SPICE_CHANNEL_INPUTS,
};

// internal forward decls
SPICE_STATUS spice_connect_channel   (struct SpiceChannel * channel);
void         spice_disconnect_channel(struct SpiceChannel * channel);

bool spice_process_ack(struct SpiceChannel * channel);

SPICE_STATUS spice_on_common_read        (struct SpiceChannel * channel, SpiceMiniDataHeader * header, int * dataAvailable);
SPICE_STATUS spice_on_main_channel_read  (int * dataAvailable);
SPICE_STATUS spice_on_inputs_channel_read(int * dataAvailable);

SPICE_STATUS spice_agent_process  (uint32_t dataSize, int * dataAvailable);
SPICE_STATUS spice_agent_connect  ();
SPICE_STATUS spice_agent_send_caps(bool request);
void         spice_agent_on_clipboard();

// utility functions
static uint32_t spice_type_to_agent_type(SpiceDataType type);
static SpiceDataType agent_type_to_spice_type(uint32_t type);

// thread safe read/write methods
bool spice_agent_start_msg(uint32_t type, ssize_t size);
bool spice_agent_write_msg(const void * buffer, ssize_t size);

// non thread safe read/write methods (nl = non-locking)
SPICE_STATUS spice_read_nl   (      struct SpiceChannel * channel, void * buffer, const ssize_t size, int * dataAvailable);
SPICE_STATUS spice_discard_nl(      struct SpiceChannel * channel, ssize_t size, int * dataAvailable);
ssize_t      spice_write_nl  (const struct SpiceChannel * channel, const void * buffer, const ssize_t size);

// ============================================================================

static uint64_t get_timestamp()
{
  struct timespec time;
  const int result = clock_gettime(CLOCK_MONOTONIC, &time);
  if (result != 0)
    perror("clock_gettime failed! this should never happen!\n");
  return (uint64_t)time.tv_sec * 1000LL + time.tv_nsec / 1000000LL;
}

// ============================================================================

bool spice_connect(const char * host, const unsigned short port, const char * password)
{
  strncpy(spice.password, password, sizeof(spice.password) - 1);
  memset(&spice.addr, 0, sizeof(spice.addr));

  if (port == 0)
  {
    spice.family = AF_UNIX;
    spice.addr.un.sun_family = spice.family;
    strncpy(spice.addr.un.sun_path, host, sizeof(spice.addr.un.sun_path) - 1);
  }
  else
  {
    spice.family = AF_INET;
    inet_pton(spice.family, host, &spice.addr.in.sin_addr);
    spice.addr.in.sin_family = spice.family;
    spice.addr.in.sin_port   = htons(port);
  }

  spice.channelID = 0;
  if (spice_connect_channel(&spice.scMain) != SPICE_STATUS_OK)
    return false;

  return true;
}

// ============================================================================

void spice_disconnect()
{
  spice_disconnect_channel(&spice.scInputs);
  spice_disconnect_channel(&spice.scMain  );

  if (spice.motionBuffer)
  {
    free(spice.motionBuffer);
    spice.motionBuffer = NULL;
  }
}

// ============================================================================

bool spice_ready()
{
  return spice.scMain.connected &&
         spice.scInputs.connected;
}

// ============================================================================

bool spice_process(int timeout)
{
  int fds = 0;
  fd_set readSet;
  FD_ZERO(&readSet);

  bool mainConnected   = false;
  bool inputsConnected = false;

  if (spice.scMain.connected)
  {
    mainConnected = true;
    FD_SET(spice.scMain.socket, &readSet);
    if (spice.scMain.socket > fds)
      fds = spice.scMain.socket;
  }

  if (spice.scInputs.connected)
  {
    inputsConnected = true;
    FD_SET(spice.scInputs.socket, &readSet);
    if (spice.scInputs.socket > fds)
      fds = spice.scInputs.socket;
  }

  struct timeval tv;
  tv.tv_sec  = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;

  int rc = select(fds + 1, &readSet, NULL, NULL, &tv);
  if (rc == 0)
    return true;

  if (rc < 0)
    return false;

  if (FD_ISSET(spice.scInputs.socket, &readSet))
  {
    // note: dataAvailable can go negative due to blocking reads
    int dataAvailable;
    ioctl(spice.scInputs.socket, FIONREAD, &dataAvailable);

    // if there is no data then the socket is closed
    if (!dataAvailable)
      spice.scInputs.connected = false;

    // process as much data as possible
    while(dataAvailable > 0)
    {
      switch(spice_on_inputs_channel_read(&dataAvailable))
      {
        case SPICE_STATUS_OK:
        case SPICE_STATUS_HANDLED:
          // if dataAvailable has gone negative then refresh it
          if (dataAvailable < 0)
            ioctl(spice.scInputs.socket, FIONREAD, &dataAvailable);
          break;

        case SPICE_STATUS_NODATA:
          spice.scInputs.connected = false;
          dataAvailable = 0;
          break;

        case SPICE_STATUS_ERROR:
          return false;
      }

      if (!spice_process_ack(&spice.scInputs))
        return false;
    }
  }

  if (FD_ISSET(spice.scMain.socket, &readSet))
  {
    // note: dataAvailable can go negative due to blocking reads
    int dataAvailable;
    ioctl(spice.scMain.socket, FIONREAD, &dataAvailable);

    // if there is no data then the socket is closed
    if (!dataAvailable)
      spice.scMain.connected = false;

    // process as much data as possible
    while(dataAvailable > 0)
    {
      switch(spice_on_main_channel_read(&dataAvailable))
      {
        case SPICE_STATUS_OK:
        case SPICE_STATUS_HANDLED:
          // if dataAvailable has gone negative then refresh it
          if (dataAvailable < 0)
            ioctl(spice.scInputs.socket, FIONREAD, &dataAvailable);
          break;

        case SPICE_STATUS_NODATA:
          spice.scMain.connected = false;
          dataAvailable = 0;
          break;

        default:
          return false;
      }

      if (!spice_process_ack(&spice.scMain))
        return false;
    }
  }

  if (spice.scMain.connected || spice.scInputs.connected)
    return true;

  /* shutdown */
  spice.sessionID = 0;
  if (spice.cbBuffer)
  {
    free(spice.cbBuffer);
    spice.cbBuffer = NULL;
  }

  spice.cbRemain = 0;
  spice.cbSize   = 0;

  spice.cbAgentGrabbed  = false;
  spice.cbClientGrabbed = false;

  if (inputsConnected)
    close(spice.scInputs.socket);

  if (mainConnected)
    close(spice.scMain.socket);

  return false;
}

// ============================================================================

bool spice_process_ack(struct SpiceChannel * channel)
{
  if (channel->ackFrequency == 0)
    return true;

  if (channel->ackCount++ != channel->ackFrequency)
    return true;

  channel->ackCount = 0;

  char * ack = SPICE_PACKET(SPICE_MSGC_ACK, char, 0);
  *ack = 0;
  return SPICE_SEND_PACKET(channel, ack);
}

// ============================================================================

SPICE_STATUS spice_on_common_read(struct SpiceChannel * channel, SpiceMiniDataHeader * header, int * dataAvailable)
{
  SPICE_STATUS status;
  if ((status = spice_read_nl(channel, header, sizeof(SpiceMiniDataHeader), dataAvailable)) != SPICE_STATUS_OK)
    return status;

  if (!channel->connected)
    return SPICE_STATUS_HANDLED;

  if (!channel->initDone)
    return SPICE_STATUS_OK;

  switch(header->type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
      return SPICE_STATUS_HANDLED;

    case SPICE_MSG_SET_ACK:
    {
      SpiceMsgSetAck in;
      if ((status = spice_read_nl(channel, &in, sizeof(in), dataAvailable)) != SPICE_STATUS_OK)
        return status;

      channel->ackFrequency = in.window;

      SpiceMsgcAckSync * out =
        SPICE_PACKET(SPICE_MSGC_ACK_SYNC, SpiceMsgcAckSync, 0);

      out->generation = in.generation;
      return SPICE_SEND_PACKET(channel, out) ?
        SPICE_STATUS_HANDLED : SPICE_STATUS_ERROR;
    }

    case SPICE_MSG_PING:
    {
      SpiceMsgPing in;
      if ((status = spice_read_nl(channel, &in, sizeof(in), dataAvailable)) != SPICE_STATUS_OK)
        return status;

      const int discard = header->size - sizeof(in);
      if ((status = spice_discard_nl(channel, discard, dataAvailable)) != SPICE_STATUS_OK)
        return status;

      SpiceMsgcPong * out =
        SPICE_PACKET(SPICE_MSGC_PONG, SpiceMsgcPong, 0);

      out->id        = in.id;
      out->timestamp = in.timestamp;
      return SPICE_SEND_PACKET(channel, out) ?
        SPICE_STATUS_HANDLED : SPICE_STATUS_ERROR;
    }

    case SPICE_MSG_WAIT_FOR_CHANNELS:
      return SPICE_STATUS_HANDLED;

    case SPICE_MSG_DISCONNECTING:
    {
      shutdown(channel->socket, SHUT_WR);
      return SPICE_STATUS_HANDLED;
    }

    case SPICE_MSG_NOTIFY:
    {
      SpiceMsgNotify in;
      if ((status = spice_read_nl(channel, &in, sizeof(in), dataAvailable)) != SPICE_STATUS_OK)
        return status;

      char msg[in.message_len + 1];
      if ((status = spice_read_nl(channel, msg, in.message_len + 1, dataAvailable)) != SPICE_STATUS_OK)
        return status;
      *dataAvailable -= in.message_len + 1;

      return SPICE_STATUS_HANDLED;
    }
  }

  return SPICE_STATUS_OK;
}

// ============================================================================

SPICE_STATUS spice_on_main_channel_read(int * dataAvailable)
{
  struct SpiceChannel *channel = &spice.scMain;

  SpiceMiniDataHeader header;

  SPICE_STATUS status;
  if ((status = spice_on_common_read(channel, &header, dataAvailable)) != SPICE_STATUS_OK)
    return status;

  if (!channel->initDone)
  {
    if (header.type != SPICE_MSG_MAIN_INIT)
    {
      spice_disconnect();
      return SPICE_STATUS_ERROR;
    }

    channel->initDone = true;
    SpiceMsgMainInit msg;
    if ((status = spice_read_nl(channel, &msg, sizeof(msg), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    spice.sessionID = msg.session_id;

    spice.serverTokens = msg.agent_tokens;
    spice.hasAgent     = msg.agent_connected;
    if (spice.hasAgent && (status = spice_agent_connect()) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT && !spice_mouse_mode(false))
      return SPICE_STATUS_ERROR;

    void * packet = SPICE_RAW_PACKET(SPICE_MSGC_MAIN_ATTACH_CHANNELS, 0, 0);
    if (!SPICE_SEND_PACKET(channel, packet))
    {
      spice_disconnect();
      return SPICE_STATUS_ERROR;
    }

    return SPICE_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_CHANNELS_LIST)
  {
    SpiceMainChannelsList msg;
    if ((status = spice_read_nl(channel, &msg, sizeof(msg), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    // documentation doesn't state that the array is null terminated but it seems that it is
    SpiceChannelID channels[msg.num_of_channels];
    if ((status = spice_read_nl(channel, &channels, msg.num_of_channels * sizeof(SpiceChannelID), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    for(int i = 0; i < msg.num_of_channels; ++i)
    {
      if (channels[i].type == SPICE_CHANNEL_INPUTS)
      {
        if (spice.scInputs.connected)
        {
          spice_disconnect();
          return SPICE_STATUS_ERROR;
        }

        if ((status = spice_connect_channel(&spice.scInputs)) != SPICE_STATUS_OK)
        {
          spice_disconnect();
          return status;
        }
      }
    }

    return SPICE_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED)
  {
    spice.hasAgent = true;
    if ((status = spice_agent_connect()) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }
    return SPICE_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS)
  {
    uint32_t num_tokens;
    if ((status = spice_read_nl(channel, &num_tokens, sizeof(num_tokens), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    spice.hasAgent    = true;
    spice.serverTokens = num_tokens;
    if ((status = spice_agent_connect()) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }
    return SPICE_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DISCONNECTED)
  {
    uint32_t error;
    if ((status = spice_read_nl(channel, &error, sizeof(error), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    spice.hasAgent = false;

    if (spice.cbBuffer)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbSize   = 0;
      spice.cbRemain = 0;
    }

    return SPICE_STATUS_OK;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DATA)
  {
    if (!spice.hasAgent)
      return spice_discard_nl(channel, header.size, dataAvailable);

    if ((status = spice_agent_process(header.size, dataAvailable)) != SPICE_STATUS_OK)
      spice_disconnect();

    return status;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_TOKEN)
  {
    uint32_t num_tokens;
    if ((status = spice_read_nl(channel, &num_tokens, sizeof(num_tokens), dataAvailable)) != SPICE_STATUS_OK)
    {
      spice_disconnect();
      return status;
    }

    spice.serverTokens = num_tokens;
    return SPICE_STATUS_OK;
  }

  return spice_discard_nl(channel, header.size, dataAvailable);
}

// ============================================================================

SPICE_STATUS spice_on_inputs_channel_read(int * dataAvailable)
{
  struct SpiceChannel *channel = &spice.scInputs;

  SpiceMiniDataHeader header;

  SPICE_STATUS status;
  if ((status = spice_on_common_read(channel, &header, dataAvailable)) != SPICE_STATUS_OK)
    return status;

  switch(header.type)
  {
    case SPICE_MSG_INPUTS_INIT:
    {
      if (channel->initDone)
        return SPICE_STATUS_ERROR;

      channel->initDone = true;

      SpiceMsgInputsInit in;
      if ((status = spice_read_nl(channel, &in, sizeof(in), dataAvailable)) != SPICE_STATUS_OK)
        return status;

      return SPICE_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      SpiceMsgInputsInit in;
      if ((status = spice_read_nl(channel, &in, sizeof(in), dataAvailable)) != SPICE_STATUS_OK)
        return status;

      spice.kb.modifiers = in.modifiers;
      return SPICE_STATUS_OK;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      const int count = atomic_fetch_sub(&spice.mouse.sentCount,
          SPICE_INPUT_MOTION_ACK_BUNCH);
      return (count >= SPICE_INPUT_MOTION_ACK_BUNCH) ?
        SPICE_STATUS_OK : SPICE_STATUS_ERROR;
    }
  }

  return spice_discard_nl(channel, header.size, dataAvailable);
}

// ============================================================================

SPICE_STATUS spice_connect_channel(struct SpiceChannel * channel)
{
  SPICE_STATUS status;

  channel->initDone     = false;
  channel->ackFrequency = 0;
  channel->ackCount     = 0;

  SPICE_LOCK_INIT(channel->lock);

  size_t addrSize;
  switch(spice.family)
  {
    case AF_UNIX:
      addrSize = sizeof(spice.addr.un);
      break;

    case AF_INET:
      addrSize = sizeof(spice.addr.in);
      break;

    case AF_INET6:
      addrSize = sizeof(spice.addr.in6);
      break;

    default:
      return SPICE_STATUS_ERROR;
  }

  channel->socket = socket(spice.family, SOCK_STREAM, 0);
  if (channel->socket == -1)
    return SPICE_STATUS_ERROR;

  if (spice.family != AF_UNIX)
  {
    const int flag = 1;
    setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY , &flag, sizeof(int));
    setsockopt(channel->socket, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(int));
  }

  if (connect(channel->socket, &spice.addr.addr, addrSize) == -1)
  {
    close(channel->socket);
    return SPICE_STATUS_ERROR;
  }

  channel->connected = true;

  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES / sizeof(uint32_t)];
    uint32_t        channelCaps[MAIN_CAPS_BYTES   / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .connection_id    = spice.sessionID,
      .channel_type     = channel->channelType,
      .channel_id       = spice.channelID,
      .num_common_caps  = COMMON_CAPS_BYTES / sizeof(uint32_t),
      .num_channel_caps = MAIN_CAPS_BYTES   / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

  if (channel == &spice.scMain)
    MAIN_SET_CAPABILITY(p.channelCaps, SPICE_MAIN_CAP_AGENT_CONNECTED_TOKENS);

  if (spice_write_nl(channel, &p, sizeof(p)) != sizeof(p))
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  if ((status = spice_read_nl(channel, &p.header, sizeof(p.header), NULL)) != SPICE_STATUS_OK)
  {
    spice_disconnect_channel(channel);
    return status;
  }

  if (p.header.magic         != SPICE_MAGIC ||
      p.header.major_version != SPICE_VERSION_MAJOR)
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  if (p.header.size < sizeof(SpiceLinkReply))
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  SpiceLinkReply reply;
  if ((status = spice_read_nl(channel, &reply, sizeof(reply), NULL)) != SPICE_STATUS_OK)
  {
    spice_disconnect_channel(channel);
    return status;
  }

  if (reply.error != SPICE_LINK_ERR_OK)
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  if ((status = spice_read_nl(channel, &capsCommon , sizeof(capsCommon ), NULL)) != SPICE_STATUS_OK ||
      (status = spice_read_nl(channel, &capsChannel, sizeof(capsChannel), NULL)) != SPICE_STATUS_OK)
  {
    spice_disconnect_channel(channel);
    return status;
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (spice_write_nl(channel, &auth, sizeof(auth)) != sizeof(auth))
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  struct spice_password pass;
  if (!spice_rsa_encrypt_password(reply.pub_key, spice.password, &pass))
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  if (spice_write_nl(channel, pass.data, pass.size) != pass.size)
  {
    spice_rsa_free_password(&pass);
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  spice_rsa_free_password(&pass);

  uint32_t linkResult;
  if ((status = spice_read_nl(channel, &linkResult, sizeof(linkResult), NULL)) != SPICE_STATUS_OK)
  {
    spice_disconnect_channel(channel);
    return status;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    spice_disconnect_channel(channel);
    return SPICE_STATUS_ERROR;
  }

  channel->ready = true;
  return SPICE_STATUS_OK;
}

// ============================================================================

void spice_disconnect_channel(struct SpiceChannel * channel)
{
  if (!channel->connected)
    return;

  if (channel->ready)
  {
    /* disable nodelay so we can trigger a flush after this message */
    int flag;
    if (spice.family != AF_UNIX)
    {
      flag = 0;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    }

    SpiceMsgcDisconnecting * packet = SPICE_PACKET(SPICE_MSGC_DISCONNECTING,
        SpiceMsgcDisconnecting, 0);
    packet->time_stamp = get_timestamp();
    packet->reason     = SPICE_LINK_ERR_OK;
    SPICE_SEND_PACKET(channel, packet);

    /* re-enable nodelay as this triggers a flush according to the man page */
    if (spice.family != AF_UNIX)
    {
      flag = 1;
      setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    }
  }

  shutdown(channel->socket, SHUT_WR);
}

// ============================================================================

SPICE_STATUS spice_agent_connect()
{
  uint32_t * packet = SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_START, uint32_t, 0);
  memcpy(packet, &(uint32_t){SPICE_AGENT_TOKENS_MAX}, sizeof(uint32_t));
  if (!SPICE_SEND_PACKET(&spice.scMain, packet))
    return SPICE_STATUS_ERROR;

  return spice_agent_send_caps(true);
}

// ============================================================================

SPICE_STATUS spice_agent_process(uint32_t dataSize, int * dataAvailable)
{
  SPICE_STATUS status;
  if (spice.cbRemain)
  {
    const uint32_t r = spice.cbRemain > dataSize ? dataSize : spice.cbRemain;
    if ((status = spice_read_nl(&spice.scMain, spice.cbBuffer + spice.cbSize, r, dataAvailable)) != SPICE_STATUS_OK)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbRemain = 0;
      spice.cbSize   = 0;
      return status;
    }

    spice.cbRemain -= r;
    spice.cbSize   += r;

    if (spice.cbRemain == 0)
      spice_agent_on_clipboard();

    return SPICE_STATUS_OK;
  }

  VDAgentMessage msg;

  #pragma pack(push,1)
  struct Selection
  {
    uint8_t selection;
    uint8_t reserved[3];
  };
  #pragma pack(pop)

  if ((status = spice_read_nl(&spice.scMain, &msg, sizeof(msg), dataAvailable)) != SPICE_STATUS_OK)
    return status;

  dataSize -= sizeof(msg);

  if (msg.protocol != VD_AGENT_PROTOCOL)
    return SPICE_STATUS_ERROR;

  switch(msg.type)
  {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    {
      // make sure the message size is not insane to avoid a stack overflow
      // since we are using alloca for performance
      if (msg.size > 1024)
        return SPICE_STATUS_ERROR;

      VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)alloca(msg.size);

      if ((status = spice_read_nl(&spice.scMain, caps, msg.size, dataAvailable)) != SPICE_STATUS_OK)
        return status;

      const int capsSize = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg.size);
      spice.cbSupported  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) ||
                           VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);
      spice.cbSelection  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);

      if (caps->request)
        return spice_agent_send_caps(false);

      return SPICE_STATUS_OK;
    }

    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_RELEASE:
    {
      uint32_t remaining = msg.size;
      if (spice.cbSelection)
      {
        struct Selection selection;
        if ((status = spice_read_nl(&spice.scMain, &selection, sizeof(selection), dataAvailable)) != SPICE_STATUS_OK)
          return status;
        remaining -= sizeof(selection);
        dataSize  -= sizeof(selection);
      }

      if (msg.type == VD_AGENT_CLIPBOARD_RELEASE)
      {
        spice.cbAgentGrabbed = false;
        if (spice.cbReleaseFn)
          spice.cbReleaseFn();
        return SPICE_STATUS_OK;
      }

      if (msg.type == VD_AGENT_CLIPBOARD || msg.type == VD_AGENT_CLIPBOARD_REQUEST)
      {
        uint32_t type;
        if ((status = spice_read_nl(&spice.scMain, &type, sizeof(type), dataAvailable)) != SPICE_STATUS_OK)
          return status;
        remaining -= sizeof(type);
        dataSize  -= sizeof(type);

        if (msg.type == VD_AGENT_CLIPBOARD)
        {
          if (spice.cbBuffer)
            return SPICE_STATUS_ERROR;

          spice.cbSize     = 0;
          spice.cbRemain   = remaining;
          spice.cbBuffer   = (uint8_t *)malloc(remaining);
          const uint32_t r = remaining > dataSize ? dataSize : remaining;

          if ((status = spice_read_nl(&spice.scMain, spice.cbBuffer, r, dataAvailable)) != SPICE_STATUS_OK)
          {
            free(spice.cbBuffer);
            spice.cbBuffer = NULL;
            spice.cbRemain = 0;
            spice.cbSize   = 0;
            return status;
          }

          spice.cbRemain -= r;
          spice.cbSize   += r;

          if (spice.cbRemain == 0)
            spice_agent_on_clipboard();

          return SPICE_STATUS_OK;
        }
        else
        {
          if (spice.cbRequestFn)
            spice.cbRequestFn(agent_type_to_spice_type(type));
          return SPICE_STATUS_OK;
        }
      }
      else
      {
        if (remaining == 0)
          return SPICE_STATUS_OK;

        // ensure the size is sane to avoid a stack overflow since we use alloca
        // for performance
        if (remaining > 1024)
          return SPICE_STATUS_ERROR;

        uint32_t *types = alloca(remaining);
        if ((status = spice_read_nl(&spice.scMain, types, remaining, dataAvailable)) != SPICE_STATUS_OK)
          return status;

        // there is zero documentation on the types field, it might be a bitfield
        // but for now we are going to assume it's not.

        spice.cbType          = agent_type_to_spice_type(types[0]);
        spice.cbAgentGrabbed  = true;
        spice.cbClientGrabbed = false;
        if (spice.cbSelection)
        {
          // Windows doesnt support this, so until it's needed there is no point messing with it
          return SPICE_STATUS_OK;
        }

        if (spice.cbNoticeFn)
            spice.cbNoticeFn(spice.cbType);

        return SPICE_STATUS_OK;
      }
    }
  }

  return spice_discard_nl(&spice.scMain, msg.size, dataAvailable);
}


// ============================================================================

void spice_agent_on_clipboard()
{
  if (spice.cbDataFn)
    spice.cbDataFn(spice.cbType, spice.cbBuffer, spice.cbSize);

  free(spice.cbBuffer);
  spice.cbBuffer = NULL;
  spice.cbSize   = 0;
  spice.cbRemain = 0;
}

// ============================================================================

SPICE_STATUS spice_agent_send_caps(bool request)
{
  const ssize_t capsSize = sizeof(VDAgentAnnounceCapabilities) + VD_AGENT_CAPS_BYTES;
  VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)alloca(capsSize);
  memset(caps, 0, capsSize);

  caps->request = request ? 1 : 0;
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);

  if (!spice_agent_start_msg(VD_AGENT_ANNOUNCE_CAPABILITIES, capsSize) ||
      !spice_agent_write_msg(caps, capsSize))
    return SPICE_STATUS_ERROR;

  return SPICE_STATUS_OK;
}

// ============================================================================

bool spice_agent_start_msg(uint32_t type, ssize_t size)
{
  VDAgentMessage * msg =
    SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_DATA, VDAgentMessage, 0);

  msg->protocol  = VD_AGENT_PROTOCOL;
  msg->type      = type;
  msg->opaque    = 0;
  msg->size      = size;
  spice.agentMsg = size;

  SPICE_LOCK(spice.scMain.lock);
  if (!SPICE_SEND_PACKET_NL(&spice.scMain, msg))
  {
    SPICE_UNLOCK(spice.scMain.lock);
    return false;
  }

  if (size == 0)
    SPICE_UNLOCK(spice.scMain.lock);

  return true;
}

// ============================================================================

bool spice_agent_write_msg(const void * buffer, ssize_t size)
{
  assert(size <= spice.agentMsg);

  while(size)
  {
    const ssize_t toWrite = size > VD_AGENT_MAX_DATA_SIZE ?
      VD_AGENT_MAX_DATA_SIZE : size;

    void * p = SPICE_RAW_PACKET(SPICE_MSGC_MAIN_AGENT_DATA, 0, toWrite);
    if (!SPICE_SEND_PACKET_NL(&spice.scMain, p))
      goto err;

    if (spice_write_nl(&spice.scMain, buffer, toWrite) != toWrite)
      goto err;

    size           -= toWrite;
    buffer         += toWrite;
    spice.agentMsg -= toWrite;
  }

  if (!spice.agentMsg)
    SPICE_UNLOCK(spice.scMain.lock);

  return true;

err:
  SPICE_UNLOCK(spice.scMain.lock);
  return false;
}

// ============================================================================

ssize_t spice_write_nl(const struct SpiceChannel * channel, const void * buffer, const ssize_t size)
{
  if (!channel->connected)
    return -1;

  if (!buffer)
    return -1;

  return send(channel->socket, buffer, size, 0);
}

// ============================================================================

SPICE_STATUS spice_read_nl(struct SpiceChannel * channel, void * buffer, const ssize_t size, int * dataAvailable)
{
  if (!channel->connected)
    return SPICE_STATUS_ERROR;

  if (!buffer)
    return SPICE_STATUS_ERROR;

  size_t    left = size;
  uint8_t * buf  = (uint8_t *)buffer;
  while(left)
  {
    ssize_t len = read(channel->socket, buf, left);
    if (len == 0)
      return SPICE_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      return SPICE_STATUS_ERROR;
    }
    left -= len;
    buf  += len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return SPICE_STATUS_OK;
}

// ============================================================================

SPICE_STATUS spice_discard_nl(struct SpiceChannel * channel, ssize_t size, int * dataAvailable)
{
  uint8_t c[1024];
  ssize_t left = size;
  while(left)
  {
    ssize_t len = read(channel->socket, c, left > sizeof(c) ? sizeof(c) : left);
    if (len == 0)
      return SPICE_STATUS_NODATA;

    if (len < 0)
    {
      channel->connected = false;
      return SPICE_STATUS_ERROR;
    }

    left -= len;

    if (dataAvailable)
      *dataAvailable -= len;
  }

  return SPICE_STATUS_OK;
}

// ============================================================================

bool spice_key_down(uint32_t code)
{
  if (!spice.scInputs.connected)
    return false;

  if (code > 0x100)
    code = 0xe0 | ((code - 0x100) << 8);

  SpiceMsgcKeyDown * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_DOWN, SpiceMsgcKeyDown, 0);
  msg->code = code;
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

bool spice_key_up(uint32_t code)
{
  if (!spice.scInputs.connected)
    return false;

  if (code < 0x100)
    code |= 0x80;
  else
    code = 0x80e0 | ((code - 0x100) << 8);

  SpiceMsgcKeyUp * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_KEY_UP, SpiceMsgcKeyUp, 0);
  msg->code = code;
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

bool spice_mouse_mode(bool server)
{
  if (!spice.scMain.connected)
    return false;

  SpiceMsgcMainMouseModeRequest * msg = SPICE_PACKET(
    SPICE_MSGC_MAIN_MOUSE_MODE_REQUEST,
    SpiceMsgcMainMouseModeRequest, 0);

  msg->mouse_mode = server ? SPICE_MOUSE_MODE_SERVER : SPICE_MOUSE_MODE_CLIENT;
  return SPICE_SEND_PACKET(&spice.scMain, msg);
}

// ============================================================================

bool spice_mouse_position(uint32_t x, uint32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  SpiceMsgcMousePosition * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_POSITION, SpiceMsgcMousePosition, 0);

  msg->display_id   = 0;
  msg->button_state = spice.mouse.buttonState;
  msg->x            = x;
  msg->y            = y;

  atomic_fetch_add(&spice.mouse.sentCount, 1);
  if (!SPICE_SEND_PACKET(&spice.scInputs, msg))
    return false;

  return true;
}

// ============================================================================

bool spice_mouse_motion(int32_t x, int32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  /* while the protocol supports movements greater then +-127 the QEMU
   * virtio-mouse device does not, so we need to split this up into seperate
   * messages. For performance we build this as a single buffer otherwise this
   * will be split into multiple packets */

  const unsigned delta = abs(x) > abs(y) ? abs(x) : abs(y);
  const unsigned msgs  = (delta + 126) / 127;
  const ssize_t bufferSize = (
    sizeof(SpiceMiniDataHeader ) +
    sizeof(SpiceMsgcMouseMotion)
  ) * msgs;

  if (bufferSize > spice.motionBufferSize)
  {
    if (spice.motionBuffer)
      free(spice.motionBuffer);
    spice.motionBuffer     = malloc(bufferSize);
    spice.motionBufferSize = bufferSize;
  }

  uint8_t * buffer = spice.motionBuffer;
  uint8_t * msg    = buffer;

  while(x != 0 || y != 0)
  {
    SpiceMiniDataHeader  *h = (SpiceMiniDataHeader  *)msg;
    SpiceMsgcMouseMotion *m = (SpiceMsgcMouseMotion *)(h + 1);
    msg = (uint8_t*)(m + 1);

    h->size = sizeof(SpiceMsgcMouseMotion);
    h->type = SPICE_MSGC_INPUTS_MOUSE_MOTION;

    m->x = x > 127 ? 127 : (x < -127 ? -127 : x);
    m->y = y > 127 ? 127 : (y < -127 ? -127 : y);
    m->button_state = spice.mouse.buttonState;

    x -= m->x;
    y -= m->y;
  }

  atomic_fetch_add(&spice.mouse.sentCount, msgs);

  SPICE_LOCK(spice.scInputs.lock);
  const ssize_t wrote = send(spice.scInputs.socket, buffer, bufferSize, 0);
  SPICE_UNLOCK(spice.scInputs.lock);

  return wrote == bufferSize;
}

// ============================================================================

bool spice_mouse_press(uint32_t button)
{
  if (!spice.scInputs.connected)
    return false;

  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  : spice.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA : spice.mouse.buttonState |= _SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMousePress * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_PRESS, SpiceMsgcMousePress, 0);

  msg->button       = button;
  msg->button_state = spice.mouse.buttonState;

  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

bool spice_mouse_release(uint32_t button)
{
  if (!spice.scInputs.connected)
    return false;

  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT   : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_LEFT   ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE ; break;
    case SPICE_MOUSE_BUTTON_RIGHT  : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT  ; break;
    case _SPICE_MOUSE_BUTTON_SIDE  : spice.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_SIDE  ; break;
    case _SPICE_MOUSE_BUTTON_EXTRA : spice.mouse.buttonState &= ~_SPICE_MOUSE_BUTTON_MASK_EXTRA ; break;
  }

  SpiceMsgcMouseRelease * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_RELEASE, SpiceMsgcMouseRelease, 0);

  msg->button       = button;
  msg->button_state = spice.mouse.buttonState;

  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

static uint32_t spice_type_to_agent_type(SpiceDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return VD_AGENT_CLIPBOARD_UTF8_TEXT ; break;
    case SPICE_DATA_PNG : return VD_AGENT_CLIPBOARD_IMAGE_PNG ; break;
    case SPICE_DATA_BMP : return VD_AGENT_CLIPBOARD_IMAGE_BMP ; break;
    case SPICE_DATA_TIFF: return VD_AGENT_CLIPBOARD_IMAGE_TIFF; break;
    case SPICE_DATA_JPEG: return VD_AGENT_CLIPBOARD_IMAGE_JPG ; break;
    default:
      return VD_AGENT_CLIPBOARD_NONE;
  }
}

static SpiceDataType agent_type_to_spice_type(uint32_t type)
{
  switch(type)
  {
    case VD_AGENT_CLIPBOARD_UTF8_TEXT : return SPICE_DATA_TEXT; break;
    case VD_AGENT_CLIPBOARD_IMAGE_PNG : return SPICE_DATA_PNG ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_BMP : return SPICE_DATA_BMP ; break;
    case VD_AGENT_CLIPBOARD_IMAGE_TIFF: return SPICE_DATA_TIFF; break;
    case VD_AGENT_CLIPBOARD_IMAGE_JPG : return SPICE_DATA_JPEG; break;
    default:
      return SPICE_DATA_NONE;
  }
}

// ============================================================================

bool spice_clipboard_request(SpiceDataType type)
{
  VDAgentClipboardRequest req;

  if (!spice.cbAgentGrabbed)
    return false;

  if (type != spice.cbType)
    return false;

  req.type = spice_type_to_agent_type(type);
  if (!spice_agent_start_msg(VD_AGENT_CLIPBOARD_REQUEST, sizeof(req)) ||
      !spice_agent_write_msg(&req, sizeof(req)))
    return false;

  return true;
}

// ============================================================================

bool spice_set_clipboard_cb(SpiceClipboardNotice cbNoticeFn, SpiceClipboardData cbDataFn, SpiceClipboardRelease cbReleaseFn, SpiceClipboardRequest cbRequestFn)
{
  if ((cbNoticeFn && !cbDataFn) || (cbDataFn && !cbNoticeFn))
    return false;

  spice.cbNoticeFn  = cbNoticeFn;
  spice.cbDataFn    = cbDataFn;
  spice.cbReleaseFn = cbReleaseFn;
  spice.cbRequestFn = cbRequestFn;

  return true;
}

// ============================================================================

bool spice_clipboard_grab(SpiceDataType type)
{
  if (type == SPICE_DATA_NONE)
    return false;

  if (spice.cbSelection)
  {
    uint8_t req[8] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    ((uint32_t*)req)[1] = spice_type_to_agent_type(type);

    if (!spice_agent_start_msg(VD_AGENT_CLIPBOARD_GRAB, sizeof(req)) ||
        !spice_agent_write_msg(req, sizeof(req)))
      return false;

    spice.cbClientGrabbed = true;
    return true;
  }

  uint32_t req = spice_type_to_agent_type(type);
  if (!spice_agent_start_msg(VD_AGENT_CLIPBOARD_GRAB, sizeof(req)) ||
      !spice_agent_write_msg(&req, sizeof(req)))
    return false;

  spice.cbClientGrabbed = true;
  return true;
}

// ============================================================================

bool spice_clipboard_release()
{
  // check if if there is anything to release first
  if (!spice.cbClientGrabbed)
    return true;

  if (spice.cbSelection)
  {
    uint8_t req[4] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    if (!spice_agent_start_msg(VD_AGENT_CLIPBOARD_RELEASE, sizeof(req)) ||
        !spice_agent_write_msg(req, sizeof(req)))
      return false;

    spice.cbClientGrabbed = false;
    return true;
  }

   if (!spice_agent_start_msg(VD_AGENT_CLIPBOARD_RELEASE, 0))
     return false;

   spice.cbClientGrabbed = false;
   return true;
}

// ============================================================================

bool spice_clipboard_data_start(SpiceDataType type, size_t size)
{
  uint8_t buffer[8];
  size_t  bufSize;

  if (spice.cbSelection)
  {
    bufSize                = 8;
    buffer[0]              = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    buffer[1]              = buffer[2] = buffer[3] = 0;
    ((uint32_t*)buffer)[1] = spice_type_to_agent_type(type);
  }
  else
  {
    bufSize                = 4;
    ((uint32_t*)buffer)[0] = spice_type_to_agent_type(type);
  }

  return spice_agent_start_msg(VD_AGENT_CLIPBOARD, bufSize + size) &&
    spice_agent_write_msg(buffer, bufSize);
}

// ============================================================================

bool spice_clipboard_data(SpiceDataType type, uint8_t * data, size_t size)
{
  return spice_agent_write_msg(data, size);
}
