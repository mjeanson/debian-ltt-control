/*
 * Copyright (C) 2017 - Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 only, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _LGPL_SOURCE
#include <lttng/trigger/trigger.h>
#include <lttng/notification/channel-internal.h>
#include <lttng/notification/notification-internal.h>
#include <lttng/condition/condition-internal.h>
#include <lttng/condition/buffer-usage-internal.h>
#include <common/error.h>
#include <common/config/session-config.h>
#include <common/defaults.h>
#include <common/utils.h>
#include <common/align.h>
#include <common/time.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>

#include "notification-thread.h"
#include "notification-thread-events.h"
#include "notification-thread-commands.h"
#include "lttng-sessiond.h"
#include "health-sessiond.h"

#include <urcu.h>
#include <urcu/list.h>
#include <urcu/rculfhash.h>

/**
 * This thread maintains an internal state associating clients and triggers.
 *
 * In order to speed-up and simplify queries, hash tables providing the
 * following associations are maintained:
 *
 *   - client_socket_ht: associate a client's socket (fd) to its "struct client"
 *             This hash table owns the "struct client" which must thus be
 *             disposed-of on removal from the hash table.
 *
 *   - channel_triggers_ht:
 *             associates a channel key to a list of
 *             struct lttng_trigger_list_nodes. The triggers in this list are
 *             those that have conditions that apply to this channel.
 *             This hash table owns the list, but not the triggers themselves.
 *
 *   - channel_state_ht:
 *             associates a pair (channel key, channel domain) to its last
 *             sampled state received from the consumer daemon
 *             (struct channel_state).
 *             This previous sample is kept to implement edge-triggered
 *             conditions as we need to detect the state transitions.
 *             This hash table owns the channel state.
 *
 *   - notification_trigger_clients_ht:
 *             associates notification-emitting triggers to clients
 *             (struct notification_client_list) subscribed to those
 *             conditions.
 *             The condition's hash and match functions are used directly since
 *             all triggers in this hash table have the "notify" action.
 *             This hash table holds no ownership.
 *
 *   - channels_ht:
 *             associates a channel_key to a struct channel_info. The hash table
 *             holds the ownership of the struct channel_info.
 *
 *   - triggers_ht:
 *             associated a condition to a struct lttng_trigger_ht_element.
 *             The hash table holds the ownership of the
 *             lttng_trigger_ht_elements along with the triggers themselves.
 *
 * The thread reacts to the following internal events:
 *   1) creation of a tracing channel,
 *   2) destruction of a tracing channel,
 *   3) registration of a trigger,
 *   4) unregistration of a trigger,
 *   5) reception of a channel monitor sample from the consumer daemon.
 *
 * Events specific to notification-emitting triggers:
 *   6) connection of a notification client,
 *   7) disconnection of a notification client,
 *   8) subscription of a client to a conditions' notifications,
 *   9) unsubscription of a client from a conditions' notifications,
 *
 *
 * 1) Creation of a tracing channel
 *    - notification_trigger_clients_ht is traversed to identify
 *      triggers which apply to this new channel,
 *    - triggers identified are added to the channel_triggers_ht.
 *    - add channel to channels_ht
 *
 * 2) Destruction of a tracing channel
 *    - remove entry from channel_triggers_ht, releasing the list wrapper and
 *      elements,
 *    - remove entry from the channel_state_ht.
 *    - remove channel from channels_ht
 *
 * 3) Registration of a trigger
 *    - if the trigger's action is of type "notify",
 *      - traverse the list of conditions of every client to build a list of
 *        clients which have to be notified when this trigger's condition is met,
 *        - add list of clients (even if it is empty) to the
 *          notification_trigger_clients_ht,
 *    - add trigger to channel_triggers_ht (if applicable),
 *    - add trigger to triggers_ht
 *
 * 4) Unregistration of a trigger
 *    - if the trigger's action is of type "notify",
 *      - remove the trigger from the notification_trigger_clients_ht,
 *    - remove trigger from channel_triggers_ht (if applicable),
 *    - remove trigger from triggers_ht
 *
 * 5) Reception of a channel monitor sample from the consumer daemon
 *    - evaluate the conditions associated with the triggers found in
 *      the channel_triggers_ht,
 *      - if a condition evaluates to "true" and the condition is of type
 *        "notify", query the notification_trigger_clients_ht and send
 *        a notification to the clients.
 *
 * 6) Connection of a client
 *    - add client socket to the client_socket_ht.
 *
 * 7) Disconnection of a client
 *    - remove client socket from the client_socket_ht,
 *    - traverse all conditions to which the client is subscribed and remove
 *      the client from the notification_trigger_clients_ht.
 *
 * 8) Subscription of a client to a condition's notifications
 *    - Add the condition to the client's list of subscribed conditions,
 *    - Look-up notification_trigger_clients_ht and add the client to
 *      list of clients.
 *
 * 9) Unsubscription of a client to a condition's notifications
 *    - Remove the condition from the client's list of subscribed conditions,
 *    - Look-up notification_trigger_clients_ht and remove the client
 *      from the list of clients.
 */

/*
 * Destroy the thread data previously created by the init function.
 */
void notification_thread_handle_destroy(
		struct notification_thread_handle *handle)
{
	int ret;

	if (!handle) {
		goto end;
	}

	if (handle->cmd_queue.event_fd < 0) {
		goto end;
	}
	ret = close(handle->cmd_queue.event_fd);
	if (ret < 0) {
		PERROR("close notification command queue event_fd");
	}

	assert(cds_list_empty(&handle->cmd_queue.list));
	pthread_mutex_destroy(&handle->cmd_queue.lock);

	if (handle->channel_monitoring_pipes.ust32_consumer >= 0) {
		ret = close(handle->channel_monitoring_pipes.ust32_consumer);
		if (ret) {
			PERROR("close 32-bit consumer channel monitoring pipe");
		}
	}
	if (handle->channel_monitoring_pipes.ust64_consumer >= 0) {
		ret = close(handle->channel_monitoring_pipes.ust64_consumer);
		if (ret) {
			PERROR("close 64-bit consumer channel monitoring pipe");
		}
	}
	if (handle->channel_monitoring_pipes.kernel_consumer >= 0) {
		ret = close(handle->channel_monitoring_pipes.kernel_consumer);
		if (ret) {
			PERROR("close kernel consumer channel monitoring pipe");
		}
	}
end:
	free(handle);
}

struct notification_thread_handle *notification_thread_handle_create(
		struct lttng_pipe *ust32_channel_monitor_pipe,
		struct lttng_pipe *ust64_channel_monitor_pipe,
		struct lttng_pipe *kernel_channel_monitor_pipe)
{
	int ret;
	struct notification_thread_handle *handle;

	handle = zmalloc(sizeof(*handle));
	if (!handle) {
		goto end;
	}

	/* FIXME Replace eventfd by a pipe to support older kernels. */
	handle->cmd_queue.event_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
	if (handle->cmd_queue.event_fd < 0) {
		PERROR("eventfd notification command queue");
		goto error;
	}
	CDS_INIT_LIST_HEAD(&handle->cmd_queue.list);
	ret = pthread_mutex_init(&handle->cmd_queue.lock, NULL);
	if (ret) {
		goto error;
	}

	if (ust32_channel_monitor_pipe) {
		handle->channel_monitoring_pipes.ust32_consumer =
				lttng_pipe_release_readfd(
					ust32_channel_monitor_pipe);
		if (handle->channel_monitoring_pipes.ust32_consumer < 0) {
			goto error;
		}
	} else {
		handle->channel_monitoring_pipes.ust32_consumer = -1;
	}
	if (ust64_channel_monitor_pipe) {
		handle->channel_monitoring_pipes.ust64_consumer =
				lttng_pipe_release_readfd(
					ust64_channel_monitor_pipe);
		if (handle->channel_monitoring_pipes.ust64_consumer < 0) {
			goto error;
		}
	} else {
		handle->channel_monitoring_pipes.ust64_consumer = -1;
	}
	if (kernel_channel_monitor_pipe) {
		handle->channel_monitoring_pipes.kernel_consumer =
				lttng_pipe_release_readfd(
					kernel_channel_monitor_pipe);
		if (handle->channel_monitoring_pipes.kernel_consumer < 0) {
			goto error;
		}
	} else {
		handle->channel_monitoring_pipes.kernel_consumer = -1;
	}
end:
	return handle;
error:
	notification_thread_handle_destroy(handle);
	return NULL;
}

static
char *get_notification_channel_sock_path(void)
{
	int ret;
	bool is_root = !getuid();
	char *sock_path;

	sock_path = zmalloc(LTTNG_PATH_MAX);
	if (!sock_path) {
		goto error;
	}

	if (is_root) {
		ret = snprintf(sock_path, LTTNG_PATH_MAX,
				DEFAULT_GLOBAL_NOTIFICATION_CHANNEL_UNIX_SOCK);
		if (ret < 0) {
			goto error;
		}
	} else {
		char *home_path = utils_get_home_dir();

		if (!home_path) {
			ERR("Can't get HOME directory for socket creation");
			goto error;
		}

		ret = snprintf(sock_path, LTTNG_PATH_MAX,
				DEFAULT_HOME_NOTIFICATION_CHANNEL_UNIX_SOCK,
				home_path);
		if (ret < 0) {
			goto error;
		}
	}

	return sock_path;
error:
	free(sock_path);
	return NULL;
}

static
void notification_channel_socket_destroy(int fd)
{
	int ret;
	char *sock_path = get_notification_channel_sock_path();

	DBG("[notification-thread] Destroying notification channel socket");

	if (sock_path) {
		ret = unlink(sock_path);
		free(sock_path);
		if (ret < 0) {
			PERROR("unlink notification channel socket");
		}
	}

	ret = close(fd);
	if (ret) {
		PERROR("close notification channel socket");
	}
}

static
int notification_channel_socket_create(void)
{
	int fd = -1, ret;
	char *sock_path = get_notification_channel_sock_path();

	DBG("[notification-thread] Creating notification channel UNIX socket at %s",
			sock_path);

	ret = lttcomm_create_unix_sock(sock_path);
	if (ret < 0) {
		ERR("[notification-thread] Failed to create notification socket");
		goto error;
	}
	fd = ret;

	ret = chmod(sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", sock_path);
		PERROR("chmod notification channel socket");
		goto error;
	}

	if (getuid() == 0) {
		ret = chown(sock_path, 0,
				utils_get_group_id(tracing_group_name));
		if (ret) {
			ERR("Failed to set the notification channel socket's group");
			ret = -1;
			goto error;
		}
	}

	DBG("[notification-thread] Notification channel UNIX socket created (fd = %i)",
			fd);
	free(sock_path);
	return fd;
error:
	if (fd >= 0 && close(fd) < 0) {
		PERROR("close notification channel socket");
	}
	free(sock_path);
	return ret;
}

static
int init_poll_set(struct lttng_poll_event *poll_set,
		struct notification_thread_handle *handle,
		int notification_channel_socket)
{
	int ret;

	/*
	 * Create pollset with size 5:
	 *	- notification channel socket (listen for new connections),
	 *	- command queue event fd (internal sessiond commands),
	 *	- consumerd (32-bit user space) channel monitor pipe,
	 *	- consumerd (64-bit user space) channel monitor pipe,
	 *	- consumerd (kernel) channel monitor pipe.
	 */
	ret = lttng_poll_create(poll_set, 5, LTTNG_CLOEXEC);
	if (ret < 0) {
		goto end;
	}

	ret = lttng_poll_add(poll_set, notification_channel_socket,
			LPOLLIN | LPOLLERR | LPOLLHUP | LPOLLRDHUP);
	if (ret < 0) {
		ERR("[notification-thread] Failed to add notification channel socket to pollset");
		goto error;
	}
	ret = lttng_poll_add(poll_set, handle->cmd_queue.event_fd,
			LPOLLIN | LPOLLERR);
	if (ret < 0) {
		ERR("[notification-thread] Failed to add notification command queue event fd to pollset");
		goto error;
	}
	ret = lttng_poll_add(poll_set,
			handle->channel_monitoring_pipes.ust32_consumer,
			LPOLLIN | LPOLLERR);
	if (ret < 0) {
		ERR("[notification-thread] Failed to add ust-32 channel monitoring pipe fd to pollset");
		goto error;
	}
	ret = lttng_poll_add(poll_set,
			handle->channel_monitoring_pipes.ust64_consumer,
			LPOLLIN | LPOLLERR);
	if (ret < 0) {
		ERR("[notification-thread] Failed to add ust-64 channel monitoring pipe fd to pollset");
		goto error;
	}
	if (handle->channel_monitoring_pipes.kernel_consumer < 0) {
		goto end;
	}
	ret = lttng_poll_add(poll_set,
			handle->channel_monitoring_pipes.kernel_consumer,
			LPOLLIN | LPOLLERR);
	if (ret < 0) {
		ERR("[notification-thread] Failed to add kernel channel monitoring pipe fd to pollset");
		goto error;
	}
end:
	return ret;
error:
	lttng_poll_clean(poll_set);
	return ret;
}

static
void fini_thread_state(struct notification_thread_state *state)
{
	int ret;

	if (state->client_socket_ht) {
		ret = handle_notification_thread_client_disconnect_all(state);
		assert(!ret);
		ret = cds_lfht_destroy(state->client_socket_ht, NULL);
		assert(!ret);
	}
	if (state->triggers_ht) {
		ret = handle_notification_thread_trigger_unregister_all(state);
		assert(!ret);
		ret = cds_lfht_destroy(state->triggers_ht, NULL);
		assert(!ret);
	}
	if (state->channel_triggers_ht) {
		ret = cds_lfht_destroy(state->channel_triggers_ht, NULL);
		assert(!ret);
	}
	if (state->channel_state_ht) {
		ret = cds_lfht_destroy(state->channel_state_ht, NULL);
		assert(!ret);
	}
	if (state->notification_trigger_clients_ht) {
		ret = cds_lfht_destroy(state->notification_trigger_clients_ht,
				NULL);
		assert(!ret);
	}
	if (state->channels_ht) {
		ret = cds_lfht_destroy(state->channels_ht,
				NULL);
		assert(!ret);
	}

	if (state->notification_channel_socket >= 0) {
		notification_channel_socket_destroy(
				state->notification_channel_socket);
	}
	lttng_poll_clean(&state->events);
}

static
int init_thread_state(struct notification_thread_handle *handle,
		struct notification_thread_state *state)
{
	int ret;

	memset(state, 0, sizeof(*state));
	state->notification_channel_socket = -1;
	lttng_poll_init(&state->events);

	ret = notification_channel_socket_create();
	if (ret < 0) {
		goto end;
	}
	state->notification_channel_socket = ret;

	ret = init_poll_set(&state->events, handle,
			state->notification_channel_socket);
	if (ret) {
		goto end;
	}

	DBG("[notification-thread] Listening on notification channel socket");
	ret = lttcomm_listen_unix_sock(state->notification_channel_socket);
	if (ret < 0) {
		ERR("[notification-thread] Listen failed on notification channel socket");
		goto error;
	}

	state->client_socket_ht = cds_lfht_new(DEFAULT_HT_SIZE, 1, 0,
			CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->client_socket_ht) {
		goto error;
	}

	state->channel_triggers_ht = cds_lfht_new(DEFAULT_HT_SIZE, 1, 0,
			CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->channel_triggers_ht) {
		goto error;
	}

	state->channel_state_ht = cds_lfht_new(DEFAULT_HT_SIZE, 1, 0,
			CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->channel_state_ht) {
		goto error;
	}

	state->notification_trigger_clients_ht = cds_lfht_new(DEFAULT_HT_SIZE,
			1, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->notification_trigger_clients_ht) {
		goto error;
	}

	state->channels_ht = cds_lfht_new(DEFAULT_HT_SIZE,
			1, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->channels_ht) {
		goto error;
	}

	state->triggers_ht = cds_lfht_new(DEFAULT_HT_SIZE,
			1, 0, CDS_LFHT_AUTO_RESIZE | CDS_LFHT_ACCOUNTING, NULL);
	if (!state->triggers_ht) {
		goto error;
	}
end:
	return 0;
error:
	fini_thread_state(state);
	return -1;
}

static
int handle_channel_monitoring_pipe(int fd, uint32_t revents,
		struct notification_thread_handle *handle,
		struct notification_thread_state *state)
{
	int ret = 0;
	enum lttng_domain_type domain;

	if (fd == handle->channel_monitoring_pipes.ust32_consumer ||
			fd == handle->channel_monitoring_pipes.ust64_consumer) {
		domain = LTTNG_DOMAIN_UST;
	} else if (fd == handle->channel_monitoring_pipes.kernel_consumer) {
		domain = LTTNG_DOMAIN_KERNEL;
	} else {
		abort();
	}

	if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
		ret = lttng_poll_del(&state->events, fd);
		if (ret) {
			ERR("[notification-thread] Failed to remove consumer monitoring pipe from poll set");
		}
		goto end;
	}

	ret = handle_notification_thread_channel_sample(
			state, fd, domain);
	if (ret) {
		ERR("[notification-thread] Consumer sample handling error occured");
		ret = -1;
		goto end;
	}
end:
	return ret;
}

/*
 * This thread services notification channel clients and commands received
 * from various lttng-sessiond components over a command queue.
 */
void *thread_notification(void *data)
{
	int ret;
	struct notification_thread_handle *handle = data;
	struct notification_thread_state state;

	DBG("[notification-thread] Started notification thread");

	if (!handle) {
		ERR("[notification-thread] Invalid thread context provided");
		goto end;
	}

	rcu_register_thread();
	rcu_thread_online();

	health_register(health_sessiond, HEALTH_SESSIOND_TYPE_NOTIFICATION);
	health_code_update();

	ret = init_thread_state(handle, &state);
	if (ret) {
		goto end;
	}

	/* Ready to handle client connections. */
	sessiond_notify_ready();

	while (true) {
		int fd_count, i;

		health_poll_entry();
		DBG("[notification-thread] Entering poll wait");
		ret = lttng_poll_wait(&state.events, -1);
		DBG("[notification-thread] Poll wait returned (%i)", ret);
		health_poll_exit();
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				continue;
			}
			ERR("[notification-thread] Error encountered during lttng_poll_wait (%i)", ret);
			goto error;
		}

		fd_count = ret;
		for (i = 0; i < fd_count; i++) {
			int fd = LTTNG_POLL_GETFD(&state.events, i);
			uint32_t revents = LTTNG_POLL_GETEV(&state.events, i);

			if (!revents) {
				continue;
			}
			DBG("[notification-thread] Handling fd (%i) activity (%u)", fd, revents);

			if (fd == state.notification_channel_socket) {
				if (revents & LPOLLIN) {
					ret = handle_notification_thread_client_connect(
							&state);
					if (ret < 0) {
						goto error;
					}
				} else if (revents &
						(LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("[notification-thread] Notification socket poll error");
					goto error;
				} else {
					ERR("[notification-thread] Unexpected poll events %u for notification socket %i", revents, fd);
					goto error;
				}
			} else if (fd == handle->cmd_queue.event_fd) {
				ret = handle_notification_thread_command(handle,
						&state);
				if (ret < 0) {
					DBG("[notification-thread] Error encountered while servicing command queue");
					goto error;
				} else if (ret > 0) {
					goto exit;
				}
			} else if (fd == handle->channel_monitoring_pipes.ust32_consumer ||
					fd == handle->channel_monitoring_pipes.ust64_consumer ||
					fd == handle->channel_monitoring_pipes.kernel_consumer) {
				ret = handle_channel_monitoring_pipe(fd,
						revents, handle, &state);
				if (ret) {
					goto error;
				}
			} else {
				/* Activity on a client's socket. */
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					/*
					 * It doesn't matter if a command was
					 * pending on the client socket at this
					 * point since it now has no way to
					 * receive the notifications to which
					 * it was subscribing or unsubscribing.
					 */
					ret = handle_notification_thread_client_disconnect(
							fd, &state);
					if (ret) {
						goto error;
					}
				} else {
					if (revents & LPOLLIN) {
						ret = handle_notification_thread_client_in(
							&state, fd);
						if (ret) {
							goto error;
						}
					}

					if (revents & LPOLLOUT) {
						ret = handle_notification_thread_client_out(
							&state, fd);
						if (ret) {
							goto error;
						}
					}
				}
			}
		}
	}
exit:
error:
	fini_thread_state(&state);
	health_unregister(health_sessiond);
	rcu_thread_offline();
	rcu_unregister_thread();
end:
	return NULL;
}
