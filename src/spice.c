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

#define SPICE_PACKET(htype, payloadType, extraData) \
({ \
  uint8_t packet[sizeof(ssize_t) + sizeof(SpiceMiniDataHeader) + sizeof(payloadType)]; \
  ssize_t             * sz     = (ssize_t*)packet; \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(packet + sizeof(ssize_t)); \
  *sz          = sizeof(SpiceMiniDataHeader) + sizeof(payloadType); \
  header->type = (htype); \
  header->size = sizeof(payloadType) + extraData; \
  (payloadType *)(header + 1); \
})

/* only the main channel requires locking due to the agent needing to send
 * multiple packets, all other channels are atomic */
#define SPICE_SEND_PACKET_LOCKED(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  SPICE_LOCK((channel)->lock); \
  const ssize_t wrote = spice_write_nl((channel), header, *sz); \
  SPICE_UNLOCK((channel)->lock); \
  wrote == *sz; \
})

#define SPICE_SEND_PACKET(channel, packet) \
({ \
  SpiceMiniDataHeader * header = (SpiceMiniDataHeader *)(((uint8_t *)packet) - \
      sizeof(SpiceMiniDataHeader)); \
  ssize_t *sz = (ssize_t *)(((uint8_t *)header) - sizeof(ssize_t)); \
  const ssize_t wrote = spice_write_nl((channel), header, *sz); \
  wrote == *sz; \
})

// ============================================================================

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

  int                  sentCount;
  int                  rpos, wpos;
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
bool spice_connect_channel   (struct SpiceChannel * channel);
void spice_disconnect_channel(struct SpiceChannel * channel);

bool spice_process_ack(struct SpiceChannel * channel);

bool spice_on_common_read        (struct SpiceChannel * channel, SpiceMiniDataHeader * header, bool * handled);
bool spice_on_main_channel_read  ();
bool spice_on_inputs_channel_read();

bool spice_agent_process  (uint32_t dataSize);
bool spice_agent_connect  ();
bool spice_agent_send_caps(bool request);
void spice_agent_on_clipboard();

// utility functions
static uint32_t spice_type_to_agent_type(SpiceDataType type);
static SpiceDataType agent_type_to_spice_type(uint32_t type);

// thread safe read/write methods
bool spice_agent_write_msg (uint32_t type, const void * buffer, ssize_t size);

// non thread safe read/write methods (nl = non-locking)
bool    spice_read_nl   (      struct SpiceChannel * channel, void * buffer, const ssize_t size);
ssize_t spice_write_nl  (const struct SpiceChannel * channel, const void * buffer, const ssize_t size);
bool    spice_discard_nl(const struct SpiceChannel * channel, ssize_t size);

// ============================================================================

static uint64_t get_timestamp()
{
  struct timespec time;
  assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
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
  if (!spice_connect_channel(&spice.scMain))
    return false;

  return true;
}

// ============================================================================

void spice_disconnect()
{
  spice_disconnect_channel(&spice.scMain  );
  spice_disconnect_channel(&spice.scInputs);
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
      fds = spice.scMain.socket;
  }

  struct timeval tv;
  tv.tv_sec  = timeout / 1000;
  tv.tv_usec = (timeout % 1000) * 1000;

  int rc = select(fds + 1, &readSet, NULL, NULL, &tv);
  if (rc < 0)
    return false;

  if (spice.scMain.connected && FD_ISSET(spice.scMain.socket, &readSet))
  {
    if (!spice_on_main_channel_read())
      return false;

    if (spice.scMain.connected && !spice_process_ack(&spice.scMain))
      return false;
  }

  if (spice.scInputs.connected && FD_ISSET(spice.scInputs.socket, &readSet))
  {
    if (!spice_process_ack(&spice.scInputs))
      return false;

    if (!spice_on_inputs_channel_read())
      return false;
  }

  if (spice.scMain.connected | spice.scInputs.connected)
    return true;

  /* shutdown */
  spice.sessionID = 0;
  if (spice.cbBuffer)
    free(spice.cbBuffer);

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

bool spice_on_common_read(struct SpiceChannel * channel, SpiceMiniDataHeader * header, bool * handled)
{
  *handled = false;
  if (!spice_read_nl(channel, header, sizeof(SpiceMiniDataHeader)))
    return false;

  if (!channel->connected)
  {
    *handled = true;
    return true;
  }

  if (!channel->initDone)
    return true;

  switch(header->type)
  {
    case SPICE_MSG_MIGRATE:
    case SPICE_MSG_MIGRATE_DATA:
    {
      *handled = true;
      return false;
    }

    case SPICE_MSG_SET_ACK:
    {
      *handled = true;

      SpiceMsgSetAck in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      channel->ackFrequency = in.window;

      SpiceMsgcAckSync * out =
        SPICE_PACKET(SPICE_MSGC_ACK_SYNC, SpiceMsgcAckSync, 0);

      out->generation = in.generation;
      return SPICE_SEND_PACKET_LOCKED(channel, out);
    }

    case SPICE_MSG_PING:
    {
      *handled = true;

      SpiceMsgPing in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      const int discard = header->size - sizeof(in);
      if (!spice_discard_nl(channel, discard))
        return false;

      SpiceMsgcPong * out =
        SPICE_PACKET(SPICE_MSGC_PONG, SpiceMsgcPong, 0);

      out->id        = in.id;
      out->timestamp = in.timestamp;
      return SPICE_SEND_PACKET_LOCKED(channel, out);
    }

    case SPICE_MSG_WAIT_FOR_CHANNELS:
    {
      *handled = true;
      return false;
    }

    case SPICE_MSG_DISCONNECTING:
    {
      *handled = true;
      shutdown(channel->socket, SHUT_WR);
      return true;
    }

    case SPICE_MSG_NOTIFY:
    {
      SpiceMsgNotify in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      char msg[in.message_len+1];
      if (!spice_read_nl(channel, msg, in.message_len+1))
        return false;

      *handled = true;
      return true;
    }
  }

  return true;
}

// ============================================================================

bool spice_on_main_channel_read()
{
  struct SpiceChannel *channel = &spice.scMain;

  SpiceMiniDataHeader header;
  bool handled;

  if (!spice_on_common_read(channel, &header, &handled))
    return false;

  if (handled)
    return true;

  if (!channel->initDone)
  {
    if (header.type != SPICE_MSG_MAIN_INIT)
    {
      spice_disconnect();
      return false;
    }

    channel->initDone = true;
    SpiceMsgMainInit msg;
    if (!spice_read_nl(channel, &msg, sizeof(msg)))
    {
      spice_disconnect();
      return false;
    }

    spice.sessionID = msg.session_id;

    spice.serverTokens = msg.agent_tokens;
    spice.hasAgent     = msg.agent_connected;
    if (spice.hasAgent && !spice_agent_connect())
    {
      spice_disconnect();
      return false;
    }

    if (msg.current_mouse_mode != SPICE_MOUSE_MODE_CLIENT && !spice_mouse_mode(false))
      return false;

    void * packet = SPICE_PACKET(SPICE_MSGC_MAIN_ATTACH_CHANNELS, void, 0);
    if (!SPICE_SEND_PACKET_LOCKED(channel, packet))
    {
      spice_disconnect();
      return false;
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_CHANNELS_LIST)
  {
    SpiceMainChannelsList msg;
    if (!spice_read_nl(channel, &msg, sizeof(msg)))
    {
      spice_disconnect();
      return false;
    }

    // documentation doesn't state that the array is null terminated but it seems that it is
    SpiceChannelID channels[msg.num_of_channels];
    if (!spice_read_nl(channel, &channels, msg.num_of_channels * sizeof(SpiceChannelID)))
    {
      spice_disconnect();
      return false;
    }

    for(int i = 0; i < msg.num_of_channels; ++i)
    {
      if (channels[i].type == SPICE_CHANNEL_INPUTS)
      {
        if (spice.scInputs.connected)
        {
          spice_disconnect();
          return false;
        }

        if (!spice_connect_channel(&spice.scInputs))
        {
          spice_disconnect();
          return false;
        }
      }
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED)
  {
    spice.hasAgent = true;
    if (!spice_agent_connect())
    {
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS)
  {
    uint32_t num_tokens;
    if (!spice_read_nl(channel, &num_tokens, sizeof(num_tokens)))
    {
      spice_disconnect();
      return false;
    }

    spice.hasAgent    = true;
    spice.serverTokens = num_tokens;
    if (!spice_agent_connect())
    {
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DISCONNECTED)
  {
    uint32_t error;
    if (!spice_read_nl(channel, &error, sizeof(error)))
    {
      spice_disconnect();
      return false;
    }

    spice.hasAgent = false;

    if (spice.cbBuffer)
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbSize   = 0;
      spice.cbRemain = 0;
    }

    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_DATA)
  {
    if (!spice.hasAgent)
    {
      spice_discard_nl(channel, header.size);
      return true;
    }

    if (!spice_agent_process(header.size))
    {
      spice_disconnect();
      return false;
    }
    return true;
  }

  if (header.type == SPICE_MSG_MAIN_AGENT_TOKEN)
  {
    uint32_t num_tokens;
    if (!spice_read_nl(channel, &num_tokens, sizeof(num_tokens)))
    {
      spice_disconnect();
      return false;
    }

    spice.serverTokens = num_tokens;
    return true;
  }

  spice_discard_nl(channel, header.size);
  return true;
}

// ============================================================================

bool spice_on_inputs_channel_read()
{
  struct SpiceChannel *channel = &spice.scInputs;

  SpiceMiniDataHeader header;
  bool handled;

  if (!spice_on_common_read(channel, &header, &handled))
    return false;

  if (handled)
    return true;

  switch(header.type)
  {
    case SPICE_MSG_INPUTS_INIT:
    {
      if (channel->initDone)
        return false;

      channel->initDone = true;

      SpiceMsgInputsInit in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      return true;
    }

    case SPICE_MSG_INPUTS_KEY_MODIFIERS:
    {
      SpiceMsgInputsInit in;
      if (!spice_read_nl(channel, &in, sizeof(in)))
        return false;

      spice.kb.modifiers = in.modifiers;
      return true;
    }

    case SPICE_MSG_INPUTS_MOUSE_MOTION_ACK:
    {
      const int count = __sync_add_and_fetch(&spice.mouse.sentCount, SPICE_INPUT_MOTION_ACK_BUNCH);
      if (count < 0)
        return false;
      return true;
    }
  }

  spice_discard_nl(channel, header.size);
  return true;
}

// ============================================================================

bool spice_connect_channel(struct SpiceChannel * channel)
{
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
      return false;
  }

  channel->socket = socket(spice.family, SOCK_STREAM, 0);
  if (channel->socket == -1)
    return false;

  if (spice.family != AF_UNIX)
  {
    const int flag = 1;
    setsockopt(channel->socket, IPPROTO_TCP, TCP_NODELAY , &flag, sizeof(int));
    setsockopt(channel->socket, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(int));
  }

  if (connect(channel->socket, &spice.addr.addr, addrSize) == -1)
  {
    close(channel->socket);
    return false;
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

  if (!spice_write_nl(channel, &p, sizeof(p)))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (!spice_read_nl(channel, &p.header, sizeof(p.header)))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (p.header.magic         != SPICE_MAGIC ||
      p.header.major_version != SPICE_VERSION_MAJOR)
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (p.header.size < sizeof(SpiceLinkReply))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  SpiceLinkReply reply;
  if (!spice_read_nl(channel, &reply, sizeof(reply)))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (reply.error != SPICE_LINK_ERR_OK)
  {
    spice_disconnect_channel(channel);
    return false;
  }

  uint32_t capsCommon [reply.num_common_caps ];
  uint32_t capsChannel[reply.num_channel_caps];
  if (
      !spice_read_nl(channel, &capsCommon , sizeof(capsCommon)) ||
      !spice_read_nl(channel, &capsChannel, sizeof(capsChannel))
     )
  {
    spice_disconnect_channel(channel);
    return false;
  }

  SpiceLinkAuthMechanism auth;
  auth.auth_mechanism = SPICE_COMMON_CAP_AUTH_SPICE;
  if (!spice_write_nl(channel, &auth, sizeof(auth)))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  struct spice_password pass;
  if (!spice_rsa_encrypt_password(reply.pub_key, spice.password, &pass))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (!spice_write_nl(channel, pass.data, pass.size))
  {
    spice_rsa_free_password(&pass);
    spice_disconnect_channel(channel);
    return false;
  }

  spice_rsa_free_password(&pass);

  uint32_t linkResult;
  if (!spice_read_nl(channel, &linkResult, sizeof(linkResult)))
  {
    spice_disconnect_channel(channel);
    return false;
  }

  if (linkResult != SPICE_LINK_ERR_OK)
  {
    spice_disconnect_channel(channel);
    return false;
  }

  channel->ready = true;
  return true;
}

// ============================================================================

void spice_disconnect_channel(struct SpiceChannel * channel)
{
  if (!channel->connected)
    return;

  if (channel->ready)
  {
    SpiceMsgcDisconnecting * packet = SPICE_PACKET(SPICE_MSGC_DISCONNECTING,
        SpiceMsgcDisconnecting, 0);
    packet->time_stamp = get_timestamp();
    packet->reason     = SPICE_LINK_ERR_OK;
    SPICE_SEND_PACKET(channel, packet);
  }

  shutdown(channel->socket, SHUT_WR);
}

// ============================================================================

bool spice_agent_connect()
{
  uint32_t * packet = SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_START, uint32_t, 0);
  *packet = SPICE_AGENT_TOKENS_MAX;
  if (!SPICE_SEND_PACKET_LOCKED(&spice.scMain, packet))
    return false;

  if (!spice_agent_send_caps(true))
    return false;

  return true;
}

// ============================================================================

bool spice_agent_process(uint32_t dataSize)
{
  if (spice.cbRemain)
  {
    const uint32_t r = spice.cbRemain > dataSize ? dataSize : spice.cbRemain;
    if (!spice_read_nl(&spice.scMain, spice.cbBuffer + spice.cbSize, r))
    {
      free(spice.cbBuffer);
      spice.cbBuffer = NULL;
      spice.cbRemain = 0;
      spice.cbSize   = 0;
      return false;
    }

    spice.cbRemain -= r;
    spice.cbSize   += r;

    if (spice.cbRemain == 0)
      spice_agent_on_clipboard();

    return true;
  }

  VDAgentMessage msg;

  #pragma pack(push,1)
  struct Selection
  {
    uint8_t selection;
    uint8_t reserved[3];
  };
  #pragma pack(pop)

  if (!spice_read_nl(&spice.scMain, &msg, sizeof(msg)))
    return false;
  dataSize -= sizeof(msg);

  if (msg.protocol != VD_AGENT_PROTOCOL)
    return false;

  switch(msg.type)
  {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    {
      VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)malloc(msg.size);
      memset(caps, 0, msg.size);

      if (!spice_read_nl(&spice.scMain, caps, msg.size))
      {
        free(caps);
        return false;
      }

      const int capsSize = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg.size);
      spice.cbSupported  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) ||
                           VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);
      spice.cbSelection  = VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize, VD_AGENT_CAP_CLIPBOARD_SELECTION);

      if (caps->request && !spice_agent_send_caps(false))
      {
        free(caps);
        return false;
      }

      free(caps);
      return true;
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
        if (!spice_read_nl(&spice.scMain, &selection, sizeof(selection)))
          return false;
        remaining -= sizeof(selection);
        dataSize  -= sizeof(selection);
      }

      if (msg.type == VD_AGENT_CLIPBOARD_RELEASE)
      {
        spice.cbAgentGrabbed = false;
        if (spice.cbReleaseFn)
          spice.cbReleaseFn();
        return true;
      }

      if (msg.type == VD_AGENT_CLIPBOARD || msg.type == VD_AGENT_CLIPBOARD_REQUEST)
      {
        uint32_t type;
        if (!spice_read_nl(&spice.scMain, &type, sizeof(type)))
          return false;
        remaining -= sizeof(type);
        dataSize  -= sizeof(type);

        if (msg.type == VD_AGENT_CLIPBOARD)
        {
          if (spice.cbBuffer)
            return false;

          spice.cbSize     = 0;
          spice.cbRemain   = remaining;
          spice.cbBuffer   = (uint8_t *)malloc(remaining);
          const uint32_t r = remaining > dataSize ? dataSize : remaining;

          if (!spice_read_nl(&spice.scMain, spice.cbBuffer, r))
          {
            free(spice.cbBuffer);
            spice.cbBuffer = NULL;
            spice.cbRemain = 0;
            spice.cbSize   = 0;
            return false;
          }

          spice.cbRemain -= r;
          spice.cbSize   += r;

          if (spice.cbRemain == 0)
            spice_agent_on_clipboard();

          return true;
        }
        else
        {
          if (spice.cbRequestFn)
            spice.cbRequestFn(agent_type_to_spice_type(type));
          return true;
        }
      }
      else
      {
        if (remaining == 0)
          return true;

        uint32_t *types = malloc(remaining);
        if (!spice_read_nl(&spice.scMain, types, remaining))
          return false;

        // there is zero documentation on the types field, it might be a bitfield
        // but for now we are going to assume it's not.

        spice.cbType          = agent_type_to_spice_type(types[0]);
        spice.cbAgentGrabbed  = true;
        spice.cbClientGrabbed = false;
        if (spice.cbSelection)
        {
          // Windows doesnt support this, so until it's needed there is no point messing with it
          return false;
        }

        if (spice.cbNoticeFn)
            spice.cbNoticeFn(spice.cbType);

        free(types);
        return true;
      }
    }
  }

  spice_discard_nl(&spice.scMain, msg.size);
  return true;
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

bool spice_agent_send_caps(bool request)
{
  const ssize_t capsSize = sizeof(VDAgentAnnounceCapabilities) + VD_AGENT_CAPS_BYTES;
  VDAgentAnnounceCapabilities *caps = (VDAgentAnnounceCapabilities *)malloc(capsSize);
  memset(caps, 0, capsSize);

  caps->request = request ? 1 : 0;
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
  VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);

  if (!spice_agent_write_msg(VD_AGENT_ANNOUNCE_CAPABILITIES, caps, capsSize))
  {
    free(caps);
    return false;
  }
  free(caps);

  return true;
}

// ============================================================================

bool spice_agent_write_msg(uint32_t type, const void * buffer, ssize_t size)
{
  uint8_t * buf   = (uint8_t *)buffer;
  ssize_t toWrite = size > VD_AGENT_MAX_DATA_SIZE - sizeof(VDAgentMessage) ?
    VD_AGENT_MAX_DATA_SIZE - sizeof(VDAgentMessage) : size;

  VDAgentMessage * msg =
    SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_DATA, VDAgentMessage, toWrite);

  msg->protocol = VD_AGENT_PROTOCOL;
  msg->type     = type;
  msg->opaque   = 0;
  msg->size     = size;

  SPICE_LOCK(spice.scMain.lock);

  if (!SPICE_SEND_PACKET(&spice.scMain, msg))
  {
    SPICE_UNLOCK(spice.scMain.lock);
    return false;
  }

  bool first = true;
  while(toWrite)
  {
    bool ok = false;
    if (first)
    {
      ok    = spice_write_nl(&spice.scMain, buf, toWrite) == toWrite;
      first = false;
    }
    else
    {
      void * cont = SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_DATA, void, toWrite);
      ok = SPICE_SEND_PACKET(&spice.scMain, cont);
    }

    if (!ok)
    {
      SPICE_UNLOCK(spice.scMain.lock);
      return false;
    }

    size   -= toWrite;
    buf    += toWrite;
    toWrite = size > VD_AGENT_MAX_DATA_SIZE ? VD_AGENT_MAX_DATA_SIZE : size;
  }

  SPICE_UNLOCK(spice.scMain.lock);
  return true;
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

bool spice_read_nl(struct SpiceChannel * channel, void * buffer, const ssize_t size)
{
  if (!channel->connected)
    return false;

  if (!buffer)
    return false;

  size_t    left = size;
  uint8_t * buf  = (uint8_t *)buffer;
  while(left)
  {
    ssize_t len = read(channel->socket, buf, left);
    if (len <= 0)
    {
      channel->connected = false;
      return true;
    }
    left -= len;
    buf  += len;
  }

  return true;
}

// ============================================================================

bool spice_discard_nl(const struct SpiceChannel * channel, ssize_t size)
{
  void *c = malloc(8192);
  ssize_t left = size;
  while(left)
  {
    size_t len = read(channel->socket, c, left > 8192 ? 8192 : left);
    if (len <= 0)
    {
      free(c);
      return false;
    }
    left -= len;
  }

  free(c);
  return true;
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
  return SPICE_SEND_PACKET_LOCKED(&spice.scMain, msg);
}

// ============================================================================

bool spice_mouse_position(uint32_t x, uint32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  SpiceMsgcMousePosition * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_POSITION, SpiceMsgcMousePosition, 0);

  msg->x            = x;
  msg->y            = y;
  msg->button_state = spice.mouse.buttonState;
  msg->display_id   = 0;

  __sync_fetch_and_add(&spice.mouse.sentCount, 1);
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

bool spice_mouse_motion(int32_t x, int32_t y)
{
  if (!spice.scInputs.connected)
    return false;

  SpiceMsgcMouseMotion * msg =
    SPICE_PACKET(SPICE_MSGC_INPUTS_MOUSE_MOTION, SpiceMsgcMouseMotion, 0);

  msg->x            = x;
  msg->y            = y;
  msg->button_state = spice.mouse.buttonState;

  __sync_fetch_and_add(&spice.mouse.sentCount, 1);
  return SPICE_SEND_PACKET(&spice.scInputs, msg);
}

// ============================================================================

bool spice_mouse_press(uint32_t button)
{
  if (!spice.scInputs.connected)
    return false;

  switch(button)
  {
    case SPICE_MOUSE_BUTTON_LEFT  : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_LEFT  ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE: spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_MIDDLE; break;
    case SPICE_MOUSE_BUTTON_RIGHT : spice.mouse.buttonState |= SPICE_MOUSE_BUTTON_MASK_RIGHT ; break;
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
    case SPICE_MOUSE_BUTTON_LEFT  : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_LEFT  ; break;
    case SPICE_MOUSE_BUTTON_MIDDLE: spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_MIDDLE; break;
    case SPICE_MOUSE_BUTTON_RIGHT : spice.mouse.buttonState &= ~SPICE_MOUSE_BUTTON_MASK_RIGHT ; break;
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
  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_REQUEST, &req, sizeof(req)))
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

    if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_GRAB, req, sizeof(req)))
      return false;

    spice.cbClientGrabbed = true;
    return true;
  }

  uint32_t req = spice_type_to_agent_type(type);
  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_GRAB, &req, sizeof(req)))
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
    if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_RELEASE, req, sizeof(req)))
      return false;

    spice.cbClientGrabbed = false;
    return true;
  }

   if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD_RELEASE, NULL, 0))
     return false;

   spice.cbClientGrabbed = false;
   return true;
}

// ============================================================================

bool spice_clipboard_data(SpiceDataType type, uint8_t * data, size_t size)
{
  uint8_t * buffer;
  size_t    bufSize;

  if (spice.cbSelection)
  {
    bufSize                = 8 + size;
    buffer                 = (uint8_t *)malloc(bufSize);
    buffer[0]              = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    buffer[1]              = buffer[2] = buffer[3] = 0;
    ((uint32_t*)buffer)[1] = spice_type_to_agent_type(type);
    memcpy(buffer + 8, data, size);
  }
  else
  {
    bufSize                = 4 + size;
    buffer                 = (uint8_t *)malloc(bufSize);
    ((uint32_t*)buffer)[0] = spice_type_to_agent_type(type);
    memcpy(buffer + 4, data, size);
  }

  if (!spice_agent_write_msg(VD_AGENT_CLIPBOARD, buffer, bufSize))
  {
    free(buffer);
    return false;
  }

  free(buffer);
  return true;
}
