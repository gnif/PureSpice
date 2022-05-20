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

#ifndef _H_I_DRAW_
#define _H_I_DRAW_

#include <stdint.h>

#pragma pack(push,1)

typedef struct SpiceChunk
{
  uint8_t *data;
  uint32_t len;
}
SpiceChunk;

enum
{
  SPICE_CHUNKS_FLAGS_UNSTABLE = (1<<0),
  SPICE_CHUNKS_FLAGS_FREE     = (1<<1)
};

typedef struct SpicePoint
{
  int32_t x;
  int32_t y;
}
SpicePoint;

typedef struct SpiceChunks
{
  uint32_t   data_size;
  uint32_t   num_chunks;
  uint32_t   flags;
  SpiceChunk chunk[];
}
SpiceChunks;

typedef struct SpicePalette {
  uint64_t unique;
  uint16_t num_ents;
  uint32_t ents[];
}
SpicePalette;

typedef struct SpiceSurface
{
  uint32_t surface_id;
}
SpiceSurface;

typedef struct SpiceBitmap
{
  uint8_t        format;
  uint8_t        flags;
  uint32_t       x;
  uint32_t       y;
  uint32_t       stride;
  SpicePalette * palette;
  uint64_t       palette_id;
  uint8_t      * data;
}
SpiceBitmap;

typedef struct SpiceQUICData
{
  uint32_t data_size;
  uint8_t  data[];
}
SpiceQUICData,
SpiceLZRGBData,
SpiceJPEGData,
SpiceLZ4Data;

typedef struct SpiceLZPLTData
{
  uint8_t        flags;
  uint32_t       data_size;
  SpicePalette * palette;
  uint64_t       palette_id;
  SpiceChunks  * data;
}
SpiceLZPLTData;

typedef struct SpiceZlibGlzRGBData
{
  uint32_t      glz_data_size;
  uint32_t      data_size;
  SpiceChunks * data;
}
SpiceZlibGlzRGBData;

typedef struct SpiceJPEGAlphaData
{
  uint8_t       flags;
  uint32_t      jpeg_size;
  uint32_t      data_size;
  SpiceChunks * data;
}
SpiceJPEGAlphaData;

typedef struct SpiceImageDescriptor
{
  uint64_t id;
  uint8_t  type;
  uint8_t  flags;
  uint32_t width;
  uint32_t height;
}
SpiceImageDescriptor;

typedef struct SpiceImage
{
  SpiceImageDescriptor descriptor;
  union
  {
    SpiceBitmap         bitmap;
//    SpiceQUICData       quic;
//    SpiceSurface        surface;
//    SpiceLZRGBData      lz_rgb;
//    SpiceLZPLTData      lz_plt;
//    SpiceJPEGData       jpeg;
//    SpiceLZ4Data        lz4;
//    SpiceZlibGlzRGBData zlib_glz;
//    SpiceJPEGAlphaData  jpeg_alpha;
  }
  u;
}
SpiceImage;

typedef struct SpicePattern
{
  SpiceImage * pat;
  SpicePoint   pos;
}
SpicePattern;

typedef struct SpiceBrush
{
  uint32_t type;
  union
  {
    uint32_t     color;
    SpicePattern pattern;
  }
  u;
}
SpiceBrush;

typedef struct SpiceQMask
{
  uint8_t    flags;
  SpicePoint pos;
  SpiceImage * bitmap;
}
SpiceQMask;

typedef struct SpiceFill
{
  SpiceBrush brush;
  uint16_t rop_descriptor;
  SpiceQMask mask;
}
SpiceFill;

typedef struct SpiceRect
{
  int32_t top;
  int32_t left;
  int32_t bottom;
  int32_t right;
}
SpiceRect;

typedef struct SpiceClipRects
{
  uint32_t num_rects;
  SpiceRect rects[];
}
SpiceClipRects;

typedef struct SpiceClip
{
  uint8_t          type;
  SpiceClipRects * rects;
}
SpiceClip;

typedef struct SpiceCopy
{
  SpiceImage * src_bitmap;
  SpiceRect    src_area;
  uint16_t     rop_descriptor;
  uint8_t      scale_mode;
  SpiceQMask   mask;
}
SpiceCopy,
SpiceBlend;

#pragma pack(pop)

#endif
