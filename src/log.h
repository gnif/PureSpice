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

#ifndef _H_I_LOG_
#define _H_I_LOG_

#include "ps.h"

#define _PS_LOG(func, fmt, ...) do { \
  func(__FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
} while(0);

#define PS_LOG_INFO(fmt, ...)  _PS_LOG(g_ps.init.log.info , fmt, ##__VA_ARGS__)
#define PS_LOG_WARN(fmt, ...)  _PS_LOG(g_ps.init.log.warn , fmt, ##__VA_ARGS__)
#define PS_LOG_ERROR(fmt, ...) _PS_LOG(g_ps.init.log.error, fmt, ##__VA_ARGS__)

#define PS_LOG_INFO_ONCE(fmt, ...) do { \
  static char first = 1; \
  if (first) \
  { \
    first = 0; \
    PS_LOG_INFO(fmt, ##__VA_ARGS__) \
  } \
} while(0)

#define PS_LOG_WARN_ONCE(fmt, ...) do { \
  static char first = 1; \
  if (first) \
  { \
    first = 0; \
    PS_LOG_WARN(fmt, ##__VA_ARGS__) \
  } \
} while(0)

#define PS_LOG_ERROR_ONCE(fmt, ...) do { \
  static char first = 1; \
  if (first) \
  { \
    first = 0; \
    PS_LOG_ERROR(fmt, ##__VA_ARGS__) \
  } \
} while(0)

void log_init(void);

#endif
