/*
 * liblttdvfs
 *
 * Linux Trace Toolkit library - Write trace to the virtual file system
 *
 * This is a simple daemonized library that reads a few LTTng debugfs channels
 * and save them in a trace.
 *
 * CPU hot-plugging is supported using inotify.
 *
 * Copyright 2005-2010 -
 * 	Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright 2010 -
 *	Michael Sills-Lavoie <michael.sills-lavoie@polymtl.ca>
 *	Oumarou Dicko <oumarou.dicko@polymtl.ca>
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _REENTRANT
#define _GNU_SOURCE
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#include "liblttdvfs.h"

struct liblttdvfs_channel_data {
	int trace;
};

struct liblttdvfs_data {
	char path_trace[PATH_MAX];
	char *end_path_trace;
	int path_trace_len;
	int append_mode;
	int verbose_mode;
};

static __thread int thread_pipe[2];

#define printf_verbose(fmt, args...) \
  do {                               \
    if (callbacks_data->verbose_mode)                \
      printf(fmt, ##args);           \
  } while (0)

int liblttdvfs_on_open_channel(struct liblttd_callbacks *data, struct fd_pair *pair, char *relative_channel_path)
{
	int open_ret = 0;
	int ret;
	struct stat stat_buf;
	struct liblttdvfs_channel_data *channel_data;
	off_t offset = 0;

	pair->user_data = malloc(sizeof(struct liblttdvfs_channel_data));
	channel_data = pair->user_data;

	struct liblttdvfs_data* callbacks_data = data->user_data;

	strncpy(callbacks_data->end_path_trace, relative_channel_path, PATH_MAX - callbacks_data->path_trace_len);
	printf_verbose("Creating trace file %s\n", callbacks_data->path_trace);

	ret = stat(callbacks_data->path_trace, &stat_buf);
	if (ret == 0) {
		if (callbacks_data->append_mode) {
			printf_verbose("Appending to file %s as requested\n",
				callbacks_data->path_trace);

			channel_data->trace = open(callbacks_data->path_trace, O_WRONLY, S_IRWXU|S_IRWXG|S_IRWXO);
			if (channel_data->trace == -1) {
				perror(callbacks_data->path_trace);
				open_ret = -1;
				goto end;
			}
			offset = lseek(channel_data->trace, 0, SEEK_END);
			if (offset < 0) {
				perror(callbacks_data->path_trace);
				open_ret = -1;
				close(channel_data->trace);
				goto end;
			}
		} else {
			printf("File %s exists, cannot open. Try append mode.\n", callbacks_data->path_trace);
			open_ret = -1;
			goto end;
		}
	} else {
		if (errno == ENOENT) {
			channel_data->trace =
				open(callbacks_data->path_trace, O_WRONLY|O_CREAT|O_EXCL, S_IRWXU|S_IRWXG|S_IRWXO);
			if (channel_data->trace == -1) {
				perror(callbacks_data->path_trace);
				open_ret = -1;
				goto end;
			}
			offset = 0;
		} else {
			perror("Channel output file open");
			open_ret = -1;
			goto end;
		}
	}
end:
	return open_ret;

}

int liblttdvfs_on_close_channel(struct liblttd_callbacks *data, struct fd_pair *pair)
{
	int ret;
	ret = close(((struct liblttdvfs_channel_data *)(pair->user_data))->trace);
	free(pair->user_data);
	return ret;
}

int liblttdvfs_on_new_channels_folder(struct liblttd_callbacks *data, char *relative_folder_path)
{
	int ret;
	int open_ret = 0;
	struct liblttdvfs_data* callbacks_data = data->user_data;

	strncpy(callbacks_data->end_path_trace, relative_folder_path, PATH_MAX - callbacks_data->path_trace_len);
	printf_verbose("Creating trace subdirectory %s\n", callbacks_data->path_trace);

	ret = mkdir(callbacks_data->path_trace, S_IRWXU|S_IRWXG|S_IRWXO);
	if (ret == -1) {
		if (errno != EEXIST) {
			perror(callbacks_data->path_trace);
			open_ret = -1;
			goto end;
		}
	}

end:
	return open_ret;
}

int liblttdvfs_on_read_subbuffer(struct liblttd_callbacks *data, struct fd_pair *pair, unsigned int len)
{
	long ret;
	off_t offset = 0;
	off_t orig_offset = pair->offset;
	int outfd = ((struct liblttdvfs_channel_data *)(pair->user_data))->trace;

	struct liblttdvfs_data* callbacks_data = data->user_data;

	while (len > 0) {
		printf_verbose("splice chan to pipe offset %lu\n",
			(unsigned long)offset);
		ret = splice(pair->channel, &offset, thread_pipe[1], NULL,
			len, SPLICE_F_MOVE | SPLICE_F_MORE);
		printf_verbose("splice chan to pipe ret %ld\n", ret);
		if (ret < 0) {
			perror("Error in relay splice");
			goto write_end;
		}
		ret = splice(thread_pipe[0], NULL, outfd,
			NULL, ret, SPLICE_F_MOVE | SPLICE_F_MORE);
		printf_verbose("splice pipe to file %ld\n", ret);
		if (ret < 0) {
			perror("Error in file splice");
			goto write_end;
		}
		len -= ret;
		/* This won't block, but will start writeout asynchronously */
		sync_file_range(outfd, pair->offset, ret,
				SYNC_FILE_RANGE_WRITE);
		pair->offset += ret;
	}
write_end:
	/*
	 * This does a blocking write-and-wait on any page that belongs to the
	 * subbuffer prior to the one we just wrote.
	 * Don't care about error values, as these are just hints and ways to
	 * limit the amount of page cache used.
	 */
	if (orig_offset >= pair->max_sb_size) {
		sync_file_range(outfd, orig_offset - pair->max_sb_size,
				pair->max_sb_size,
				SYNC_FILE_RANGE_WAIT_BEFORE
				| SYNC_FILE_RANGE_WRITE
				| SYNC_FILE_RANGE_WAIT_AFTER);
		/*
		 * Give hints to the kernel about how we access the file:
		 * POSIX_FADV_DONTNEED : we won't re-access data in a near
		 * future after we write it.
		 * We need to call fadvise again after the file grows because
		 * the kernel does not seem to apply fadvise to non-existing
		 * parts of the file.
		 * Call fadvise _after_ having waited for the page writeback to
		 * complete because the dirty page writeback semantic is not
		 * well defined. So it can be expected to lead to lower
		 * throughput in streaming.
		 */
		posix_fadvise(outfd, orig_offset - pair->max_sb_size,
			      pair->max_sb_size, POSIX_FADV_DONTNEED);
	}

	return ret;
}

int liblttdvfs_on_new_thread(struct liblttd_callbacks *data, unsigned long thread_num)
{
	int ret;
	ret = pipe(thread_pipe);
	if (ret < 0) {
		perror("Error creating pipe");
		return ret;
	}
	return 0;
}

int liblttdvfs_on_close_thread(struct liblttd_callbacks *data, unsigned long thread_num)
{
	close(thread_pipe[0]);	/* close read end */
	close(thread_pipe[1]);	/* close write end */
	return 0;
}

int liblttdvfs_on_trace_end(struct liblttd_instance *instance)
{
	struct liblttd_callbacks *callbacks = instance->callbacks;
	struct liblttdvfs_data *data = callbacks->user_data;

	free(data);
	free(callbacks);
}

struct liblttd_callbacks* liblttdvfs_new_callbacks(char* trace_name,
	int append_mode, int verbose_mode)
{
	struct liblttdvfs_data *data;
	struct liblttd_callbacks *callbacks;

	if (!trace_name)
		goto error;

	data = malloc(sizeof(struct liblttdvfs_data));
	if (!data)
		goto error;

	strncpy(data->path_trace, trace_name, PATH_MAX-1);
	data->path_trace_len = strlen(data->path_trace);
	data->end_path_trace = data->path_trace + data->path_trace_len;
	data->append_mode = append_mode;
	data->verbose_mode = verbose_mode;

	callbacks = malloc(sizeof(struct liblttd_callbacks));
	if (!callbacks)
		goto alloc_cb_error;

	callbacks->on_open_channel = liblttdvfs_on_open_channel;
	callbacks->on_close_channel = liblttdvfs_on_close_channel;
	callbacks->on_new_channels_folder = liblttdvfs_on_new_channels_folder;
	callbacks->on_read_subbuffer = liblttdvfs_on_read_subbuffer;
	callbacks->on_trace_end = liblttdvfs_on_trace_end;
	callbacks->on_new_thread = liblttdvfs_on_new_thread;
	callbacks->on_close_thread = liblttdvfs_on_close_thread;
	callbacks->user_data = data;

	return callbacks;

	/* Error handling */
alloc_cb_error:
	free(data);
error:
	return NULL;
}
