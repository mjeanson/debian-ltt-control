/* liblttd
 *
 * Linux Trace Toolkit Daemon
 *
 * This is a simple daemon that reads a few relay+debugfs channels and save
 * them in a trace.
 *
 * CPU hot-plugging is supported using inotify.
 *
 * Copyright 2005 -
 * 	Mathieu Desnoyers <mathieu.desnoyers@polymtl.ca>
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

#include "liblttd.h"

#define _REENTRANT
#define _GNU_SOURCE
#include <features.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <asm/ioctls.h>

#include <linux/version.h>

/* Relayfs IOCTL */
#include <asm/ioctl.h>
#include <asm/types.h>

/* Get the next sub buffer that can be read. */
#define RELAY_GET_SB		_IOR(0xF5, 0x00,__u32)
/* Release the oldest reserved (by "get") sub buffer. */
#define RELAY_PUT_SB		_IOW(0xF5, 0x01,__u32)
/* returns the number of sub buffers in the per cpu channel. */
#define RELAY_GET_N_SB		_IOR(0xF5, 0x02,__u32)
/* returns the size of the current sub buffer. */
#define RELAY_GET_SB_SIZE	_IOR(0xF5, 0x03, __u32)
/* returns the size of data to consume in the current sub-buffer. */
#define RELAY_GET_MAX_SB_SIZE	_IOR(0xF5, 0x04, __u32)


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,14)
#include <sys/inotify.h>

#define HAS_INOTIFY
#else
static inline int inotify_init (void)
{
	return -1;
}

static inline int inotify_add_watch (int fd, const char *name, __u32 mask)
{
	return 0;
}

static inline int inotify_rm_watch (int fd, __u32 wd)
{
	return 0;
}
#undef HAS_INOTIFY
#endif

struct liblttd_thread_data {
	int thread_num;
	struct liblttd_instance *instance;
};

#define printf_verbose(fmt, args...) \
  do {                               \
    if (instance->verbose_mode)      \
      printf(fmt, ##args);           \
  } while (0)


int open_buffer_file(struct liblttd_instance *instance, char *filename,
	char *path_channel, char *base_path_channel)
{
	int open_ret = 0;
	int ret = 0;
	int fd;

	if (strncmp(filename, "flight-", sizeof("flight-")-1) != 0) {
		if (instance->dump_flight_only) {
			printf_verbose("Skipping normal channel %s\n",
				path_channel);
			return 0;
		}
	} else {
		if (instance->dump_normal_only) {
			printf_verbose("Skipping flight channel %s\n",
				path_channel);
			return 0;
		}
	}
	printf_verbose("Opening file.\n");

	instance->fd_pairs.pair = realloc(instance->fd_pairs.pair,
			++instance->fd_pairs.num_pairs * sizeof(struct fd_pair));

	/* Open the channel in read mode */
	fd = open(path_channel, O_RDONLY | O_NONBLOCK);
	instance->fd_pairs.pair[instance->fd_pairs.num_pairs-1].channel = fd;

	if (instance->fd_pairs.pair[instance->fd_pairs.num_pairs-1].channel == -1) {
		perror(path_channel);
		instance->fd_pairs.num_pairs--;
		return 0;	/* continue */
	}

	if (instance->callbacks->on_open_channel) ret = instance->callbacks->on_open_channel(
			instance->callbacks, &instance->fd_pairs.pair[instance->fd_pairs.num_pairs-1],
			base_path_channel);

	if (ret != 0) {
		open_ret = -1;
		close(instance->fd_pairs.pair[instance->fd_pairs.num_pairs-1].channel);
		instance->fd_pairs.num_pairs--;
		goto end;
	}

end:
	return open_ret;
}

int open_channel_trace_pairs(struct liblttd_instance *instance,
	char *subchannel_name, char *base_subchannel_name)
{
	DIR *channel_dir = opendir(subchannel_name);
	struct dirent *entry;
	struct stat stat_buf;
	int ret;
	char path_channel[PATH_MAX];
	int path_channel_len;
	char *path_channel_ptr;
	char *base_subchannel_ptr;

	int open_ret = 0;

	if (channel_dir == NULL) {
		perror(subchannel_name);
		open_ret = ENOENT;
		goto end;
	}

	printf_verbose("Calling : on new channels folder\n");
	if (instance->callbacks->on_new_channels_folder) ret = instance->callbacks->
			on_new_channels_folder(instance->callbacks,
			base_subchannel_name);
	if (ret == -1) {
		open_ret = -1;
		goto end;
	}

	strncpy(path_channel, subchannel_name, PATH_MAX-1);
	path_channel_len = strlen(path_channel);
	path_channel[path_channel_len] = '/';
	path_channel_len++;
	path_channel_ptr = path_channel + path_channel_len;
	base_subchannel_ptr = path_channel +
		(base_subchannel_name - subchannel_name);

#ifdef HAS_INOTIFY
	instance->inotify_watch_array.elem = realloc(instance->inotify_watch_array.elem,
		++instance->inotify_watch_array.num * sizeof(struct inotify_watch));

	printf_verbose("Adding inotify for channel %s\n", path_channel);
	instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].wd = inotify_add_watch(instance->inotify_fd, path_channel, IN_CREATE);
	strcpy(instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].path_channel, path_channel);
	instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].base_path_channel =
		instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].path_channel +
		(base_subchannel_name - subchannel_name);
	printf_verbose("Added inotify for channel %s, wd %u\n",
		instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].path_channel,
		instance->inotify_watch_array.elem[instance->inotify_watch_array.num-1].wd);
#endif

	while((entry = readdir(channel_dir)) != NULL) {

		if (entry->d_name[0] == '.') continue;

		strncpy(path_channel_ptr, entry->d_name, PATH_MAX - path_channel_len);

		ret = stat(path_channel, &stat_buf);
		if (ret == -1) {
			perror(path_channel);
			continue;
		}

		printf_verbose("Channel file : %s\n", path_channel);

		if (S_ISDIR(stat_buf.st_mode)) {

			printf_verbose("Entering channel subdirectory...\n");
			ret = open_channel_trace_pairs(instance, path_channel, base_subchannel_ptr);
			if (ret < 0) continue;
		} else if (S_ISREG(stat_buf.st_mode)) {
			open_ret = open_buffer_file(instance, entry->d_name,
				path_channel, base_subchannel_ptr);
			if (open_ret)
				goto end;
		}
	}

end:
	closedir(channel_dir);

	return open_ret;
}


int read_subbuffer(struct liblttd_instance *instance, struct fd_pair *pair)
{
	unsigned int consumed_old, len;
	int err;
	long ret;
	off_t offset;

	err = ioctl(pair->channel, RELAY_GET_SB, &consumed_old);
	printf_verbose("cookie : %u\n", consumed_old);
	if (err != 0) {
		ret = errno;
		perror("Reserving sub buffer failed (everything is normal, it is due to concurrency)");
		goto get_error;
	}

	err = ioctl(pair->channel, RELAY_GET_SB_SIZE, &len);
	if (err != 0) {
		ret = errno;
		perror("Getting sub-buffer len failed.");
		goto get_error;
	}

	if (instance->callbacks->on_read_subbuffer)
		ret = instance->callbacks->on_read_subbuffer(
			instance->callbacks, pair, len);

write_error:
	ret = 0;
	err = ioctl(pair->channel, RELAY_PUT_SB, &consumed_old);
	if (err != 0) {
		ret = errno;
		if (errno == EFAULT) {
			perror("Error in unreserving sub buffer\n");
		} else if (errno == EIO) {
			/* Should never happen with newer LTTng versions */
			perror("Reader has been pushed by the writer, last sub-buffer corrupted.");
		}
		goto get_error;
	}

get_error:
	return ret;
}


int map_channels(struct liblttd_instance *instance, int idx_begin, int idx_end)
{
	int i,j;
	int ret=0;

	if (instance->fd_pairs.num_pairs <= 0) {
		printf("No channel to read\n");
		goto end;
	}

	/* Get the subbuf sizes and number */

	for(i=idx_begin;i<idx_end;i++) {
		struct fd_pair *pair = &instance->fd_pairs.pair[i];

		ret = ioctl(pair->channel, RELAY_GET_N_SB, &pair->n_sb);
		if (ret != 0) {
			perror("Error in getting the number of sub-buffers");
			goto end;
		}
		ret = ioctl(pair->channel, RELAY_GET_MAX_SB_SIZE,
			    &pair->max_sb_size);
		if (ret != 0) {
			perror("Error in getting the max sub-buffer size");
			goto end;
		}
		ret = pthread_mutex_init(&pair->mutex, NULL);	/* Fast mutex */
		if (ret != 0) {
			perror("Error in mutex init");
			goto end;
		}
	}

end:
	return ret;
}

int unmap_channels(struct liblttd_instance *instance)
{
	int j;
	int ret=0;

	/* Munmap each FD */
	for(j=0;j<instance->fd_pairs.num_pairs;j++) {
		struct fd_pair *pair = &instance->fd_pairs.pair[j];
		int err_ret;

		err_ret = pthread_mutex_destroy(&pair->mutex);
		if (err_ret != 0) {
			perror("Error in mutex destroy");
		}
		ret |= err_ret;
	}

	return ret;
}

#ifdef HAS_INOTIFY
/* Inotify event arrived.
 *
 * Only support add file for now.
 */
int read_inotify(struct liblttd_instance *instance)
{
	char buf[sizeof(struct inotify_event) + PATH_MAX];
	char path_channel[PATH_MAX];
	ssize_t len;
	struct inotify_event *ievent;
	size_t offset;
	unsigned int i;
	int ret;
	int old_num;

	offset = 0;
	len = read(instance->inotify_fd, buf, sizeof(struct inotify_event) + PATH_MAX);
	if (len < 0) {

		if (errno == EAGAIN)
			return 0;  /* another thread got the data before us */

		printf("Error in read from inotify FD %s.\n", strerror(len));
		return -1;
	}
	while(offset < len) {
		ievent = (struct inotify_event *)&(buf[offset]);
		for(i=0; i<instance->inotify_watch_array.num; i++) {
			if (instance->inotify_watch_array.elem[i].wd == ievent->wd &&
				ievent->mask == IN_CREATE) {
				printf_verbose(
					"inotify wd %u event mask : %u for %s%s\n",
					ievent->wd, ievent->mask,
					instance->inotify_watch_array.elem[i].path_channel,
					ievent->name);
				old_num = instance->fd_pairs.num_pairs;
				strcpy(path_channel, instance->inotify_watch_array.elem[i].path_channel);
				strcat(path_channel, ievent->name);
				if (ret = open_buffer_file(instance, ievent->name, path_channel,
					path_channel + (instance->inotify_watch_array.elem[i].base_path_channel -
					instance->inotify_watch_array.elem[i].path_channel))) {
					printf("Error opening buffer file\n");
					return -1;
				}
				if (ret = map_channels(instance, old_num, instance->fd_pairs.num_pairs)) {
					printf("Error mapping channel\n");
					return -1;
				}

			}
		}
		offset += sizeof(*ievent) + ievent->len;
	}
}
#endif //HAS_INOTIFY

/*
 * read_channels
 *
 * Thread worker.
 *
 * Read the debugfs channels and write them in the paired tracefiles.
 *
 * @fd_pairs : paired channels and trace files.
 *
 * returns 0 on success, -1 on error.
 *
 * Note that the high priority polled channels are consumed first. We then poll
 * again to see if these channels are still in priority. Only when no
 * high priority channel is left, we start reading low priority channels.
 *
 * Note that a channel is considered high priority when the buffer is almost
 * full.
 */

int read_channels(struct liblttd_instance *instance, unsigned long thread_num)
{
	struct pollfd *pollfd = NULL;
	int num_pollfd;
	int i,j;
	int num_rdy, num_hup;
	int high_prio;
	int ret = 0;
	int inotify_fds;
	unsigned int old_num;

#ifdef HAS_INOTIFY
	inotify_fds = 1;
#else
	inotify_fds = 0;
#endif

	pthread_rwlock_rdlock(&instance->fd_pairs_lock);

	/* Start polling the FD. Keep one fd for inotify */
	pollfd = malloc((inotify_fds + instance->fd_pairs.num_pairs) * sizeof(struct pollfd));

#ifdef HAS_INOTIFY
	pollfd[0].fd = instance->inotify_fd;
	pollfd[0].events = POLLIN|POLLPRI;
#endif

	for(i=0;i<instance->fd_pairs.num_pairs;i++) {
		pollfd[inotify_fds+i].fd = instance->fd_pairs.pair[i].channel;
		pollfd[inotify_fds+i].events = POLLIN|POLLPRI;
	}
	num_pollfd = inotify_fds + instance->fd_pairs.num_pairs;


	pthread_rwlock_unlock(&instance->fd_pairs_lock);

	while(1) {
		high_prio = 0;
		num_hup = 0;
#ifdef DEBUG
		printf("Press a key for next poll...\n");
		char buf[1];
		read(STDIN_FILENO, &buf, 1);
		printf("Next poll (polling %d fd) :\n", num_pollfd);
#endif //DEBUG

		/* Have we received a signal ? */
		if (instance->quit_program) break;

		num_rdy = poll(pollfd, num_pollfd, -1);

		if (num_rdy == -1) {
			perror("Poll error");
			goto free_fd;
		}

		printf_verbose("Data received\n");
#ifdef HAS_INOTIFY
		switch(pollfd[0].revents) {
			case POLLERR:
				printf_verbose(
					"Error returned in polling inotify fd %d.\n",
					pollfd[0].fd);
				break;
			case POLLHUP:
				printf_verbose(
					"Polling inotify fd %d tells it has hung up.\n",
					pollfd[0].fd);
				break;
			case POLLNVAL:
				printf_verbose(
					"Polling inotify fd %d tells fd is not open.\n",
					pollfd[0].fd);
				break;
			case POLLPRI:
			case POLLIN:
				printf_verbose(
					"Polling inotify fd %d : data ready.\n",
					pollfd[0].fd);

				pthread_rwlock_wrlock(&instance->fd_pairs_lock);
				read_inotify(instance);
				pthread_rwlock_unlock(&instance->fd_pairs_lock);

			break;
		}
#endif

		for(i=inotify_fds;i<num_pollfd;i++) {
			switch(pollfd[i].revents) {
				case POLLERR:
					printf_verbose(
						"Error returned in polling fd %d.\n",
						pollfd[i].fd);
					num_hup++;
					break;
				case POLLHUP:
					printf_verbose(
						"Polling fd %d tells it has hung up.\n",
						pollfd[i].fd);
					num_hup++;
					break;
				case POLLNVAL:
					printf_verbose(
						"Polling fd %d tells fd is not open.\n",
						pollfd[i].fd);
					num_hup++;
					break;
				case POLLPRI:
					pthread_rwlock_rdlock(&instance->fd_pairs_lock);
					if (pthread_mutex_trylock(&instance->fd_pairs.pair[i-inotify_fds].mutex) == 0) {
						printf_verbose(
							"Urgent read on fd %d\n",
							pollfd[i].fd);
						/* Take care of high priority channels first. */
						high_prio = 1;
						/* it's ok to have an unavailable sub-buffer */
						ret = read_subbuffer(instance, &instance->fd_pairs.pair[i-inotify_fds]);
						if (ret == EAGAIN) ret = 0;

						ret = pthread_mutex_unlock(&instance->fd_pairs.pair[i-inotify_fds].mutex);
						if (ret)
							printf("Error in mutex unlock : %s\n", strerror(ret));
					}
					pthread_rwlock_unlock(&instance->fd_pairs_lock);
					break;
			}
		}
		/* If every buffer FD has hung up, we end the read loop here */
		if (num_hup == num_pollfd - inotify_fds) break;

		if (!high_prio) {
			for(i=inotify_fds;i<num_pollfd;i++) {
				switch(pollfd[i].revents) {
					case POLLIN:
						pthread_rwlock_rdlock(&instance->fd_pairs_lock);
						if (pthread_mutex_trylock(&instance->fd_pairs.pair[i-inotify_fds].mutex) == 0) {
							/* Take care of low priority channels. */
							printf_verbose(
								"Normal read on fd %d\n",
								pollfd[i].fd);
							/* it's ok to have an unavailable subbuffer */
							ret = read_subbuffer(instance, &instance->fd_pairs.pair[i-inotify_fds]);
							if (ret == EAGAIN) ret = 0;

							ret = pthread_mutex_unlock(&instance->fd_pairs.pair[i-inotify_fds].mutex);
							if (ret)
								printf("Error in mutex unlock : %s\n", strerror(ret));
						}
						pthread_rwlock_unlock(&instance->fd_pairs_lock);
						break;
				}
			}
		}

		/* Update pollfd array if an entry was added to fd_pairs */
		pthread_rwlock_rdlock(&instance->fd_pairs_lock);
		if ((inotify_fds + instance->fd_pairs.num_pairs) != num_pollfd) {
			pollfd = realloc(pollfd,
					(inotify_fds + instance->fd_pairs.num_pairs) * sizeof(struct pollfd));
			for(i=num_pollfd-inotify_fds;i<instance->fd_pairs.num_pairs;i++) {
				pollfd[inotify_fds+i].fd = instance->fd_pairs.pair[i].channel;
				pollfd[inotify_fds+i].events = POLLIN|POLLPRI;
			}
			num_pollfd = instance->fd_pairs.num_pairs + inotify_fds;
		}
		pthread_rwlock_unlock(&instance->fd_pairs_lock);

		/* NB: If the fd_pairs structure is updated by another thread from this
		 *     point forward, the current thread will wait in the poll without
		 *     monitoring the new channel. However, this thread will add the
		 *     new channel on next poll (and this should not take too much time
		 *     on a loaded system).
		 *
		 *     This event is quite unlikely and can only occur if a CPU is
		 *     hot-plugged while multple lttd threads are running.
		 */
	}

free_fd:
	free(pollfd);

end:
	return ret;
}


void close_channel_trace_pairs(struct liblttd_instance *instance)
{
	int i;
	int ret;

	for(i=0;i<instance->fd_pairs.num_pairs;i++) {
		ret = close(instance->fd_pairs.pair[i].channel);
		if (ret == -1) perror("Close error on channel");
		if (instance->callbacks->on_close_channel) {
			ret = instance->callbacks->on_close_channel(
				instance->callbacks, &instance->fd_pairs.pair[i]);
			if (ret != 0) perror("Error on close channel callback");
		}
	}
	free(instance->fd_pairs.pair);
	free(instance->inotify_watch_array.elem);
}

/* Thread worker */
void * thread_main(void *arg)
{
	long ret = 0;
	struct liblttd_thread_data *thread_data = (struct liblttd_thread_data*) arg;

	if (thread_data->instance->callbacks->on_new_thread)
		ret = thread_data->instance->callbacks->on_new_thread(
		thread_data->instance->callbacks, thread_data->thread_num);

	if (ret < 0) {
		return (void*)ret;
	}
	ret = read_channels(thread_data->instance, thread_data->thread_num);

	if (thread_data->instance->callbacks->on_close_thread)
		thread_data->instance->callbacks->on_close_thread(
		thread_data->instance->callbacks, thread_data->thread_num);

	free(thread_data);

	return (void*)ret;
}

int channels_init(struct liblttd_instance *instance)
{
	int ret = 0;

	instance->inotify_fd = inotify_init();
	fcntl(instance->inotify_fd, F_SETFL, O_NONBLOCK);

	if (ret = open_channel_trace_pairs(instance, instance->channel_name,
			instance->channel_name +
			strlen(instance->channel_name)))
		goto close_channel;
	if (instance->fd_pairs.num_pairs == 0) {
		printf("No channel available for reading, exiting\n");
		ret = -ENOENT;
		goto close_channel;
	}

	if (ret = map_channels(instance, 0, instance->fd_pairs.num_pairs))
		goto close_channel;
	return 0;

close_channel:
	close_channel_trace_pairs(instance);
	if (instance->inotify_fd >= 0)
		close(instance->inotify_fd);
	return ret;
}

int delete_instance(struct liblttd_instance *instance)
{
	pthread_rwlock_destroy(&instance->fd_pairs_lock);
	free(instance);
	return 0;
}

int liblttd_start_instance(struct liblttd_instance *instance)
{
	int ret = 0;
	pthread_t *tids;
	unsigned long i;
	void *tret;

	if (!instance)
		return -EINVAL;

	if (ret = channels_init(instance))
		return ret;

	tids = malloc(sizeof(pthread_t) * instance->num_threads);
	for(i=0; i<instance->num_threads; i++) {
		struct liblttd_thread_data *thread_data =
			malloc(sizeof(struct liblttd_thread_data));
		thread_data->thread_num = i;
		thread_data->instance = instance;

		ret = pthread_create(&tids[i], NULL, thread_main, thread_data);
		if (ret) {
			perror("Error creating thread");
			break;
		}
	}

	for(i=0; i<instance->num_threads; i++) {
		ret = pthread_join(tids[i], &tret);
		if (ret) {
			perror("Error joining thread");
			break;
		}
		if ((long)tret != 0) {
			printf("Error %s occured in thread %ld\n",
				strerror((long)tret), i);
		}
	}

	free(tids);
	ret = unmap_channels(instance);
	close_channel_trace_pairs(instance);
	if (instance->inotify_fd >= 0)
		close(instance->inotify_fd);

	if (instance->callbacks->on_trace_end)
		instance->callbacks->on_trace_end(instance);

	delete_instance(instance);

	return ret;
}

struct liblttd_instance * liblttd_new_instance(
	struct liblttd_callbacks *callbacks, char *channel_path,
	unsigned long n_threads, int flight_only, int normal_only, int verbose)
{
	struct liblttd_instance * instance;

	if (!channel_path || !callbacks)
		return NULL;
	if (n_threads == 0)
		n_threads = 1;
	if (flight_only && normal_only)
		return NULL;

	instance = malloc(sizeof(struct liblttd_instance));
	if (!instance)
		return NULL;

	instance->callbacks = callbacks;

	instance->inotify_fd = -1;

	instance->fd_pairs.pair = NULL;
	instance->fd_pairs.num_pairs = 0;

	instance->inotify_watch_array.elem = NULL;
	instance->inotify_watch_array.num = 0;

	pthread_rwlock_init(&instance->fd_pairs_lock, NULL);

	strncpy(instance->channel_name, channel_path, PATH_MAX -1);
	instance->num_threads = n_threads;
	instance->dump_flight_only = flight_only;
	instance->dump_normal_only = normal_only;
	instance->verbose_mode = verbose;
	instance->quit_program = 0;

	return instance;
}

int liblttd_stop_instance(struct liblttd_instance *instance)
{
	instance->quit_program = 1;
	return 0;
}

