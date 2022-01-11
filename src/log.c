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

#include "log.h"
#include "ps.h"

#include <stdarg.h>
#include <stdio.h>

static void log_stdout(const char * file, unsigned int line,
    const char * function, const char * format, ...)
{
  va_list va;

  const char * f = strrchr(file, '/') + 1;
  fprintf(stdout, "%s:%d (%s): ", f, line, function);

  va_start(va, format);
  vfprintf(stdout, format, va);
  va_end(va);
  fputc('\n', stdout);
}

static void log_stderr(const char * file, unsigned int line,
    const char * function, const char * format, ...)
{
  va_list va;

  const char * f = strrchr(file, '/') + 1;
  fprintf(stderr, "%s:%d (%s): ", f, line, function);

  va_start(va, format);
  vfprintf(stderr, format, va);
  va_end(va);
  fputc('\n', stderr);
}

void log_init(void)
{
  if (!g_ps.init.log.info)
    g_ps.init.log.info  = log_stdout;

  if (!g_ps.init.log.warn)
    g_ps.init.log.warn  = log_stdout;

  if (!g_ps.init.log.error)
    g_ps.init.log.error = log_stderr;
}

#endif
