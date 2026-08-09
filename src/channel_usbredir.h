/**
 * PureSpice - A pure C implementation of the SPICE client protocol
 * Copyright © 2017-2026 Geoffrey McRae <geoff@hostfission.com>
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

#ifndef _H_PURESPICE_CHANNEL_USBREDIR_
#define _H_PURESPICE_CHANNEL_USBREDIR_

#include "ps.h"

void channelUSBRedir_init(void);
void channelUSBRedir_sessionStart(void);

PS_STATUS channelUSBRedir_reconcile(
    const SpiceChannelID * channels, size_t count);

void channelUSBRedir_shutdownBegin(void);
void channelUSBRedir_shutdownEnd(void);
void channelUSBRedir_finishShutdown(void);

#endif
