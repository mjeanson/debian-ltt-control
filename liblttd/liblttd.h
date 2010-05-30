/*
 * liblttd header file
 *
 * Copyright 2005-2010 -
 * 		 Mathieu Desnoyers <mathieu.desnoyers@polymtl.ca>
 * Copyright 2010-
 *		 Oumarou Dicko <oumarou.dicko@polymtl.ca>
 *		 Michael Sills-Lavoie <michael.sills-lavoie@polymtl.ca>
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

#ifndef _LIBLTTD_H
#define _LIBLTTD_H

#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>

/**
 * struct fd_pair - Contains the data associated with the channel file
 * descriptor. The lib user can use user_data to store the data associated to
 * the specified channel. The lib user can read but MUST NOT change the other
 * attributes.
 * @channel: channel file descriptor
 * @n_sb: the number of subbuffer for this channel
 * @max_sb_size: the subbuffer size for this channel
 * @mmap: Not used anymore.
 * @mutex: a mutex for internal library usage
 * @user_data: library user data
 * @offset: write position in the output file descriptor (optional)
 */
struct fd_pair {
	int channel;
	unsigned int n_sb;
	unsigned int max_sb_size;
	void *mmap;
	pthread_mutex_t	mutex;
	void *user_data;
	off_t offset;
};

struct channel_trace_fd {
	struct fd_pair *pair;
	int num_pairs;
};

struct inotify_watch {
	int wd;
	char path_channel[PATH_MAX];
	char *base_path_channel;
};

struct inotify_watch_array {
	struct inotify_watch *elem;
	int num;
};

struct liblttd_callbacks;

/**
 * struct liblttd_instance - Contains the data associated with a trace instance.
 * The lib user can read but MUST NOT change any attributes but callbacks.
 * @callbacks: Contains the necessary callbacks for a tracing session.
 */
struct liblttd_instance {
	struct liblttd_callbacks *callbacks;

	int inotify_fd;
	struct channel_trace_fd fd_pairs;
	struct inotify_watch_array inotify_watch_array;

	/* protects fd_pairs and inotify_watch_array */
	pthread_rwlock_t fd_pairs_lock;

	char channel_name[PATH_MAX];
	unsigned long num_threads;
	int quit_program;
	int dump_flight_only;
	int dump_normal_only;
	int verbose_mode;
};

/**
* struct liblttd_callbacks - Contains the necessary callbacks for a tracing
* session. The user can set the unnecessary functions to NULL if he does not
* need them.
*/
struct liblttd_callbacks {
	/**
	 * on_open_channel - Is called after a channel file is open.
	 *
	 * @data: pointer to the callbacks structure that has been passed to
	 *        the lib.
	 * @pair: structure that contains the data associated with the
	 *        channel file descriptor. The library user can use user_data to
	 *        store the data associated to the specified channel.
	 * @relative_channel_path:
	 *        represents a relative path to the channel file. This path is
	 *        relative to the root folder of the trace channels.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 */
	int (*on_open_channel)(struct liblttd_callbacks *data,
			       struct fd_pair *pair,
			       char *relative_channel_path);

	/**
	 * on_close_channel - Is called after a channel file is closed.
	 *
	 * @data: pointer to the callbacks structure that has been passed to the
	 *        lib.
	 * @pair: structure that contains the data associated with the channel
	 *        file descriptor. The lib user should clean user_data at this
	 *        time.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 *
	 * After a channel file has been closed, it will never be read again.
	 */
	int (*on_close_channel)(struct liblttd_callbacks *data,
				struct fd_pair *pair);

	/**
	 * on_new_channels_folder - Is called when the library enter in a new
         * subfolder while it is scanning the trace channel tree. It can be used
         * to create the output file structure of the trace.
	 *
	 * @data: pointer to the callbacks structure that has been passed to the
	 *        library.
	 * @relative_folder_path:
	 *        represents a relative path to the channel folder. This path is
	 *        relative to the root folder of the trace channels.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 */
	int (*on_new_channels_folder)(struct liblttd_callbacks *data,
				      char *relative_folder_path);

	/**
	 * on_read_subbuffer - Is called after a subbuffer is a reserved.
	 *
	 * @data: pointer to the callbacks structure that has been passed to the
	 *        library.
	 * @pair: structure that contains the data associated with the channel
	 *        file descriptor.
	 * @len:  represents the length the data that has to be read.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 *
	 * It has to be thread safe, because it is called by many threads.
	 */
	int (*on_read_subbuffer)(struct liblttd_callbacks *data,
				struct fd_pair *pair, unsigned int len);

	/**
	 * on_trace_en - Is called at the very end of the tracing session. At
	 * this time, all the channels have been closed and the threads have
	 * been destroyed.
	 *
	 * @instance: pointer to the instance structure that has been passed to
	 *            the library.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 *
	 * After this callback is called, no other callback will be called
	 * again and the tracing instance will be deleted automatically by
	 * liblttd. After this call, the user must not use the liblttd instance.
	 */
	int (*on_trace_end)(struct liblttd_instance *instance);

	/**
	 * on_new_thread - Is called after a new thread has been created.
	 *
	 * @data: pointer to the callbacks structure that has been passed to the
	 *        lib.
	 * @thread_num: represents the id of the thread.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 *
	 * It has to be thread safe, because it is called by many threads.
	 */
	int (*on_new_thread)(struct liblttd_callbacks *data,
			     unsigned long thread_num);

	/**
	 * on_close_thread - Is called just before a thread is destroyed.
	 *
	 * @data: pointer to the callbacks structure that has been passed to the
	 *        library.
	 * @thread_num: represents the number of the thread.
	 *
	 * Returns 0 if the callback succeeds else not 0.
	 *
	 * It has to be thread safe, because it is called by many threads.
	 */
	int (*on_close_thread)(struct liblttd_callbacks *data,
			       unsigned long thread_num);

	/**
	 * The library's data.
	 */
	void *user_data;
};

/**
 * liblttd_new_instance - Is called to create a new tracing session.
 *
 * @callbacks:    Pointer to a callbacks structure that contain the user
 *                callbacks and data.
 * @channel_path: This argument is a path to the root folder of the trace's
 *                channels.
 * @n_threads:    This argument represents the number of threads that will be
 *                used by the library.
 * @flight_only:  If this argument to set to 1, only the channel that are in
 *                flight recorder mode will be recorded.
 * @normal_only:  If this argument to set to 1, only the channel that are in
 *                normal mode will be recorded.
 * @verbose:      If this argument to set to 1, more informations will be
 *                printed.
 *
 * Returns the instance if the function succeeds else NULL.
 */
struct liblttd_instance *
liblttd_new_instance(struct liblttd_callbacks *callbacks, char *channel_path,
		     unsigned long n_threads, int flight_only, int normal_only,
		     int verbose);

/**
 * liblttd_start - Is called to start a new tracing session.
 *
 * @instance: The tracing session instance that needs to be started.
 *
 * Returns 0 if the function succeeds.
 *
 * This is a blocking function. The caller will be blocked on it until the
 * tracing session is stopped by the user using liblttd_stop_instance or until
 * the trace is stopped by LTTng directly.
 */
int liblttd_start_instance(struct liblttd_instance *instance);

/**
 * liblttd_stop - Is called to stop a tracing session.
 *
 * @instance: The tracing session instance that needs to be stoped.
 *
 * Returns 0 if the function succeeds.
 *
 * This function returns immediately, it only tells liblttd to stop the
 * instance. The on_trace_end callback will be called when the tracing session
 * will really be stopped (after every thread has finished using it). The
 * instance is deleted automatically by liblttd after on_trace_end is called.
 */
int liblttd_stop_instance(struct liblttd_instance *instance);

#endif /*_LIBLTTD_H */
