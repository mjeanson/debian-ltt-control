/*
 * liblttdvfs header file
 *
 * Copyright 2010 - Oumarou Dicko <oumarou.dicko@polymtl.ca>
 *		    Michael Sills-Lavoie <michael.sills-lavoie@polymtl.ca>
 *                  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _LIBLTTDVFS_H
#define _LIBLTTDVFS_H

#include "liblttd.h"

/**
 * liblttdvfs_new_callbacks - Is a utility function called to create a new
 * callbacks struct used by liblttd to write trace data to the virtual file
 * system.
 *
 * @trace_name:   Directory name of the trace to write to. It will be created.
 * @append_mode:  Append to a possibly existing trace.
 * @verbose_mode: Verbose mode.
 *
 * Returns the callbacks if the function succeeds else NULL.
 */
struct liblttd_callbacks*
liblttdvfs_new_callbacks(char* trace_name, int append_mode, int verbose_mode);

#endif /*_LIBLTTDVFS_H */
