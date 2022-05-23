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

#include "ps.h"

PS_STATUS channel_connect(PSChannel * channel);

void channel_internal_disconnect(PSChannel * channel);

void channel_disconnect(PSChannel * channel);

PSHandlerFn channel_onMessage(PSChannel * channel);

bool channel_ack(PSChannel * channel);

ssize_t channel_writeNL(const PSChannel * channel,
    const void * buffer, size_t size);

PS_STATUS channel_readNL(PSChannel * channel, void * buffer,
    size_t size, int * dataAvailable);

PS_STATUS channel_discardNL(PSChannel * channel,
    size_t size, int * dataAvailable);
