/*
 * Copyright (C) 2011 - Julien Desfossez <julien.desfossez@polymtl.ca>
 * Copyright (C) 2011 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _LTTNG_USTCONSUMER_H
#define _LTTNG_USTCONSUMER_H

#include <config.h>
#include <errno.h>

#include <common/consumer.h>

#ifdef HAVE_LIBLTTNG_UST_CTL

/*
 * Take a snapshot for a specific fd
 *
 * Returns 0 on success, < 0 on error
 */
int lttng_ustconsumer_take_snapshot(struct lttng_consumer_local_data *ctx,
        struct lttng_consumer_stream *stream);

/*
 * Get the produced position
 *
 * Returns 0 on success, < 0 on error
 */
int lttng_ustconsumer_get_produced_snapshot(
        struct lttng_consumer_local_data *ctx,
        struct lttng_consumer_stream *stream,
        unsigned long *pos);

int lttng_ustconsumer_recv_cmd(struct lttng_consumer_local_data *ctx,
		int sock, struct pollfd *consumer_sockpoll);

extern int lttng_ustconsumer_allocate_channel(struct lttng_consumer_channel *chan);
extern void lttng_ustconsumer_del_channel(struct lttng_consumer_channel *chan);
extern int lttng_ustconsumer_allocate_stream(struct lttng_consumer_stream *stream);
extern void lttng_ustconsumer_del_stream(struct lttng_consumer_stream *stream);

int lttng_ustconsumer_read_subbuffer(struct lttng_consumer_stream *stream,
		struct lttng_consumer_local_data *ctx);
int lttng_ustconsumer_on_recv_stream(struct lttng_consumer_stream *stream);

void lttng_ustconsumer_on_stream_hangup(struct lttng_consumer_stream *stream);

extern int lttng_ustctl_get_mmap_read_offset(
		struct lttng_ust_shm_handle *handle,
		struct lttng_ust_lib_ring_buffer *buf, unsigned long *off);

#else /* HAVE_LIBLTTNG_UST_CTL */

static inline
ssize_t lttng_ustconsumer_on_read_subbuffer_mmap(
		struct lttng_consumer_local_data *ctx,
		struct lttng_consumer_stream *stream, unsigned long len)
{
	return -ENOSYS;
}

static inline
ssize_t lttng_ustconsumer_on_read_subbuffer_splice(
		struct lttng_consumer_local_data *ctx,
		struct lttng_consumer_stream *uststream, unsigned long len)
{
	return -ENOSYS;
}

static inline
int lttng_ustconsumer_take_snapshot(struct lttng_consumer_local_data *ctx,
        struct lttng_consumer_stream *stream)
{
	return -ENOSYS;
}

static inline
int lttng_ustconsumer_get_produced_snapshot(
        struct lttng_consumer_local_data *ctx,
        struct lttng_consumer_stream *stream,
        unsigned long *pos)
{
	return -ENOSYS;
}

static inline
int lttng_ustconsumer_recv_cmd(struct lttng_consumer_local_data *ctx,
		int sock, struct pollfd *consumer_sockpoll)
{
	return -ENOSYS;
}

static inline
int lttng_ustconsumer_allocate_channel(struct lttng_consumer_channel *chan)
{
	return -ENOSYS;
}

static inline
void lttng_ustconsumer_del_channel(struct lttng_consumer_channel *chan)
{
}

static inline
int lttng_ustconsumer_allocate_stream(struct lttng_consumer_stream *stream)
{
	return -ENOSYS;
}

static inline
void lttng_ustconsumer_del_stream(struct lttng_consumer_stream *stream)
{
}

static inline
int lttng_ustconsumer_read_subbuffer(struct lttng_consumer_stream *stream,
		struct lttng_consumer_local_data *ctx)
{
	return -ENOSYS;
}

static inline
int lttng_ustconsumer_on_recv_stream(struct lttng_consumer_stream *stream)
{
	return -ENOSYS;
}

static inline
void lttng_ustconsumer_on_stream_hangup(struct lttng_consumer_stream *stream)
{
}

static inline
int lttng_ustctl_get_mmap_read_offset(struct lttng_ust_shm_handle *handle,
		struct lttng_ust_lib_ring_buffer *buf, unsigned long *off)
{
	return -ENOSYS;
}
#endif /* HAVE_LIBLTTNG_UST_CTL */

#endif /* _LTTNG_USTCONSUMER_H */