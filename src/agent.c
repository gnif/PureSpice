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

#include "purespice.h"

#include "ps.h"
#include "log.h"
#include "channel.h"
#include "channel_main.h"

#include "messages.h"
#include "rsa.h"
#include "queue.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>

#include <sys/ioctl.h>
#include <sys/epoll.h>

#include <spice/vd_agent.h>

typedef struct PSAgent PSAgent;
typedef struct PSAgentDeferred PSAgentDeferred;

struct PSAgentDeferred
{
  PSAgentDeferred * next;
  uint32_t          type;
  size_t            size;
  uint8_t           data[];
};

struct PSAgent
{
  bool present;
  struct Queue * queue;
  atomic_uint    serverTokens;

  // clipboard variables
  bool               cbSupported;
  bool               cbSelection;
  bool               cbAgentGrabbed;
  bool               cbClientGrabbed;
  PSDataType         cbType;
  uint8_t *          cbBuffer;
  uint32_t           cbRemain;
  uint32_t           cbSize;

  ssize_t msgSize;
  bool    clipboardWriteActive;
  PSDataType clipboardWriteType;

  PSAgentDeferred * deferred;
  PSAgentDeferred ** deferredLast;
};

static PSAgent agent =
{
  .deferredLast = &agent.deferred
};

static atomic_flag agentLock = ATOMIC_FLAG_INIT;
static _Thread_local unsigned int agentLockDepth;

static PS_STATUS agent_sendCaps(bool request);
static void agent_onClipboard(void);
static uint32_t psTypeToAgentType(PSDataType type);
static PSDataType agentTypeToPSType(uint32_t type);

static void agent_lock(void)
{
  if (agentLockDepth++ == 0)
    SPICE_LOCK(agentLock);
}

static void agent_unlock(void)
{
  assert(agentLockDepth > 0);
  if (--agentLockDepth == 0)
    SPICE_UNLOCK(agentLock);
}

static bool agent_presentLocked(void)
{
  return agent.present;
}

bool agent_present(void)
{
  agent_lock();
  const bool present = agent_presentLocked();
  agent_unlock();
  return present;
}

static PS_STATUS agent_connectLocked(void)
{
  if (!agent.queue)
    agent.queue = queue_new();
  else
  {
    void * msg;
    while(queue_shift(agent.queue, &msg))
      SPICE_RAW_PACKET_FREE(msg);
  }

  agent.msgSize = 0;
  agent.clipboardWriteActive = false;
  agent.clipboardWriteType = SPICE_DATA_NONE;
  while(agent.deferred)
  {
    PSAgentDeferred * msg = agent.deferred;
    agent.deferred = msg->next;
    free(msg);
  }
  agent.deferredLast = &agent.deferred;

  PSChannel * channel = &g_ps.channels[PS_CHANNEL_MAIN];
  uint32_t * packet = SPICE_PACKET(SPICE_MSGC_MAIN_AGENT_START, uint32_t, 0);
  memcpy(packet, &(uint32_t){SPICE_AGENT_TOKENS_MAX}, sizeof(uint32_t));
  if (!SPICE_SEND_PACKET(channel, packet))
  {
    PS_LOG_ERROR("Failed to send SPICE_MSGC_MAIN_AGENT_START");
    return PS_STATUS_ERROR;
  }

  agent.present = true;
  PS_STATUS ret = agent_sendCaps(true);
  if (ret != PS_STATUS_OK)
  {
    agent.present = false;
    PS_LOG_ERROR("Failed to send our capabillities to the spice guest agent");
    return ret;
  }

  PS_LOG_INFO("Connected to the spice guest agent");
  return PS_STATUS_OK;
}

PS_STATUS agent_connect(void)
{
  agent_lock();
  const PS_STATUS status = agent_connectLocked();
  agent_unlock();
  return status;
}

static void agent_disconnectLocked(void)
{
  if (agent.queue)
  {
    void * msg;
    while(queue_shift(agent.queue, &msg))
      SPICE_RAW_PACKET_FREE(msg);
    queue_free(agent.queue);
    agent.queue = NULL;
  }

  if (agent.cbBuffer)
  {
    free(agent.cbBuffer);
    agent.cbBuffer = NULL;
  }

  agent.cbRemain = 0;
  agent.cbSize   = 0;
  agent.msgSize = 0;
  agent.clipboardWriteActive = false;
  agent.clipboardWriteType = SPICE_DATA_NONE;

  while(agent.deferred)
  {
    PSAgentDeferred * msg = agent.deferred;
    agent.deferred = msg->next;
    free(msg);
  }
  agent.deferredLast = &agent.deferred;

  agent.cbAgentGrabbed  = false;
  agent.cbClientGrabbed = false;

  agent.present = false;
}

void agent_disconnect(void)
{
  agent_lock();
  agent_disconnectLocked();
  agent_unlock();
}

#pragma pack(push,1)
struct Selection
{
  uint8_t selection;
  uint8_t reserved[3];
};
#pragma pack(pop)

static PS_STATUS agent_processLocked(PSChannel * channel)
{
  if (agent.cbRemain)
  {
    if (channel->header.size > agent.cbRemain)
    {
      PS_LOG_ERROR(
          "Agent clipboard continuation exceeds remaining transfer size "
          "(fragment: %u, remaining: %u)",
          channel->header.size, agent.cbRemain);
      return PS_STATUS_ERROR;
    }

    memcpy(agent.cbBuffer + agent.cbSize, channel->buffer, channel->header.size);
    agent.cbRemain -= channel->header.size;
    agent.cbSize   += channel->header.size;

    if (!agent.cbRemain)
      agent_onClipboard();

    return PS_STATUS_OK;
  }


  if (!channel_validatePayload(
        channel, sizeof(VDAgentMessage), "MAIN_AGENT_DATA"))
    return PS_STATUS_ERROR;

  uint8_t        * data     = channel->buffer;
  unsigned int     dataSize = channel->header.size;
  VDAgentMessage * msg      = (VDAgentMessage *)data;
  data     += sizeof(*msg);
  dataSize -= sizeof(*msg);

  if (msg->size > SPICE_MAX_MESSAGE_SIZE)
  {
    PS_LOG_ERROR("VDAgent message is too large (%u bytes, maximum %u)",
        msg->size, SPICE_MAX_MESSAGE_SIZE);
    return PS_STATUS_ERROR;
  }

  if (dataSize > msg->size)
  {
    PS_LOG_ERROR(
        "VDAgent fragment exceeds its declared payload "
        "(fragment: %u, payload: %u)",
        dataSize, msg->size);
    return PS_STATUS_ERROR;
  }

  if (msg->protocol != VD_AGENT_PROTOCOL)
  {
    PS_LOG_ERROR("VDAgent protocol %d expected, but got %d",
        VD_AGENT_PROTOCOL, msg->protocol);
    return PS_STATUS_ERROR;
  }

  switch(msg->type)
  {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
    {
      if (msg->size < sizeof(VDAgentAnnounceCapabilities) ||
          msg->size > dataSize ||
          (msg->size - sizeof(VDAgentAnnounceCapabilities)) %
            sizeof(uint32_t) != 0)
      {
        PS_LOG_ERROR("Invalid VDAgent capabilities payload");
        return PS_STATUS_ERROR;
      }

      VDAgentAnnounceCapabilities * caps = (VDAgentAnnounceCapabilities *)data;
      const int capsSize = VD_AGENT_CAPS_SIZE_FROM_MSG_SIZE(msg->size);

      agent.cbSupported  =
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_BY_DEMAND) ||
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_SELECTION);

      agent.cbSelection  =
        VD_AGENT_HAS_CAPABILITY(caps->caps, capsSize,
            VD_AGENT_CAP_CLIPBOARD_SELECTION);

      if (caps->request)
        return agent_sendCaps(false);

      return PS_STATUS_OK;
    }

    case VD_AGENT_CLIPBOARD:
    case VD_AGENT_CLIPBOARD_REQUEST:
    case VD_AGENT_CLIPBOARD_GRAB:
    case VD_AGENT_CLIPBOARD_RELEASE:
    {
      // all clipboard messages might have this
      if (agent.cbSelection)
      {
        if (msg->size < sizeof(struct Selection) ||
            dataSize < sizeof(struct Selection))
        {
          PS_LOG_ERROR("Agent clipboard message is missing its selection");
          return PS_STATUS_ERROR;
        }

        data     += sizeof(struct Selection);
        dataSize -= sizeof(struct Selection);
      }

      switch(msg->type)
      {
        case VD_AGENT_CLIPBOARD_RELEASE:
          agent.cbAgentGrabbed = false;
          if (g_ps.config.clipboard.enable)
            g_ps.config.clipboard.release();
          return PS_STATUS_OK;

        case VD_AGENT_CLIPBOARD:
        {
          if (dataSize < sizeof(uint32_t))
          {
            PS_LOG_ERROR("Agent clipboard message is missing its data type");
            return PS_STATUS_ERROR;
          }

          data     += sizeof(uint32_t);
          dataSize -= sizeof(uint32_t);

          if (agent.cbBuffer)
          {
            PS_LOG_ERROR(
                "Agent tried to send a new clipboard instead of remaining data");
            return PS_STATUS_ERROR;
          }

          const unsigned int prefixSize =
            sizeof(uint32_t) +
            (agent.cbSelection ? sizeof(struct Selection) : 0);
          if (msg->size < prefixSize)
          {
            PS_LOG_ERROR("Agent clipboard message size is smaller than its header");
            return PS_STATUS_ERROR;
          }

          const unsigned int totalData = msg->size - prefixSize;
          if (dataSize > totalData)
          {
            PS_LOG_ERROR(
                "Agent clipboard fragment exceeds declared transfer size "
                "(fragment: %u, transfer: %u)",
                dataSize, totalData);
            return PS_STATUS_ERROR;
          }

          agent.cbBuffer = (uint8_t *)malloc(totalData);
          if (!agent.cbBuffer)
          {
            PS_LOG_ERROR("Failed to allocate buffer for clipboard transfer");
            return PS_STATUS_ERROR;
          }

          agent.cbSize   = dataSize;
          agent.cbRemain = totalData - dataSize;
          memcpy(agent.cbBuffer, data, dataSize);

          if (agent.cbRemain == 0)
            agent_onClipboard();

          return PS_STATUS_OK;
        }

        case VD_AGENT_CLIPBOARD_REQUEST:
        {
          if (msg->size < sizeof(uint32_t) +
                (agent.cbSelection ? sizeof(struct Selection) : 0) ||
              dataSize < sizeof(uint32_t))
          {
            PS_LOG_ERROR("Agent clipboard request is missing its data type");
            return PS_STATUS_ERROR;
          }

          uint32_t * type = (uint32_t *)data;

          if (g_ps.config.clipboard.enable)
            g_ps.config.clipboard.request(agentTypeToPSType(*type));
          return PS_STATUS_OK;
        }

        case VD_AGENT_CLIPBOARD_GRAB:
        {
          if (msg->size < sizeof(uint32_t) +
                (agent.cbSelection ? sizeof(struct Selection) : 0) ||
              dataSize < sizeof(uint32_t))
          {
            PS_LOG_ERROR("Agent clipboard grab contains no data types");
            return PS_STATUS_ERROR;
          }

          uint32_t *types = (uint32_t *)data;

          // there is zero documentation on the types field, it might be a
          // bitfield but for now we are going to assume it's not.

          agent.cbType          = agentTypeToPSType(types[0]);
          agent.cbAgentGrabbed  = true;
          agent.cbClientGrabbed = false;
          if (agent.cbSelection)
          {
            // Windows doesnt support this, so until it's needed there is no point
            // messing with it
            return PS_STATUS_OK;
          }

          if (g_ps.config.clipboard.enable)
            g_ps.config.clipboard.notice(agent.cbType);

          return PS_STATUS_OK;
        }
      }
    }
  }

  return PS_STATUS_OK;
}

PS_STATUS agent_process(PSChannel * channel)
{
  agent_lock();
  const PS_STATUS status = agent_processLocked(channel);
  agent_unlock();
  return status;
}

static void agent_onClipboard(void)
{
  if (g_ps.config.clipboard.enable)
    g_ps.config.clipboard.data(agent.cbType, agent.cbBuffer, agent.cbSize);

  free(agent.cbBuffer);
  agent.cbBuffer = NULL;
  agent.cbSize   = 0;
  agent.cbRemain = 0;
}

void agent_setServerTokens(unsigned int tokens)
{
  atomic_store(&agent.serverTokens, tokens);
}

static bool agent_takeServerToken(void)
{
  PSChannel * channel = &g_ps.channels[PS_CHANNEL_MAIN];

  unsigned int tokens;
  do
  {
    if (!channel->connected)
      return false;

    tokens = atomic_load(&agent.serverTokens);
    if (tokens == 0)
      return false;
  }
  while(!atomic_compare_exchange_weak(&agent.serverTokens, &tokens, tokens - 1));

  return true;
}

void agent_returnServerTokens(unsigned int tokens)
{
  atomic_fetch_add(&agent.serverTokens, tokens);
}

static bool agent_processQueueLocked(void)
{
  PSChannel * channel = &g_ps.channels[PS_CHANNEL_MAIN];

  SPICE_LOCK(channel->lock);
  while (queue_peek(agent.queue, NULL) && agent_takeServerToken())
  {
    void * msg;
    queue_shift(agent.queue, &msg);
    if (!SPICE_SEND_PACKET_NL(channel, msg))
    {
      SPICE_RAW_PACKET_FREE(msg);
      SPICE_UNLOCK(channel->lock);
      PS_LOG_ERROR("Failed to send a queued packet");
      return false;
    }
    SPICE_RAW_PACKET_FREE(msg);
  }
  SPICE_UNLOCK(channel->lock);
  return true;
}

static bool agent_startMsg(uint32_t type, ssize_t size)
{
  if (size < 0 || agent.msgSize != 0)
  {
    PS_LOG_ERROR("Attempted to start an overlapping VDAgent message");
    return false;
  }

  VDAgentMessage * msg =
    SPICE_PACKET_MALLOC(SPICE_MSGC_MAIN_AGENT_DATA, VDAgentMessage, 0);

  msg->protocol  = VD_AGENT_PROTOCOL;
  msg->type      = type;
  msg->opaque    = 0;
  msg->size      = size;
  agent.msgSize  = size;
  queue_push(agent.queue, msg);

  return agent_processQueueLocked();
}

static bool agent_writeMsg(const void * buffer_, ssize_t size)
{
  if (size < 0 || size > agent.msgSize)
  {
    PS_LOG_ERROR("VDAgent write exceeds the active message "
        "(write: %zd, remaining: %zd)", size, agent.msgSize);
    return false;
  }

  const char * buffer = buffer_;
  while(size)
  {
    const ssize_t toWrite = size > VD_AGENT_MAX_DATA_SIZE ?
      VD_AGENT_MAX_DATA_SIZE : size;

    void * msg = SPICE_RAW_PACKET_MALLOC(SPICE_MSGC_MAIN_AGENT_DATA, toWrite, 0);
    memcpy(msg, buffer, toWrite);
    queue_push(agent.queue, msg);

    size          -= toWrite;
    buffer        += toWrite;
    agent.msgSize -= toWrite;
  }

  return agent_processQueueLocked();
}

static bool agent_deferMessage(uint32_t type, const void * data, size_t size)
{
  if (size > SIZE_MAX - sizeof(PSAgentDeferred))
    return false;

  PSAgentDeferred * msg = malloc(sizeof(*msg) + size);
  if (!msg)
    return false;

  msg->next = NULL;
  msg->type = type;
  msg->size = size;
  if (size)
    memcpy(msg->data, data, size);

  *agent.deferredLast = msg;
  agent.deferredLast = &msg->next;
  return true;
}

static bool agent_sendMessage(
    uint32_t type, const void * data, size_t size)
{
  if (agent.clipboardWriteActive)
    return agent_deferMessage(type, data, size);

  if (size > SSIZE_MAX ||
      !agent_startMsg(type, (ssize_t)size) ||
      !agent_writeMsg(data, (ssize_t)size))
    return false;

  return true;
}

static bool agent_flushDeferred(void)
{
  while(agent.deferred)
  {
    PSAgentDeferred * msg = agent.deferred;
    agent.deferred = msg->next;
    if (!agent.deferred)
      agent.deferredLast = &agent.deferred;

    const bool result = agent_sendMessage(msg->type, msg->data, msg->size);
    free(msg);
    if (!result)
      return false;
  }

  return true;
}

bool agent_processQueue(void)
{
  agent_lock();
  const bool result = agent_processQueueLocked();
  agent_unlock();
  return result;
}

static PS_STATUS agent_sendCaps(bool request)
{
  if (!agent.present)
    return PS_STATUS_ERROR;

  const ssize_t capsSize = sizeof(VDAgentAnnounceCapabilities) +
    VD_AGENT_CAPS_BYTES;
  VDAgentAnnounceCapabilities *caps =
    (VDAgentAnnounceCapabilities *)alloca(capsSize);
  memset(caps, 0, capsSize);

  if (g_ps.config.clipboard.enable)
  {
    caps->request = request ? 1 : 0;
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
    VD_AGENT_SET_CAPABILITY(caps->caps, VD_AGENT_CAP_CLIPBOARD_SELECTION);
  }

  if (!agent_sendMessage(VD_AGENT_ANNOUNCE_CAPABILITIES, caps, capsSize))
  {
    PS_LOG_ERROR("Failed to send our agent capabilities");
    return PS_STATUS_ERROR;
  }

  return PS_STATUS_OK;
}

static uint32_t psTypeToAgentType(PSDataType type)
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

static PSDataType agentTypeToPSType(uint32_t type)
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

static bool purespice_clipboardRequestLocked(PSDataType type)
{
  if (!agent.present)
    return false;

  VDAgentClipboardRequest req;

  if (!agent.cbAgentGrabbed)
    return false;

  if (type != agent.cbType)
    return false;

  req.type = psTypeToAgentType(type);
  if (!agent_sendMessage(VD_AGENT_CLIPBOARD_REQUEST, &req, sizeof(req)))
  {
    PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD_REQUEST");
    return false;
  }

  return true;
}

static bool purespice_clipboardGrabLocked(PSDataType types[], int count)
{
  if (!agent.present)
    return false;

  if (count == 0)
    return false;

  if (agent.cbSelection)
  {
    struct Msg
    {
      uint8_t  selection;
      uint8_t  reserved;
      uint32_t types[0];
    };

    const int size = sizeof(struct Msg) + count * sizeof(uint32_t);
    struct Msg * msg = alloca(size);
    msg->selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    msg->reserved  = 0;
    for(int i = 0; i < count; ++i)
      msg->types[i] = psTypeToAgentType(types[i]);

    if (!agent_sendMessage(VD_AGENT_CLIPBOARD_GRAB, msg, size))
    {
      PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD_GRAB");
      return false;
    }

    agent.cbClientGrabbed = true;
    return true;
  }

  uint32_t msg[count];
  for(int i = 0; i < count; ++i)
    msg[i] = psTypeToAgentType(types[i]);

  if (!agent_sendMessage(VD_AGENT_CLIPBOARD_GRAB, msg, sizeof(msg)))
  {
    PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD_GRAB");
    return false;
  }

  agent.cbClientGrabbed = true;
  return true;
}

static bool purespice_clipboardReleaseLocked(void)
{
  if (!agent.present)
    return false;

  // check if if there is anything to release first
  if (!agent.cbClientGrabbed)
    return true;

  if (agent.cbSelection)
  {
    uint8_t req[4] = { VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD };
    if (!agent_sendMessage(VD_AGENT_CLIPBOARD_RELEASE, req, sizeof(req)))
    {
      PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD");
      return false;
    }

    agent.cbClientGrabbed = false;
    return true;
  }

   if (!agent_sendMessage(VD_AGENT_CLIPBOARD_RELEASE, NULL, 0))
   {
     PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD_RELEASE");
     return false;
   }

   agent.cbClientGrabbed = false;
   return true;
}

static bool purespice_clipboardDataStartLocked(
    PSDataType type, size_t size)
{
  if (!agent.present)
    return false;

  if (agent.clipboardWriteActive)
  {
    PS_LOG_ERROR("A clipboard write is already active");
    return false;
  }

  uint8_t buffer[8];
  size_t  bufSize;

  if (agent.cbSelection)
  {
    bufSize                = 8;
    buffer[0]              = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    buffer[1]              = buffer[2] = buffer[3] = 0;
    ((uint32_t*)buffer)[1] = psTypeToAgentType(type);
  }
  else
  {
    bufSize                = 4;
    ((uint32_t*)buffer)[0] = psTypeToAgentType(type);
  }

  if (size > SPICE_MAX_MESSAGE_SIZE - bufSize ||
      !agent_startMsg(VD_AGENT_CLIPBOARD, (ssize_t)(bufSize + size)))
  {
    PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD start");
    return false;
  }

  if (!agent_writeMsg(buffer, bufSize))
  {
    PS_LOG_ERROR("Failed to write VD_AGENT_CLIPBOARD data");
    return false;
  }

  if (size == 0)
    return agent_flushDeferred();

  agent.clipboardWriteActive = true;
  agent.clipboardWriteType = type;
  return true;
}

static bool purespice_clipboardDataLocked(
    PSDataType type, uint8_t * data, size_t size)
{
  if (!agent.present || !agent.clipboardWriteActive ||
      type != agent.clipboardWriteType || size > (size_t)agent.msgSize)
    return false;

  if (!agent_writeMsg(data, (ssize_t)size))
    return false;

  if (agent.msgSize != 0)
    return true;

  agent.clipboardWriteActive = false;
  agent.clipboardWriteType = SPICE_DATA_NONE;
  return agent_flushDeferred();
}

bool purespice_clipboardRequest(PSDataType type)
{
  agent_lock();
  const bool result = purespice_clipboardRequestLocked(type);
  agent_unlock();
  return result;
}

bool purespice_clipboardGrab(PSDataType types[], int count)
{
  agent_lock();
  const bool result = purespice_clipboardGrabLocked(types, count);
  agent_unlock();
  return result;
}

bool purespice_clipboardRelease(void)
{
  agent_lock();
  const bool result = purespice_clipboardReleaseLocked();
  agent_unlock();
  return result;
}

bool purespice_clipboardDataStart(PSDataType type, size_t size)
{
  agent_lock();
  const bool result = purespice_clipboardDataStartLocked(type, size);
  agent_unlock();
  return result;
}

bool purespice_clipboardData(PSDataType type, uint8_t * data, size_t size)
{
  agent_lock();
  const bool result = purespice_clipboardDataLocked(type, data, size);
  agent_unlock();
  return result;
}
