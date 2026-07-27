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
#include <limits.h>
#include <stddef.h>
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

  DISPLAY_SET_CAPABILITY(p.channelCaps, SPICE_DISPLAY_CAP_PREF_COMPRESSION);

  return &p.header;
}

PS_STATUS channelDisplay_onConnect(PSChannel * channel)
{
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
  }

  {
    SpiceMsgcPreferredCompression * msg =
      SPICE_PACKET(SPICE_MSGC_DISPLAY_PREFERRED_COMPRESSION,
          SpiceMsgcPreferredCompression, 0);

    msg->image_compression = SPICE_IMAGE_COMPRESSION_OFF;
    if (!SPICE_SEND_PACKET(channel, msg))
    {
      PS_LOG_ERROR("Failed to send SpiceMsgcPreferredCompression");
      return PS_STATUS_ERROR;
    }
  }

  return PS_STATUS_OK;
}

struct DisplayReader
{
  PSChannel * channel;
  size_t      offset;
};

static bool displayRead(struct DisplayReader * reader, void * dst,
    size_t length, const char * field)
{
  if (!channel_validateRange(reader->channel, reader->offset, length, field))
    return false;

  memcpy(dst, reader->channel->buffer + reader->offset, length);
  reader->offset += length;
  return true;
}

static bool displayOffset(struct DisplayReader * reader, uint32_t offset,
    size_t minimum, const uint8_t ** dst, const char * field)
{
  if (!offset)
  {
    *dst = NULL;
    return true;
  }

  if (!channel_validateRange(reader->channel, offset, minimum, field))
    return false;

  *dst = reader->channel->buffer + offset;
  return true;
}

static bool resolveDisplayBase(
    struct DisplayReader * reader, SpiceMsgDisplayBase * base)
{
  if (!displayRead(reader, &base->surface_id,
        sizeof(base->surface_id), "display surface id") ||
      !displayRead(reader, &base->box,
        sizeof(base->box), "display destination box") ||
      !displayRead(reader, &base->clip.type,
        sizeof(base->clip.type), "display clip type"))
    return false;

  base->clip.rects = NULL;
  switch(base->clip.type)
  {
    case SPICE_CLIP_TYPE_NONE:
      return true;

    case SPICE_CLIP_TYPE_RECTS:
    {
      const size_t rectsOffset = reader->offset;
      uint32_t count;
      if (!displayRead(reader, &count, sizeof(count), "display clip count"))
        return false;

      const size_t available =
        reader->channel->header.size - reader->offset;
      if (count > available / sizeof(SpiceRect))
      {
        PS_LOG_ERROR("DISPLAY: clip rectangle count exceeds its payload");
        return false;
      }

      base->clip.rects =
        (SpiceClipRects *)(reader->channel->buffer + rectsOffset);
      reader->offset += (size_t)count * sizeof(SpiceRect);
      return true;
    }

    default:
      PS_LOG_ERROR("DISPLAY: unknown clip type: %u", base->clip.type);
      return false;
  }
}

static bool resolveSpiceImage(
    struct DisplayReader * reader, SpiceImage ** dst)
{
  uint32_t offset;
  if (!displayRead(reader, &offset, sizeof(offset), "display image offset"))
    return false;

  const uint8_t * image;
  if (!displayOffset(reader, offset, sizeof(SpiceImageDescriptor),
        &image, "display image"))
    return false;

  *dst = (SpiceImage *)image;
  return true;
}

static bool resolveSpicePalette(struct DisplayReader * reader,
    SpicePalette ** dst, uint64_t * dstId)
{
  uint32_t offset;
  if (!displayRead(reader, &offset, sizeof(offset), "display palette offset"))
    return false;

  if (!offset)
  {
    *dst   = NULL;
    *dstId = 0;
    return true;
  }

  const uint8_t * palette;
  if (!displayOffset(reader, offset, offsetof(SpicePalette, ents),
        &palette, "display palette"))
    return false;
  *dst = (SpicePalette *)palette;

  uint16_t entries;
  memcpy(&entries,
      reader->channel->buffer + offset + offsetof(SpicePalette, num_ents),
      sizeof(entries));
  if (!channel_validateRange(reader->channel, offset,
        offsetof(SpicePalette, ents) + (size_t)entries * sizeof(uint32_t),
        "display palette entries"))
    return false;

  return displayRead(
      reader, dstId, sizeof(*dstId), "display palette cache id");
}

static bool resolveSpiceQMask(
    struct DisplayReader * reader, SpiceQMask * dst)
{
  return
    displayRead(reader, &dst->flags, sizeof(dst->flags), "display mask flags") &&
    displayRead(reader, &dst->pos  , sizeof(dst->pos  ), "display mask position") &&
    resolveSpiceImage(reader, &dst->bitmap);
}

static bool resolveSpiceCopy(
    struct DisplayReader * reader, SpiceCopy * dst)
{
  return
    resolveSpiceImage(reader, &dst->src_bitmap) &&
    displayRead(reader, &dst->meta, sizeof(dst->meta), "display copy metadata") &&
    resolveSpiceQMask(reader, &dst->mask);
}

static bool resolveSpicePattern(
    struct DisplayReader * reader, SpicePattern * dst)
{
  return
    resolveSpiceImage(reader, &dst->pat) &&
    displayRead(reader, &dst->pos, sizeof(dst->pos), "display pattern position");
}

static bool resolveSpiceBrush(
    struct DisplayReader * reader, SpiceBrush * dst)
{
  if (!displayRead(
        reader, &dst->type, sizeof(dst->type), "display brush type"))
    return false;

  switch(dst->type)
  {
    case SPICE_BRUSH_TYPE_NONE:
      return true;

    case SPICE_BRUSH_TYPE_SOLID:
      return displayRead(reader, &dst->u.color,
          sizeof(dst->u.color), "display brush color");

    case SPICE_BRUSH_TYPE_PATTERN:
      return resolveSpicePattern(reader, &dst->u.pattern);

    default:
      PS_LOG_ERROR("DISPLAY: unknown brush type: %u", dst->type);
      return false;
  }
}

static bool resolveSpiceFill(
    struct DisplayReader * reader, SpiceFill * dst)
{
  return
    resolveSpiceBrush(reader, &dst->brush) &&
    displayRead(reader, &dst->rop_descriptor,
        sizeof(dst->rop_descriptor), "display fill ROP") &&
    resolveSpiceQMask(reader, &dst->mask);
}

static bool readSpiceBitmap(PSChannel * channel,
    const SpiceImage * image, SpiceBitmap * dst)
{
  const size_t imageOffset = (const uint8_t *)image - channel->buffer;
  struct DisplayReader reader =
  {
    .channel = channel,
    .offset  = imageOffset + sizeof(SpiceImageDescriptor)
  };

  if (!displayRead(&reader, &dst->format, sizeof(dst->format), "bitmap format") ||
      !displayRead(&reader, &dst->flags , sizeof(dst->flags ), "bitmap flags") ||
      !displayRead(&reader, &dst->x     , sizeof(dst->x     ), "bitmap width") ||
      !displayRead(&reader, &dst->y     , sizeof(dst->y     ), "bitmap height") ||
      !displayRead(&reader, &dst->stride, sizeof(dst->stride), "bitmap stride") ||
      !resolveSpicePalette(&reader, &dst->palette, &dst->palette_id))
    return false;

  if (dst->format != SPICE_BITMAP_FMT_32BIT &&
      dst->format != SPICE_BITMAP_FMT_RGBA)
  {
    PS_LOG_ERROR("DISPLAY: unsupported bitmap format: %u", dst->format);
    return false;
  }

  if (dst->x > INT_MAX || dst->y > INT_MAX || dst->stride > INT_MAX ||
      dst->x > dst->stride / 4)
  {
    PS_LOG_ERROR("DISPLAY: invalid bitmap dimensions or stride "
        "(%ux%u, stride: %u)", dst->x, dst->y, dst->stride);
    return false;
  }

  if (dst->y && dst->stride > SIZE_MAX / dst->y)
  {
    PS_LOG_ERROR("DISPLAY: bitmap byte size overflows");
    return false;
  }

  const size_t bitmapSize = (size_t)dst->stride * dst->y;
  if (!channel_validateRange(
        channel, reader.offset, bitmapSize, "display bitmap data"))
    return false;

  dst->data = channel->buffer + reader.offset;
  return true;
}

static bool resolveDisplayDrawCopy(
    PSChannel * channel, SpiceMsgDisplayDrawCopy * dst)
{
  struct DisplayReader reader = { .channel = channel, .offset = 0 };
  return
    resolveDisplayBase(&reader, &dst->base) &&
    resolveSpiceCopy(&reader, &dst->data);
}

static bool resolveDisplayDrawFill(
    PSChannel * channel, SpiceMsgDisplayDrawFill * dst)
{
  struct DisplayReader reader = { .channel = channel, .offset = 0 };
  return
    resolveDisplayBase(&reader, &dst->base) &&
    resolveSpiceFill(&reader, &dst->data);
}

static PS_STATUS onMessage_displaySurfaceCreate(PSChannel * channel)
{
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgSurfaceCreate), "DISPLAY_SURFACE_CREATE"))
    return PS_STATUS_ERROR;

  SpiceMsgSurfaceCreate * msg = (SpiceMsgSurfaceCreate *)channel->buffer;
  if (msg->width > INT_MAX || msg->height > INT_MAX)
  {
    PS_LOG_ERROR("DISPLAY: surface dimensions exceed the client API");
    return PS_STATUS_ERROR;
  }

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

static PS_STATUS onMessage_displaySurfaceDestroy(PSChannel * channel)
{
  if (!channel_validatePayload(
        channel, sizeof(SpiceMsgSurfaceDestroy), "DISPLAY_SURFACE_DESTROY"))
    return PS_STATUS_ERROR;

  SpiceMsgSurfaceDestroy * msg = (SpiceMsgSurfaceDestroy *)channel->buffer;

  g_ps.config.display.surfaceDestroy(msg->surface_id);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_displayDrawFill(PSChannel * channel)
{
  SpiceMsgDisplayDrawFill dst;
  if (!resolveDisplayDrawFill(channel, &dst))
    return PS_STATUS_ERROR;

  if (dst.data.brush.type != SPICE_BRUSH_TYPE_SOLID)
  {
    PS_LOG_WARN("PureSpice only supports solid brushes for now");
    return PS_STATUS_OK;
  }

  const int64_t width  = (int64_t)dst.base.box.right -
    dst.base.box.left;
  const int64_t height = (int64_t)dst.base.box.bottom -
    dst.base.box.top;
  if (width < 0 || width > INT_MAX || height < 0 || height > INT_MAX)
  {
    PS_LOG_ERROR("DISPLAY: invalid fill destination rectangle");
    return PS_STATUS_ERROR;
  }

  g_ps.config.display.drawFill(
      dst.base.surface_id,
      dst.base.box.left,
      dst.base.box.top,
      width,
      height,
      dst.data.brush.u.color);
  return PS_STATUS_OK;
}

static PS_STATUS onMessage_displayDrawCopy(PSChannel * channel)
{
  SpiceMsgDisplayDrawCopy dst;
  if (!resolveDisplayDrawCopy(channel, &dst))
    return PS_STATUS_ERROR;

  const int64_t width  = (int64_t)dst.base.box.right -
    dst.base.box.left;
  const int64_t height = (int64_t)dst.base.box.bottom -
    dst.base.box.top;
  if (width < 0 || width > INT_MAX || height < 0 || height > INT_MAX)
  {
    PS_LOG_ERROR("DISPLAY: invalid copy destination rectangle");
    return PS_STATUS_ERROR;
  }

  // we only support bitmaps for now
  if (!dst.data.src_bitmap)
  {
    PS_LOG_WARN("PureSpice only supports bitmaps for now");
    return PS_STATUS_OK;
  }

  switch(dst.data.src_bitmap->descriptor.type)
  {
    case SPICE_IMAGE_TYPE_BITMAP:
    {
      SpiceBitmap bmp;
      if (!readSpiceBitmap(channel, dst.data.src_bitmap, &bmp))
        return PS_STATUS_ERROR;

      const bool topDown = bmp.flags & SPICE_BITMAP_FLAGS_TOP_DOWN;
      g_ps.config.display.drawBitmap(
          dst.base.surface_id,
          PS_BITMAP_FMT_RGBA,
          topDown,
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

PSHandlerFn channelDisplay_onMessage(PSChannel * channel)
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
