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

#include "purespice.h"
#include <stdlib.h>

#include "ps.h"
#include "log.h"
#include "channel.h"
#include "channel_playback.h"

#include "messages.h"

const SpiceLinkHeader * channelDisplay_getConnectPacket(void)
{
  typedef struct
  {
    SpiceLinkHeader header;
    SpiceLinkMess   message;
    uint32_t        supportCaps[COMMON_CAPS_BYTES  / sizeof(uint32_t)];
    uint32_t        channelCaps[DISPLAY_CAPS_BYTES / sizeof(uint32_t)];
  }
  __attribute__((packed)) ConnectPacket;

  static ConnectPacket p =
  {
    .header = {
      .magic         = SPICE_MAGIC        ,
      .major_version = SPICE_VERSION_MAJOR,
      .minor_version = SPICE_VERSION_MINOR,
      .size          = sizeof(ConnectPacket) - sizeof(SpiceLinkHeader)
    },
    .message = {
      .channel_type     = SPICE_CHANNEL_DISPLAY,
      .num_common_caps  = COMMON_CAPS_BYTES   / sizeof(uint32_t),
      .num_channel_caps = DISPLAY_CAPS_BYTES / sizeof(uint32_t),
      .caps_offset      = sizeof(SpiceLinkMess)
    }
  };

  p.message.connection_id = g_ps.sessionID;
  p.message.channel_id    = g_ps.channelID;

  memset(p.supportCaps, 0, sizeof(p.supportCaps));
  memset(p.channelCaps, 0, sizeof(p.channelCaps));

  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_PROTOCOL_AUTH_SELECTION);
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_AUTH_SPICE             );
  COMMON_SET_CAPABILITY(p.supportCaps, SPICE_COMMON_CAP_MINI_HEADER            );

//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_SIZED_STREAM);
//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_STREAM_REPORT);
//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_MONITORS_CONFIG);
//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_MULTI_CODEC);
//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_LZ4_COMPRESSION);
//  DISPLAY_SET_CAPABILITY(p.supportCaps, SPICE_DISPLAY_CAP_PREF_COMPRESSION);

  return &p.header;
}

PS_STATUS channelDisplay_onConnect(struct PSChannel * channel)
{
  SpiceMsgcDisplayInit * msg =
    SPICE_PACKET(SPICE_MSGC_DISPLAY_INIT,
        SpiceMsgcDisplayInit, 0);

  memset(msg, 0, sizeof(*msg));
  if (!SPICE_SEND_PACKET(channel, msg))
  {
    PS_LOG_ERROR("Failed to send SpiceMsgcDisplayInit");
    return PS_STATUS_ERROR;
  }

  return PS_STATUS_OK;
}

static void resolveDisplayBase(uint8_t ** ptr, SpiceMsgDisplayBase * base)
{
  memcpy(&base->surface_id, *ptr, sizeof(base->surface_id));
  *ptr += sizeof(base->surface_id);

  memcpy(&base->box, *ptr, sizeof(base->box));
  *ptr += sizeof(base->box);

  memcpy(&base->clip.type, *ptr, sizeof(base->clip.type));
  *ptr += sizeof(base->clip.type);

  if (base->clip.type == SPICE_CLIP_TYPE_RECTS)
  {
    base->clip.rects = (SpiceClipRects *)*ptr;
    *ptr += sizeof(base->clip.rects->num_rects);
    *ptr += base->clip.rects->num_rects * sizeof(SpiceRect);
  }
}

static void resolveSpiceCopy(const uint8_t * data, uint8_t ** ptr, SpiceCopy * dst)
{
  uint32_t offset;
  memcpy(&offset, *ptr, sizeof(offset));
  *ptr += sizeof(offset);
  dst->src_bitmap = (SpiceImage *)(offset > 0 ? data + offset : NULL);

  const int copy =
      sizeof(dst->src_area      ) +
      sizeof(dst->rop_descriptor) +
      sizeof(dst->scale_mode    ) +
      sizeof(dst->mask.flags    ) +
      sizeof(dst->mask.pos      );

  memcpy(&dst->src_area, *ptr, copy);
  *ptr += copy;

  memcpy(&offset, data, sizeof(offset));
  *ptr += sizeof(offset);
  dst->mask.bitmap = (SpiceImage *)(offset > 0 ? data + offset : NULL);
}

static void resolveDisplayDrawCopy(uint8_t * data, SpiceMsgDisplayDrawCopy * dst)
{
  uint8_t * ptr = data;
  resolveDisplayBase(      &ptr, &dst->base);
  resolveSpiceCopy  (data, &ptr, &dst->data);
}

static void resolveSpiceBitmap(const uint8_t * data, const SpiceImage * img,
    SpiceBitmap * bmp)
{
  uint8_t * ptr = (uint8_t *)&img->u.bitmap;

  const int copy =
      sizeof(bmp->format      ) +
      sizeof(bmp->flags       ) +
      sizeof(bmp->x           ) +
      sizeof(bmp->y           ) +
      sizeof(bmp->stride      );

  memcpy(bmp, ptr, copy);
  ptr += copy;

  uint32_t offset;
  memcpy(&offset, ptr, sizeof(offset));
  ptr += sizeof(offset);

  if (offset)
  {
    bmp->palette = (SpicePalette *)data + offset;
    memcpy(&bmp->palette_id, ptr, sizeof(bmp->palette_id));
    ptr += sizeof(bmp->palette_id);
  }
  else
  {
    bmp->palette    = NULL;
    bmp->palette_id = 0;
  }

  bmp->data = ptr;
}

static PS_STATUS onMessage_displaySurfaceCreate(struct PSChannel * channel)
{
  SpiceMsgSurfaceCreate * msg = (SpiceMsgSurfaceCreate *)channel->buffer;

  PSSurfaceFormat fmt;
  switch((SpiceSurfaceFmt)msg->format)
  {
    case SPICE_SURFACE_FMT_1_A    : fmt = PS_SURFACE_FMT_1_A    ; break;
    case SPICE_SURFACE_FMT_8_A    : fmt = PS_SURFACE_FMT_8_A    ; break;
    case SPICE_SURFACE_FMT_16_555 : fmt = PS_SURFACE_FMT_16_555 ; break;
    case SPICE_SURFACE_FMT_32_xRGB: fmt = PS_SURFACE_FMT_32_xRGB; break;
    case SPICE_SURFACE_FMT_16_565 : fmt = PS_SURFACE_FMT_16_565 ; break;
    case SPICE_SURFACE_FMT_32_ARGB: fmt = PS_SURFACE_FMT_32_ARGB; break;

    default:
      PS_LOG_ERROR("Unknown surface format: %u", msg->format);
      return PS_STATUS_ERROR;
  }

  g_ps.config.display.surfaceCreate(msg->surface_id, fmt,
      msg->width, msg->height);

  return PS_STATUS_OK;
}

static PS_STATUS onMessage_displaySurfaceDestroy(struct PSChannel * channel)
{
  SpiceMsgSurfaceDestroy * msg = (SpiceMsgSurfaceDestroy *)channel->buffer;

  g_ps.config.display.surfaceDestroy(msg->surface_id);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_displayDrawFill(struct PSChannel * channel)
{
  SpiceMsgDisplayDrawFill * msg = (SpiceMsgDisplayDrawFill *)channel->buffer;
  (void)msg;

  return PS_STATUS_OK;
}

#include <stdio.h>
static PS_STATUS onMessage_displayDrawCopy(struct PSChannel * channel)
{
  SpiceMsgDisplayDrawCopy dst;
  resolveDisplayDrawCopy(channel->buffer, &dst);

  // we only support bitmaps for now
  if (!dst.data.src_bitmap)
  {
    PS_LOG_ERROR("PureSpice only supports bitmaps for now");
    return PS_STATUS_OK;
  }

  switch(dst.data.src_bitmap->descriptor.type)
  {
    case SPICE_IMAGE_TYPE_BITMAP:
    {
      SpiceBitmap bmp;
      resolveSpiceBitmap(channel->buffer, dst.data.src_bitmap, &bmp);
      g_ps.config.display.drawBitmap(
          dst.base.surface_id,
          PS_BITMAP_FMT_RGBA,
          bmp.flags & SPICE_BITMAP_FLAGS_TOP_DOWN,
          dst.base.box.left,
          dst.base.box.top,
          bmp.x,
          bmp.y,
          bmp.stride,
          bmp.data);
      break;
    }

    default:
      PS_LOG_ERROR("PureSpice does not support compressed formats yet");
      break;
  }

  return PS_STATUS_OK;
}

PSHandlerFn channelDisplay_onMessage(struct PSChannel * channel)
{
  channel->initDone = true;
  switch(channel->header.type)
  {
    case SPICE_MSG_DISPLAY_SURFACE_CREATE:
      return onMessage_displaySurfaceCreate;

    case SPICE_MSG_DISPLAY_SURFACE_DESTROY:
      return onMessage_displaySurfaceDestroy;

    case SPICE_MSG_DISPLAY_DRAW_FILL:
      return onMessage_displayDrawFill;

    case SPICE_MSG_DISPLAY_DRAW_COPY:
      return onMessage_displayDrawCopy;
  }

  return PS_HANDLER_DISCARD;
}
