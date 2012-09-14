/*
 * Copyright (C) 2011 - David Goulet <david.goulet@polymtl.ca>
 *                      Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
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

#define _GNU_SOURCE
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <urcu/uatomic.h>
#include <unistd.h>
#include <config.h>

#include <common/common.h>
#include <common/compat/poll.h>
#include <common/compat/socket.h>
#include <common/defaults.h>
#include <common/kernel-consumer/kernel-consumer.h>
#include <common/futex.h>
#include <common/relayd/relayd.h>
#include <common/utils.h>

#include "lttng-sessiond.h"
#include "channel.h"
#include "cmd.h"
#include "consumer.h"
#include "context.h"
#include "event.h"
#include "kernel.h"
#include "kernel-consumer.h"
#include "modprobe.h"
#include "shm.h"
#include "ust-ctl.h"
#include "ust-consumer.h"
#include "utils.h"
#include "fd-limit.h"
#include "filter.h"
#include "health.h"

#define CONSUMERD_FILE	"lttng-consumerd"

/* Const values */
const char default_home_dir[] = DEFAULT_HOME_DIR;
const char default_tracing_group[] = DEFAULT_TRACING_GROUP;
const char default_ust_sock_dir[] = DEFAULT_UST_SOCK_DIR;
const char default_global_apps_pipe[] = DEFAULT_GLOBAL_APPS_PIPE;

const char *progname;
const char *opt_tracing_group;
static int opt_sig_parent;
static int opt_verbose_consumer;
static int opt_daemon;
static int opt_no_kernel;
static int is_root;			/* Set to 1 if the daemon is running as root */
static pid_t ppid;          /* Parent PID for --sig-parent option */
static char *rundir;

/* Consumer daemon specific control data */
static struct consumer_data kconsumer_data = {
	.type = LTTNG_CONSUMER_KERNEL,
	.err_unix_sock_path = DEFAULT_KCONSUMERD_ERR_SOCK_PATH,
	.cmd_unix_sock_path = DEFAULT_KCONSUMERD_CMD_SOCK_PATH,
	.err_sock = -1,
	.cmd_sock = -1,
	.pid_mutex = PTHREAD_MUTEX_INITIALIZER,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};
static struct consumer_data ustconsumer64_data = {
	.type = LTTNG_CONSUMER64_UST,
	.err_unix_sock_path = DEFAULT_USTCONSUMERD64_ERR_SOCK_PATH,
	.cmd_unix_sock_path = DEFAULT_USTCONSUMERD64_CMD_SOCK_PATH,
	.err_sock = -1,
	.cmd_sock = -1,
	.pid_mutex = PTHREAD_MUTEX_INITIALIZER,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};
static struct consumer_data ustconsumer32_data = {
	.type = LTTNG_CONSUMER32_UST,
	.err_unix_sock_path = DEFAULT_USTCONSUMERD32_ERR_SOCK_PATH,
	.cmd_unix_sock_path = DEFAULT_USTCONSUMERD32_CMD_SOCK_PATH,
	.err_sock = -1,
	.cmd_sock = -1,
	.pid_mutex = PTHREAD_MUTEX_INITIALIZER,
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

/* Shared between threads */
static int dispatch_thread_exit;

/* Global application Unix socket path */
static char apps_unix_sock_path[PATH_MAX];
/* Global client Unix socket path */
static char client_unix_sock_path[PATH_MAX];
/* global wait shm path for UST */
static char wait_shm_path[PATH_MAX];
/* Global health check unix path */
static char health_unix_sock_path[PATH_MAX];

/* Sockets and FDs */
static int client_sock = -1;
static int apps_sock = -1;
int kernel_tracer_fd = -1;
static int kernel_poll_pipe[2] = { -1, -1 };

/*
 * Quit pipe for all threads. This permits a single cancellation point
 * for all threads when receiving an event on the pipe.
 */
static int thread_quit_pipe[2] = { -1, -1 };

/*
 * This pipe is used to inform the thread managing application communication
 * that a command is queued and ready to be processed.
 */
static int apps_cmd_pipe[2] = { -1, -1 };

/* Pthread, Mutexes and Semaphores */
static pthread_t apps_thread;
static pthread_t reg_apps_thread;
static pthread_t client_thread;
static pthread_t kernel_thread;
static pthread_t dispatch_thread;
static pthread_t health_thread;

/*
 * UST registration command queue. This queue is tied with a futex and uses a N
 * wakers / 1 waiter implemented and detailed in futex.c/.h
 *
 * The thread_manage_apps and thread_dispatch_ust_registration interact with
 * this queue and the wait/wake scheme.
 */
static struct ust_cmd_queue ust_cmd_queue;

/*
 * Pointer initialized before thread creation.
 *
 * This points to the tracing session list containing the session count and a
 * mutex lock. The lock MUST be taken if you iterate over the list. The lock
 * MUST NOT be taken if you call a public function in session.c.
 *
 * The lock is nested inside the structure: session_list_ptr->lock. Please use
 * session_lock_list and session_unlock_list for lock acquisition.
 */
static struct ltt_session_list *session_list_ptr;

int ust_consumerd64_fd = -1;
int ust_consumerd32_fd = -1;

static const char *consumerd32_bin = CONFIG_CONSUMERD32_BIN;
static const char *consumerd64_bin = CONFIG_CONSUMERD64_BIN;
static const char *consumerd32_libdir = CONFIG_CONSUMERD32_LIBDIR;
static const char *consumerd64_libdir = CONFIG_CONSUMERD64_LIBDIR;

static const char *module_proc_lttng = "/proc/lttng";

/*
 * Consumer daemon state which is changed when spawning it, killing it or in
 * case of a fatal error.
 */
enum consumerd_state {
	CONSUMER_STARTED = 1,
	CONSUMER_STOPPED = 2,
	CONSUMER_ERROR   = 3,
};

/*
 * This consumer daemon state is used to validate if a client command will be
 * able to reach the consumer. If not, the client is informed. For instance,
 * doing a "lttng start" when the consumer state is set to ERROR will return an
 * error to the client.
 *
 * The following example shows a possible race condition of this scheme:
 *
 * consumer thread error happens
 *                                    client cmd arrives
 *                                    client cmd checks state -> still OK
 * consumer thread exit, sets error
 *                                    client cmd try to talk to consumer
 *                                    ...
 *
 * However, since the consumer is a different daemon, we have no way of making
 * sure the command will reach it safely even with this state flag. This is why
 * we consider that up to the state validation during command processing, the
 * command is safe. After that, we can not guarantee the correctness of the
 * client request vis-a-vis the consumer.
 */
static enum consumerd_state ust_consumerd_state;
static enum consumerd_state kernel_consumerd_state;

/* Used for the health monitoring of the session daemon. See health.h */
struct health_state health_thread_cmd;
struct health_state health_thread_app_manage;
struct health_state health_thread_app_reg;
struct health_state health_thread_kernel;

static
void setup_consumerd_path(void)
{
	const char *bin, *libdir;

	/*
	 * Allow INSTALL_BIN_PATH to be used as a target path for the
	 * native architecture size consumer if CONFIG_CONSUMER*_PATH
	 * has not been defined.
	 */
#if (CAA_BITS_PER_LONG == 32)
	if (!consumerd32_bin[0]) {
		consumerd32_bin = INSTALL_BIN_PATH "/" CONSUMERD_FILE;
	}
	if (!consumerd32_libdir[0]) {
		consumerd32_libdir = INSTALL_LIB_PATH;
	}
#elif (CAA_BITS_PER_LONG == 64)
	if (!consumerd64_bin[0]) {
		consumerd64_bin = INSTALL_BIN_PATH "/" CONSUMERD_FILE;
	}
	if (!consumerd64_libdir[0]) {
		consumerd64_libdir = INSTALL_LIB_PATH;
	}
#else
#error "Unknown bitness"
#endif

	/*
	 * runtime env. var. overrides the build default.
	 */
	bin = getenv("LTTNG_CONSUMERD32_BIN");
	if (bin) {
		consumerd32_bin = bin;
	}
	bin = getenv("LTTNG_CONSUMERD64_BIN");
	if (bin) {
		consumerd64_bin = bin;
	}
	libdir = getenv("LTTNG_CONSUMERD32_LIBDIR");
	if (libdir) {
		consumerd32_libdir = libdir;
	}
	libdir = getenv("LTTNG_CONSUMERD64_LIBDIR");
	if (libdir) {
		consumerd64_libdir = libdir;
	}
}

/*
 * Create a poll set with O_CLOEXEC and add the thread quit pipe to the set.
 */
static int create_thread_poll_set(struct lttng_poll_event *events,
		unsigned int size)
{
	int ret;

	if (events == NULL || size == 0) {
		ret = -1;
		goto error;
	}

	ret = lttng_poll_create(events, size, LTTNG_CLOEXEC);
	if (ret < 0) {
		goto error;
	}

	/* Add quit pipe */
	ret = lttng_poll_add(events, thread_quit_pipe[0], LPOLLIN);
	if (ret < 0) {
		goto error;
	}

	return 0;

error:
	return ret;
}

/*
 * Check if the thread quit pipe was triggered.
 *
 * Return 1 if it was triggered else 0;
 */
static int check_thread_quit_pipe(int fd, uint32_t events)
{
	if (fd == thread_quit_pipe[0] && (events & LPOLLIN)) {
		return 1;
	}

	return 0;
}

/*
 * Return group ID of the tracing group or -1 if not found.
 */
static gid_t allowed_group(void)
{
	struct group *grp;

	if (opt_tracing_group) {
		grp = getgrnam(opt_tracing_group);
	} else {
		grp = getgrnam(default_tracing_group);
	}
	if (!grp) {
		return -1;
	} else {
		return grp->gr_gid;
	}
}

/*
 * Init thread quit pipe.
 *
 * Return -1 on error or 0 if all pipes are created.
 */
static int init_thread_quit_pipe(void)
{
	int ret, i;

	ret = pipe(thread_quit_pipe);
	if (ret < 0) {
		PERROR("thread quit pipe");
		goto error;
	}

	for (i = 0; i < 2; i++) {
		ret = fcntl(thread_quit_pipe[i], F_SETFD, FD_CLOEXEC);
		if (ret < 0) {
			PERROR("fcntl");
			goto error;
		}
	}

error:
	return ret;
}

/*
 * Stop all threads by closing the thread quit pipe.
 */
static void stop_threads(void)
{
	int ret;

	/* Stopping all threads */
	DBG("Terminating all threads");
	ret = notify_thread_pipe(thread_quit_pipe[1]);
	if (ret < 0) {
		ERR("write error on thread quit pipe");
	}

	/* Dispatch thread */
	CMM_STORE_SHARED(dispatch_thread_exit, 1);
	futex_nto1_wake(&ust_cmd_queue.futex);
}

/*
 * Cleanup the daemon
 */
static void cleanup(void)
{
	int ret;
	char *cmd;
	struct ltt_session *sess, *stmp;

	DBG("Cleaning up");

	/* First thing first, stop all threads */
	utils_close_pipe(thread_quit_pipe);

	DBG("Removing %s directory", rundir);
	ret = asprintf(&cmd, "rm -rf %s", rundir);
	if (ret < 0) {
		ERR("asprintf failed. Something is really wrong!");
	}

	/* Remove lttng run directory */
	ret = system(cmd);
	if (ret < 0) {
		ERR("Unable to clean %s", rundir);
	}
	free(cmd);

	DBG("Cleaning up all sessions");

	/* Destroy session list mutex */
	if (session_list_ptr != NULL) {
		pthread_mutex_destroy(&session_list_ptr->lock);

		/* Cleanup ALL session */
		cds_list_for_each_entry_safe(sess, stmp,
				&session_list_ptr->head, list) {
			cmd_destroy_session(sess, kernel_poll_pipe[1]);
		}
	}

	DBG("Closing all UST sockets");
	ust_app_clean_list();

	if (is_root && !opt_no_kernel) {
		DBG2("Closing kernel fd");
		if (kernel_tracer_fd >= 0) {
			ret = close(kernel_tracer_fd);
			if (ret) {
				PERROR("close");
			}
		}
		DBG("Unloading kernel modules");
		modprobe_remove_lttng_all();
	}

	utils_close_pipe(kernel_poll_pipe);
	utils_close_pipe(apps_cmd_pipe);

	/* <fun> */
	DBG("%c[%d;%dm*** assert failed :-) *** ==> %c[%dm%c[%d;%dm"
			"Matthew, BEET driven development works!%c[%dm",
			27, 1, 31, 27, 0, 27, 1, 33, 27, 0);
	/* </fun> */
}

/*
 * Send data on a unix socket using the liblttsessiondcomm API.
 *
 * Return lttcomm error code.
 */
static int send_unix_sock(int sock, void *buf, size_t len)
{
	/* Check valid length */
	if (len <= 0) {
		return -1;
	}

	return lttcomm_send_unix_sock(sock, buf, len);
}

/*
 * Free memory of a command context structure.
 */
static void clean_command_ctx(struct command_ctx **cmd_ctx)
{
	DBG("Clean command context structure");
	if (*cmd_ctx) {
		if ((*cmd_ctx)->llm) {
			free((*cmd_ctx)->llm);
		}
		if ((*cmd_ctx)->lsm) {
			free((*cmd_ctx)->lsm);
		}
		free(*cmd_ctx);
		*cmd_ctx = NULL;
	}
}

/*
 * Notify UST applications using the shm mmap futex.
 */
static int notify_ust_apps(int active)
{
	char *wait_shm_mmap;

	DBG("Notifying applications of session daemon state: %d", active);

	/* See shm.c for this call implying mmap, shm and futex calls */
	wait_shm_mmap = shm_ust_get_mmap(wait_shm_path, is_root);
	if (wait_shm_mmap == NULL) {
		goto error;
	}

	/* Wake waiting process */
	futex_wait_update((int32_t *) wait_shm_mmap, active);

	/* Apps notified successfully */
	return 0;

error:
	return -1;
}

/*
 * Setup the outgoing data buffer for the response (llm) by allocating the
 * right amount of memory and copying the original information from the lsm
 * structure.
 *
 * Return total size of the buffer pointed by buf.
 */
static int setup_lttng_msg(struct command_ctx *cmd_ctx, size_t size)
{
	int ret, buf_size;

	buf_size = size;

	cmd_ctx->llm = zmalloc(sizeof(struct lttcomm_lttng_msg) + buf_size);
	if (cmd_ctx->llm == NULL) {
		PERROR("zmalloc");
		ret = -ENOMEM;
		goto error;
	}

	/* Copy common data */
	cmd_ctx->llm->cmd_type = cmd_ctx->lsm->cmd_type;
	cmd_ctx->llm->pid = cmd_ctx->lsm->domain.attr.pid;

	cmd_ctx->llm->data_size = size;
	cmd_ctx->lttng_msg_size = sizeof(struct lttcomm_lttng_msg) + buf_size;

	return buf_size;

error:
	return ret;
}

/*
 * Update the kernel poll set of all channel fd available over all tracing
 * session. Add the wakeup pipe at the end of the set.
 */
static int update_kernel_poll(struct lttng_poll_event *events)
{
	int ret;
	struct ltt_session *session;
	struct ltt_kernel_channel *channel;

	DBG("Updating kernel poll set");

	session_lock_list();
	cds_list_for_each_entry(session, &session_list_ptr->head, list) {
		session_lock(session);
		if (session->kernel_session == NULL) {
			session_unlock(session);
			continue;
		}

		cds_list_for_each_entry(channel,
				&session->kernel_session->channel_list.head, list) {
			/* Add channel fd to the kernel poll set */
			ret = lttng_poll_add(events, channel->fd, LPOLLIN | LPOLLRDNORM);
			if (ret < 0) {
				session_unlock(session);
				goto error;
			}
			DBG("Channel fd %d added to kernel set", channel->fd);
		}
		session_unlock(session);
	}
	session_unlock_list();

	return 0;

error:
	session_unlock_list();
	return -1;
}

/*
 * Find the channel fd from 'fd' over all tracing session. When found, check
 * for new channel stream and send those stream fds to the kernel consumer.
 *
 * Useful for CPU hotplug feature.
 */
static int update_kernel_stream(struct consumer_data *consumer_data, int fd)
{
	int ret = 0;
	struct ltt_session *session;
	struct ltt_kernel_session *ksess;
	struct ltt_kernel_channel *channel;

	DBG("Updating kernel streams for channel fd %d", fd);

	session_lock_list();
	cds_list_for_each_entry(session, &session_list_ptr->head, list) {
		session_lock(session);
		if (session->kernel_session == NULL) {
			session_unlock(session);
			continue;
		}
		ksess = session->kernel_session;

		cds_list_for_each_entry(channel, &ksess->channel_list.head, list) {
			if (channel->fd == fd) {
				DBG("Channel found, updating kernel streams");
				ret = kernel_open_channel_stream(channel);
				if (ret < 0) {
					goto error;
				}

				/*
				 * Have we already sent fds to the consumer? If yes, it means
				 * that tracing is started so it is safe to send our updated
				 * stream fds.
				 */
				if (ksess->consumer_fds_sent == 1 && ksess->consumer != NULL) {
					struct lttng_ht_iter iter;
					struct consumer_socket *socket;


					cds_lfht_for_each_entry(ksess->consumer->socks->ht,
							&iter.iter, socket, node.node) {
						/* Code flow error */
						assert(socket->fd >= 0);

						pthread_mutex_lock(socket->lock);
						ret = kernel_consumer_send_channel_stream(socket->fd,
								channel, ksess);
						pthread_mutex_unlock(socket->lock);
						if (ret < 0) {
							goto error;
						}
					}
				}
				goto error;
			}
		}
		session_unlock(session);
	}
	session_unlock_list();
	return ret;

error:
	session_unlock(session);
	session_unlock_list();
	return ret;
}

/*
 * For each tracing session, update newly registered apps.
 */
static void update_ust_app(int app_sock)
{
	struct ltt_session *sess, *stmp;

	session_lock_list();

	/* For all tracing session(s) */
	cds_list_for_each_entry_safe(sess, stmp, &session_list_ptr->head, list) {
		session_lock(sess);
		if (sess->ust_session) {
			ust_app_global_update(sess->ust_session, app_sock);
		}
		session_unlock(sess);
	}

	session_unlock_list();
}

/*
 * This thread manage event coming from the kernel.
 *
 * Features supported in this thread:
 *    -) CPU Hotplug
 */
static void *thread_manage_kernel(void *data)
{
	int ret, i, pollfd, update_poll_flag = 1, err = -1;
	uint32_t revents, nb_fd;
	char tmp;
	struct lttng_poll_event events;

	DBG("Thread manage kernel started");

	health_code_update(&health_thread_kernel);

	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_poll_create;
	}

	ret = lttng_poll_add(&events, kernel_poll_pipe[0], LPOLLIN);
	if (ret < 0) {
		goto error;
	}

	while (1) {
		health_code_update(&health_thread_kernel);

		if (update_poll_flag == 1) {
			/*
			 * Reset number of fd in the poll set. Always 2 since there is the thread
			 * quit pipe and the kernel pipe.
			 */
			events.nb_fd = 2;

			ret = update_kernel_poll(&events);
			if (ret < 0) {
				goto error;
			}
			update_poll_flag = 0;
		}

		nb_fd = LTTNG_POLL_GETNB(&events);

		DBG("Thread kernel polling on %d fds", nb_fd);

		/* Zeroed the poll events */
		lttng_poll_reset(&events);

		/* Poll infinite value of time */
	restart:
		health_poll_update(&health_thread_kernel);
		ret = lttng_poll_wait(&events, -1);
		health_poll_update(&health_thread_kernel);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		} else if (ret == 0) {
			/* Should not happen since timeout is infinite */
			ERR("Return value of poll is 0 with an infinite timeout.\n"
				"This should not have happened! Continuing...");
			continue;
		}

		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			health_code_update(&health_thread_kernel);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_thread_quit_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Check for data on kernel pipe */
			if (pollfd == kernel_poll_pipe[0] && (revents & LPOLLIN)) {
				ret = read(kernel_poll_pipe[0], &tmp, 1);
				update_poll_flag = 1;
				continue;
			} else {
				/*
				 * New CPU detected by the kernel. Adding kernel stream to
				 * kernel session and updating the kernel consumer
				 */
				if (revents & LPOLLIN) {
					ret = update_kernel_stream(&kconsumer_data, pollfd);
					if (ret < 0) {
						continue;
					}
					break;
					/*
					 * TODO: We might want to handle the LPOLLERR | LPOLLHUP
					 * and unregister kernel stream at this point.
					 */
				}
			}
		}
	}

exit:
error:
	lttng_poll_clean(&events);
error_poll_create:
	if (err) {
		health_error(&health_thread_kernel);
		ERR("Health error occurred in %s", __func__);
	}
	health_exit(&health_thread_kernel);
	DBG("Kernel thread dying");
	return NULL;
}

/*
 * This thread manage the consumer error sent back to the session daemon.
 */
static void *thread_manage_consumer(void *data)
{
	int sock = -1, i, ret, pollfd, err = -1;
	uint32_t revents, nb_fd;
	enum lttcomm_return_code code;
	struct lttng_poll_event events;
	struct consumer_data *consumer_data = data;

	DBG("[thread] Manage consumer started");

	health_code_update(&consumer_data->health);

	ret = lttcomm_listen_unix_sock(consumer_data->err_sock);
	if (ret < 0) {
		goto error_listen;
	}

	/*
	 * Pass 2 as size here for the thread quit pipe and kconsumerd_err_sock.
	 * Nothing more will be added to this poll set.
	 */
	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_poll;
	}

	ret = lttng_poll_add(&events, consumer_data->err_sock, LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error;
	}

	nb_fd = LTTNG_POLL_GETNB(&events);

	health_code_update(&consumer_data->health);

	/* Inifinite blocking call, waiting for transmission */
restart:
	health_poll_update(&consumer_data->health);
	ret = lttng_poll_wait(&events, -1);
	health_poll_update(&consumer_data->health);
	if (ret < 0) {
		/*
		 * Restart interrupted system call.
		 */
		if (errno == EINTR) {
			goto restart;
		}
		goto error;
	}

	for (i = 0; i < nb_fd; i++) {
		/* Fetch once the poll data */
		revents = LTTNG_POLL_GETEV(&events, i);
		pollfd = LTTNG_POLL_GETFD(&events, i);

		health_code_update(&consumer_data->health);

		/* Thread quit pipe has been closed. Killing thread. */
		ret = check_thread_quit_pipe(pollfd, revents);
		if (ret) {
			err = 0;
			goto exit;
		}

		/* Event on the registration socket */
		if (pollfd == consumer_data->err_sock) {
			if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
				ERR("consumer err socket poll error");
				goto error;
			}
		}
	}

	sock = lttcomm_accept_unix_sock(consumer_data->err_sock);
	if (sock < 0) {
		goto error;
	}

	health_code_update(&consumer_data->health);

	DBG2("Receiving code from consumer err_sock");

	/* Getting status code from kconsumerd */
	ret = lttcomm_recv_unix_sock(sock, &code,
			sizeof(enum lttcomm_return_code));
	if (ret <= 0) {
		goto error;
	}

	health_code_update(&consumer_data->health);

	if (code == LTTCOMM_CONSUMERD_COMMAND_SOCK_READY) {
		consumer_data->cmd_sock =
			lttcomm_connect_unix_sock(consumer_data->cmd_unix_sock_path);
		if (consumer_data->cmd_sock < 0) {
			sem_post(&consumer_data->sem);
			PERROR("consumer connect");
			goto error;
		}
		/* Signal condition to tell that the kconsumerd is ready */
		sem_post(&consumer_data->sem);
		DBG("consumer command socket ready");
	} else {
		ERR("consumer error when waiting for SOCK_READY : %s",
				lttcomm_get_readable_code(-code));
		goto error;
	}

	/* Remove the kconsumerd error sock since we've established a connexion */
	ret = lttng_poll_del(&events, consumer_data->err_sock);
	if (ret < 0) {
		goto error;
	}

	ret = lttng_poll_add(&events, sock, LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error;
	}

	health_code_update(&consumer_data->health);

	/* Update number of fd */
	nb_fd = LTTNG_POLL_GETNB(&events);

	/* Inifinite blocking call, waiting for transmission */
restart_poll:
	health_poll_update(&consumer_data->health);
	ret = lttng_poll_wait(&events, -1);
	health_poll_update(&consumer_data->health);
	if (ret < 0) {
		/*
		 * Restart interrupted system call.
		 */
		if (errno == EINTR) {
			goto restart_poll;
		}
		goto error;
	}

	for (i = 0; i < nb_fd; i++) {
		/* Fetch once the poll data */
		revents = LTTNG_POLL_GETEV(&events, i);
		pollfd = LTTNG_POLL_GETFD(&events, i);

		health_code_update(&consumer_data->health);

		/* Thread quit pipe has been closed. Killing thread. */
		ret = check_thread_quit_pipe(pollfd, revents);
		if (ret) {
			err = 0;
			goto exit;
		}

		/* Event on the kconsumerd socket */
		if (pollfd == sock) {
			if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
				ERR("consumer err socket second poll error");
				goto error;
			}
		}
	}

	health_code_update(&consumer_data->health);

	/* Wait for any kconsumerd error */
	ret = lttcomm_recv_unix_sock(sock, &code,
			sizeof(enum lttcomm_return_code));
	if (ret <= 0) {
		ERR("consumer closed the command socket");
		goto error;
	}

	ERR("consumer return code : %s", lttcomm_get_readable_code(-code));

exit:
error:
	/* Immediately set the consumerd state to stopped */
	if (consumer_data->type == LTTNG_CONSUMER_KERNEL) {
		uatomic_set(&kernel_consumerd_state, CONSUMER_ERROR);
	} else if (consumer_data->type == LTTNG_CONSUMER64_UST ||
			consumer_data->type == LTTNG_CONSUMER32_UST) {
		uatomic_set(&ust_consumerd_state, CONSUMER_ERROR);
	} else {
		/* Code flow error... */
		assert(0);
	}

	if (consumer_data->err_sock >= 0) {
		ret = close(consumer_data->err_sock);
		if (ret) {
			PERROR("close");
		}
	}
	if (consumer_data->cmd_sock >= 0) {
		ret = close(consumer_data->cmd_sock);
		if (ret) {
			PERROR("close");
		}
	}
	if (sock >= 0) {
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
	}

	unlink(consumer_data->err_unix_sock_path);
	unlink(consumer_data->cmd_unix_sock_path);
	consumer_data->pid = 0;

	lttng_poll_clean(&events);
error_poll:
error_listen:
	if (err) {
		health_error(&consumer_data->health);
		ERR("Health error occurred in %s", __func__);
	}
	health_exit(&consumer_data->health);
	DBG("consumer thread cleanup completed");

	return NULL;
}

/*
 * This thread manage application communication.
 */
static void *thread_manage_apps(void *data)
{
	int i, ret, pollfd, err = -1;
	uint32_t revents, nb_fd;
	struct ust_command ust_cmd;
	struct lttng_poll_event events;

	DBG("[thread] Manage application started");

	rcu_register_thread();
	rcu_thread_online();

	health_code_update(&health_thread_app_manage);

	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_poll_create;
	}

	ret = lttng_poll_add(&events, apps_cmd_pipe[0], LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error;
	}

	health_code_update(&health_thread_app_manage);

	while (1) {
		/* Zeroed the events structure */
		lttng_poll_reset(&events);

		nb_fd = LTTNG_POLL_GETNB(&events);

		DBG("Apps thread polling on %d fds", nb_fd);

		/* Inifinite blocking call, waiting for transmission */
	restart:
		health_poll_update(&health_thread_app_manage);
		ret = lttng_poll_wait(&events, -1);
		health_poll_update(&health_thread_app_manage);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			health_code_update(&health_thread_app_manage);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_thread_quit_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Inspect the apps cmd pipe */
			if (pollfd == apps_cmd_pipe[0]) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("Apps command pipe error");
					goto error;
				} else if (revents & LPOLLIN) {
					/* Empty pipe */
					ret = read(apps_cmd_pipe[0], &ust_cmd, sizeof(ust_cmd));
					if (ret < 0 || ret < sizeof(ust_cmd)) {
						PERROR("read apps cmd pipe");
						goto error;
					}

					health_code_update(&health_thread_app_manage);

					/* Register applicaton to the session daemon */
					ret = ust_app_register(&ust_cmd.reg_msg,
							ust_cmd.sock);
					if (ret == -ENOMEM) {
						goto error;
					} else if (ret < 0) {
						break;
					}

					health_code_update(&health_thread_app_manage);

					/*
					 * Validate UST version compatibility.
					 */
					ret = ust_app_validate_version(ust_cmd.sock);
					if (ret >= 0) {
						/*
						 * Add channel(s) and event(s) to newly registered apps
						 * from lttng global UST domain.
						 */
						update_ust_app(ust_cmd.sock);
					}

					health_code_update(&health_thread_app_manage);

					ret = ust_app_register_done(ust_cmd.sock);
					if (ret < 0) {
						/*
						 * If the registration is not possible, we simply
						 * unregister the apps and continue
						 */
						ust_app_unregister(ust_cmd.sock);
					} else {
						/*
						 * We just need here to monitor the close of the UST
						 * socket and poll set monitor those by default.
						 * Listen on POLLIN (even if we never expect any
						 * data) to ensure that hangup wakes us.
						 */
						ret = lttng_poll_add(&events, ust_cmd.sock, LPOLLIN);
						if (ret < 0) {
							goto error;
						}

						DBG("Apps with sock %d added to poll set",
								ust_cmd.sock);
					}

					health_code_update(&health_thread_app_manage);

					break;
				}
			} else {
				/*
				 * At this point, we know that a registered application made
				 * the event at poll_wait.
				 */
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					/* Removing from the poll set */
					ret = lttng_poll_del(&events, pollfd);
					if (ret < 0) {
						goto error;
					}

					/* Socket closed on remote end. */
					ust_app_unregister(pollfd);
					break;
				}
			}

			health_code_update(&health_thread_app_manage);
		}
	}

exit:
error:
	lttng_poll_clean(&events);
error_poll_create:
	if (err) {
		health_error(&health_thread_app_manage);
		ERR("Health error occurred in %s", __func__);
	}
	health_exit(&health_thread_app_manage);
	DBG("Application communication apps thread cleanup complete");
	rcu_thread_offline();
	rcu_unregister_thread();
	return NULL;
}

/*
 * Dispatch request from the registration threads to the application
 * communication thread.
 */
static void *thread_dispatch_ust_registration(void *data)
{
	int ret;
	struct cds_wfq_node *node;
	struct ust_command *ust_cmd = NULL;

	DBG("[thread] Dispatch UST command started");

	while (!CMM_LOAD_SHARED(dispatch_thread_exit)) {
		/* Atomically prepare the queue futex */
		futex_nto1_prepare(&ust_cmd_queue.futex);

		do {
			/* Dequeue command for registration */
			node = cds_wfq_dequeue_blocking(&ust_cmd_queue.queue);
			if (node == NULL) {
				DBG("Woken up but nothing in the UST command queue");
				/* Continue thread execution */
				break;
			}

			ust_cmd = caa_container_of(node, struct ust_command, node);

			DBG("Dispatching UST registration pid:%d ppid:%d uid:%d"
					" gid:%d sock:%d name:%s (version %d.%d)",
					ust_cmd->reg_msg.pid, ust_cmd->reg_msg.ppid,
					ust_cmd->reg_msg.uid, ust_cmd->reg_msg.gid,
					ust_cmd->sock, ust_cmd->reg_msg.name,
					ust_cmd->reg_msg.major, ust_cmd->reg_msg.minor);
			/*
			 * Inform apps thread of the new application registration. This
			 * call is blocking so we can be assured that the data will be read
			 * at some point in time or wait to the end of the world :)
			 */
			ret = write(apps_cmd_pipe[1], ust_cmd,
					sizeof(struct ust_command));
			if (ret < 0) {
				PERROR("write apps cmd pipe");
				if (errno == EBADF) {
					/*
					 * We can't inform the application thread to process
					 * registration. We will exit or else application
					 * registration will not occur and tracing will never
					 * start.
					 */
					goto error;
				}
			}
			free(ust_cmd);
		} while (node != NULL);

		/* Futex wait on queue. Blocking call on futex() */
		futex_nto1_wait(&ust_cmd_queue.futex);
	}

error:
	DBG("Dispatch thread dying");
	return NULL;
}

/*
 * This thread manage application registration.
 */
static void *thread_registration_apps(void *data)
{
	int sock = -1, i, ret, pollfd, err = -1;
	uint32_t revents, nb_fd;
	struct lttng_poll_event events;
	/*
	 * Get allocated in this thread, enqueued to a global queue, dequeued and
	 * freed in the manage apps thread.
	 */
	struct ust_command *ust_cmd = NULL;

	DBG("[thread] Manage application registration started");

	ret = lttcomm_listen_unix_sock(apps_sock);
	if (ret < 0) {
		goto error_listen;
	}

	/*
	 * Pass 2 as size here for the thread quit pipe and apps socket. Nothing
	 * more will be added to this poll set.
	 */
	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error_create_poll;
	}

	/* Add the application registration socket */
	ret = lttng_poll_add(&events, apps_sock, LPOLLIN | LPOLLRDHUP);
	if (ret < 0) {
		goto error_poll_add;
	}

	/* Notify all applications to register */
	ret = notify_ust_apps(1);
	if (ret < 0) {
		ERR("Failed to notify applications or create the wait shared memory.\n"
			"Execution continues but there might be problem for already\n"
			"running applications that wishes to register.");
	}

	while (1) {
		DBG("Accepting application registration");

		nb_fd = LTTNG_POLL_GETNB(&events);

		/* Inifinite blocking call, waiting for transmission */
	restart:
		health_poll_update(&health_thread_app_reg);
		ret = lttng_poll_wait(&events, -1);
		health_poll_update(&health_thread_app_reg);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		for (i = 0; i < nb_fd; i++) {
			health_code_update(&health_thread_app_reg);

			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_thread_quit_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Event on the registration socket */
			if (pollfd == apps_sock) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("Register apps socket poll error");
					goto error;
				} else if (revents & LPOLLIN) {
					sock = lttcomm_accept_unix_sock(apps_sock);
					if (sock < 0) {
						goto error;
					}

					/* Create UST registration command for enqueuing */
					ust_cmd = zmalloc(sizeof(struct ust_command));
					if (ust_cmd == NULL) {
						PERROR("ust command zmalloc");
						goto error;
					}

					/*
					 * Using message-based transmissions to ensure we don't
					 * have to deal with partially received messages.
					 */
					ret = lttng_fd_get(LTTNG_FD_APPS, 1);
					if (ret < 0) {
						ERR("Exhausted file descriptors allowed for applications.");
						free(ust_cmd);
						ret = close(sock);
						if (ret) {
							PERROR("close");
						}
						sock = -1;
						continue;
					}
					health_code_update(&health_thread_app_reg);
					ret = lttcomm_recv_unix_sock(sock, &ust_cmd->reg_msg,
							sizeof(struct ust_register_msg));
					if (ret < 0 || ret < sizeof(struct ust_register_msg)) {
						if (ret < 0) {
							PERROR("lttcomm_recv_unix_sock register apps");
						} else {
							ERR("Wrong size received on apps register");
						}
						free(ust_cmd);
						ret = close(sock);
						if (ret) {
							PERROR("close");
						}
						lttng_fd_put(LTTNG_FD_APPS, 1);
						sock = -1;
						continue;
					}
					health_code_update(&health_thread_app_reg);

					ust_cmd->sock = sock;
					sock = -1;

					DBG("UST registration received with pid:%d ppid:%d uid:%d"
							" gid:%d sock:%d name:%s (version %d.%d)",
							ust_cmd->reg_msg.pid, ust_cmd->reg_msg.ppid,
							ust_cmd->reg_msg.uid, ust_cmd->reg_msg.gid,
							ust_cmd->sock, ust_cmd->reg_msg.name,
							ust_cmd->reg_msg.major, ust_cmd->reg_msg.minor);

					/*
					 * Lock free enqueue the registration request. The red pill
					 * has been taken! This apps will be part of the *system*.
					 */
					cds_wfq_enqueue(&ust_cmd_queue.queue, &ust_cmd->node);

					/*
					 * Wake the registration queue futex. Implicit memory
					 * barrier with the exchange in cds_wfq_enqueue.
					 */
					futex_nto1_wake(&ust_cmd_queue.futex);
				}
			}
		}
	}

exit:
error:
	if (err) {
		health_error(&health_thread_app_reg);
		ERR("Health error occurred in %s", __func__);
	}
	health_exit(&health_thread_app_reg);

	/* Notify that the registration thread is gone */
	notify_ust_apps(0);

	if (apps_sock >= 0) {
		ret = close(apps_sock);
		if (ret) {
			PERROR("close");
		}
	}
	if (sock >= 0) {
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
		lttng_fd_put(LTTNG_FD_APPS, 1);
	}
	unlink(apps_unix_sock_path);

error_poll_add:
	lttng_poll_clean(&events);
error_listen:
error_create_poll:
	DBG("UST Registration thread cleanup complete");

	return NULL;
}

/*
 * Start the thread_manage_consumer. This must be done after a lttng-consumerd
 * exec or it will fails.
 */
static int spawn_consumer_thread(struct consumer_data *consumer_data)
{
	int ret;
	struct timespec timeout;

	timeout.tv_sec = DEFAULT_SEM_WAIT_TIMEOUT;
	timeout.tv_nsec = 0;

	/* Setup semaphore */
	ret = sem_init(&consumer_data->sem, 0, 0);
	if (ret < 0) {
		PERROR("sem_init consumer semaphore");
		goto error;
	}

	ret = pthread_create(&consumer_data->thread, NULL,
			thread_manage_consumer, consumer_data);
	if (ret != 0) {
		PERROR("pthread_create consumer");
		ret = -1;
		goto error;
	}

	/* Get time for sem_timedwait absolute timeout */
	ret = clock_gettime(CLOCK_REALTIME, &timeout);
	if (ret < 0) {
		PERROR("clock_gettime spawn consumer");
		/* Infinite wait for the kconsumerd thread to be ready */
		ret = sem_wait(&consumer_data->sem);
	} else {
		/* Normal timeout if the gettime was successful */
		timeout.tv_sec += DEFAULT_SEM_WAIT_TIMEOUT;
		ret = sem_timedwait(&consumer_data->sem, &timeout);
	}

	if (ret < 0) {
		if (errno == ETIMEDOUT) {
			/*
			 * Call has timed out so we kill the kconsumerd_thread and return
			 * an error.
			 */
			ERR("The consumer thread was never ready. Killing it");
			ret = pthread_cancel(consumer_data->thread);
			if (ret < 0) {
				PERROR("pthread_cancel consumer thread");
			}
		} else {
			PERROR("semaphore wait failed consumer thread");
		}
		goto error;
	}

	pthread_mutex_lock(&consumer_data->pid_mutex);
	if (consumer_data->pid == 0) {
		ERR("Kconsumerd did not start");
		pthread_mutex_unlock(&consumer_data->pid_mutex);
		goto error;
	}
	pthread_mutex_unlock(&consumer_data->pid_mutex);

	return 0;

error:
	return ret;
}

/*
 * Join consumer thread
 */
static int join_consumer_thread(struct consumer_data *consumer_data)
{
	void *status;
	int ret;

	/* Consumer pid must be a real one. */
	if (consumer_data->pid > 0) {
		ret = kill(consumer_data->pid, SIGTERM);
		if (ret) {
			ERR("Error killing consumer daemon");
			return ret;
		}
		return pthread_join(consumer_data->thread, &status);
	} else {
		return 0;
	}
}

/*
 * Fork and exec a consumer daemon (consumerd).
 *
 * Return pid if successful else -1.
 */
static pid_t spawn_consumerd(struct consumer_data *consumer_data)
{
	int ret;
	pid_t pid;
	const char *consumer_to_use;
	const char *verbosity;
	struct stat st;

	DBG("Spawning consumerd");

	pid = fork();
	if (pid == 0) {
		/*
		 * Exec consumerd.
		 */
		if (opt_verbose_consumer) {
			verbosity = "--verbose";
		} else {
			verbosity = "--quiet";
		}
		switch (consumer_data->type) {
		case LTTNG_CONSUMER_KERNEL:
			/*
			 * Find out which consumerd to execute. We will first try the
			 * 64-bit path, then the sessiond's installation directory, and
			 * fallback on the 32-bit one,
			 */
			DBG3("Looking for a kernel consumer at these locations:");
			DBG3("	1) %s", consumerd64_bin);
			DBG3("	2) %s/%s", INSTALL_BIN_PATH, CONSUMERD_FILE);
			DBG3("	3) %s", consumerd32_bin);
			if (stat(consumerd64_bin, &st) == 0) {
				DBG3("Found location #1");
				consumer_to_use = consumerd64_bin;
			} else if (stat(INSTALL_BIN_PATH "/" CONSUMERD_FILE, &st) == 0) {
				DBG3("Found location #2");
				consumer_to_use = INSTALL_BIN_PATH "/" CONSUMERD_FILE;
			} else if (stat(consumerd32_bin, &st) == 0) {
				DBG3("Found location #3");
				consumer_to_use = consumerd32_bin;
			} else {
				DBG("Could not find any valid consumerd executable");
				break;
			}
			DBG("Using kernel consumer at: %s",  consumer_to_use);
			execl(consumer_to_use,
				"lttng-consumerd", verbosity, "-k",
				"--consumerd-cmd-sock", consumer_data->cmd_unix_sock_path,
				"--consumerd-err-sock", consumer_data->err_unix_sock_path,
				NULL);
			break;
		case LTTNG_CONSUMER64_UST:
		{
			char *tmpnew = NULL;

			if (consumerd64_libdir[0] != '\0') {
				char *tmp;
				size_t tmplen;

				tmp = getenv("LD_LIBRARY_PATH");
				if (!tmp) {
					tmp = "";
				}
				tmplen = strlen("LD_LIBRARY_PATH=")
					+ strlen(consumerd64_libdir) + 1 /* : */ + strlen(tmp);
				tmpnew = zmalloc(tmplen + 1 /* \0 */);
				if (!tmpnew) {
					ret = -ENOMEM;
					goto error;
				}
				strcpy(tmpnew, "LD_LIBRARY_PATH=");
				strcat(tmpnew, consumerd64_libdir);
				if (tmp[0] != '\0') {
					strcat(tmpnew, ":");
					strcat(tmpnew, tmp);
				}
				ret = putenv(tmpnew);
				if (ret) {
					ret = -errno;
					goto error;
				}
			}
			DBG("Using 64-bit UST consumer at: %s",  consumerd64_bin);
			ret = execl(consumerd64_bin, "lttng-consumerd", verbosity, "-u",
					"--consumerd-cmd-sock", consumer_data->cmd_unix_sock_path,
					"--consumerd-err-sock", consumer_data->err_unix_sock_path,
					NULL);
			if (consumerd64_libdir[0] != '\0') {
				free(tmpnew);
			}
			if (ret) {
				goto error;
			}
			break;
		}
		case LTTNG_CONSUMER32_UST:
		{
			char *tmpnew = NULL;

			if (consumerd32_libdir[0] != '\0') {
				char *tmp;
				size_t tmplen;

				tmp = getenv("LD_LIBRARY_PATH");
				if (!tmp) {
					tmp = "";
				}
				tmplen = strlen("LD_LIBRARY_PATH=")
					+ strlen(consumerd32_libdir) + 1 /* : */ + strlen(tmp);
				tmpnew = zmalloc(tmplen + 1 /* \0 */);
				if (!tmpnew) {
					ret = -ENOMEM;
					goto error;
				}
				strcpy(tmpnew, "LD_LIBRARY_PATH=");
				strcat(tmpnew, consumerd32_libdir);
				if (tmp[0] != '\0') {
					strcat(tmpnew, ":");
					strcat(tmpnew, tmp);
				}
				ret = putenv(tmpnew);
				if (ret) {
					ret = -errno;
					goto error;
				}
			}
			DBG("Using 32-bit UST consumer at: %s",  consumerd32_bin);
			ret = execl(consumerd32_bin, "lttng-consumerd", verbosity, "-u",
					"--consumerd-cmd-sock", consumer_data->cmd_unix_sock_path,
					"--consumerd-err-sock", consumer_data->err_unix_sock_path,
					NULL);
			if (consumerd32_libdir[0] != '\0') {
				free(tmpnew);
			}
			if (ret) {
				goto error;
			}
			break;
		}
		default:
			PERROR("unknown consumer type");
			exit(EXIT_FAILURE);
		}
		if (errno != 0) {
			PERROR("kernel start consumer exec");
		}
		exit(EXIT_FAILURE);
	} else if (pid > 0) {
		ret = pid;
	} else {
		PERROR("start consumer fork");
		ret = -errno;
	}
error:
	return ret;
}

/*
 * Spawn the consumerd daemon and session daemon thread.
 */
static int start_consumerd(struct consumer_data *consumer_data)
{
	int ret;

	pthread_mutex_lock(&consumer_data->pid_mutex);
	if (consumer_data->pid != 0) {
		pthread_mutex_unlock(&consumer_data->pid_mutex);
		goto end;
	}

	ret = spawn_consumerd(consumer_data);
	if (ret < 0) {
		ERR("Spawning consumerd failed");
		pthread_mutex_unlock(&consumer_data->pid_mutex);
		goto error;
	}

	/* Setting up the consumer_data pid */
	consumer_data->pid = ret;
	DBG2("Consumer pid %d", consumer_data->pid);
	pthread_mutex_unlock(&consumer_data->pid_mutex);

	DBG2("Spawning consumer control thread");
	ret = spawn_consumer_thread(consumer_data);
	if (ret < 0) {
		ERR("Fatal error spawning consumer control thread");
		goto error;
	}

end:
	return 0;

error:
	return ret;
}

/*
 * Compute health status of each consumer. If one of them is zero (bad
 * state), we return 0.
 */
static int check_consumer_health(void)
{
	int ret;

	ret = health_check_state(&kconsumer_data.health) &&
		health_check_state(&ustconsumer32_data.health) &&
		health_check_state(&ustconsumer64_data.health);

	DBG3("Health consumer check %d", ret);

	return ret;
}

/*
 * Setup necessary data for kernel tracer action.
 */
static int init_kernel_tracer(void)
{
	int ret;

	/* Modprobe lttng kernel modules */
	ret = modprobe_lttng_control();
	if (ret < 0) {
		goto error;
	}

	/* Open debugfs lttng */
	kernel_tracer_fd = open(module_proc_lttng, O_RDWR);
	if (kernel_tracer_fd < 0) {
		DBG("Failed to open %s", module_proc_lttng);
		ret = -1;
		goto error_open;
	}

	/* Validate kernel version */
	ret = kernel_validate_version(kernel_tracer_fd);
	if (ret < 0) {
		goto error_version;
	}

	ret = modprobe_lttng_data();
	if (ret < 0) {
		goto error_modules;
	}

	DBG("Kernel tracer fd %d", kernel_tracer_fd);
	return 0;

error_version:
	modprobe_remove_lttng_control();
	ret = close(kernel_tracer_fd);
	if (ret) {
		PERROR("close");
	}
	kernel_tracer_fd = -1;
	return LTTNG_ERR_KERN_VERSION;

error_modules:
	ret = close(kernel_tracer_fd);
	if (ret) {
		PERROR("close");
	}

error_open:
	modprobe_remove_lttng_control();

error:
	WARN("No kernel tracer available");
	kernel_tracer_fd = -1;
	if (!is_root) {
		return LTTNG_ERR_NEED_ROOT_SESSIOND;
	} else {
		return LTTNG_ERR_KERN_NA;
	}
}


/*
 * Copy consumer output from the tracing session to the domain session. The
 * function also applies the right modification on a per domain basis for the
 * trace files destination directory.
 */
static int copy_session_consumer(int domain, struct ltt_session *session)
{
	int ret;
	const char *dir_name;
	struct consumer_output *consumer;

	assert(session);
	assert(session->consumer);

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		DBG3("Copying tracing session consumer output in kernel session");
		session->kernel_session->consumer =
			consumer_copy_output(session->consumer);
		/* Ease our life a bit for the next part */
		consumer = session->kernel_session->consumer;
		dir_name = DEFAULT_KERNEL_TRACE_DIR;
		break;
	case LTTNG_DOMAIN_UST:
		DBG3("Copying tracing session consumer output in UST session");
		session->ust_session->consumer =
			consumer_copy_output(session->consumer);
		/* Ease our life a bit for the next part */
		consumer = session->ust_session->consumer;
		dir_name = DEFAULT_UST_TRACE_DIR;
		break;
	default:
		ret = LTTNG_ERR_UNKNOWN_DOMAIN;
		goto error;
	}

	/* Append correct directory to subdir */
	strncat(consumer->subdir, dir_name,
			sizeof(consumer->subdir) - strlen(consumer->subdir) - 1);
	DBG3("Copy session consumer subdir %s", consumer->subdir);

	ret = LTTNG_OK;

error:
	return ret;
}

/*
 * Create an UST session and add it to the session ust list.
 */
static int create_ust_session(struct ltt_session *session,
		struct lttng_domain *domain)
{
	int ret;
	struct ltt_ust_session *lus = NULL;

	assert(session);
	assert(domain);
	assert(session->consumer);

	switch (domain->type) {
	case LTTNG_DOMAIN_UST:
		break;
	default:
		ERR("Unknown UST domain on create session %d", domain->type);
		ret = LTTNG_ERR_UNKNOWN_DOMAIN;
		goto error;
	}

	DBG("Creating UST session");

	lus = trace_ust_create_session(session->path, session->id, domain);
	if (lus == NULL) {
		ret = LTTNG_ERR_UST_SESS_FAIL;
		goto error;
	}

	lus->uid = session->uid;
	lus->gid = session->gid;
	session->ust_session = lus;

	/* Copy session output to the newly created UST session */
	ret = copy_session_consumer(domain->type, session);
	if (ret != LTTNG_OK) {
		goto error;
	}

	return LTTNG_OK;

error:
	free(lus);
	session->ust_session = NULL;
	return ret;
}

/*
 * Create a kernel tracer session then create the default channel.
 */
static int create_kernel_session(struct ltt_session *session)
{
	int ret;

	DBG("Creating kernel session");

	ret = kernel_create_session(session, kernel_tracer_fd);
	if (ret < 0) {
		ret = LTTNG_ERR_KERN_SESS_FAIL;
		goto error;
	}

	/* Code flow safety */
	assert(session->kernel_session);

	/* Copy session output to the newly created Kernel session */
	ret = copy_session_consumer(LTTNG_DOMAIN_KERNEL, session);
	if (ret != LTTNG_OK) {
		goto error;
	}

	/* Create directory(ies) on local filesystem. */
	if (session->kernel_session->consumer->type == CONSUMER_DST_LOCAL &&
			strlen(session->kernel_session->consumer->dst.trace_path) > 0) {
		ret = run_as_mkdir_recursive(
				session->kernel_session->consumer->dst.trace_path,
				S_IRWXU | S_IRWXG, session->uid, session->gid);
		if (ret < 0) {
			if (ret != -EEXIST) {
				ERR("Trace directory creation error");
				goto error;
			}
		}
	}

	session->kernel_session->uid = session->uid;
	session->kernel_session->gid = session->gid;

	return LTTNG_OK;

error:
	trace_kernel_destroy_session(session->kernel_session);
	session->kernel_session = NULL;
	return ret;
}

/*
 * Count number of session permitted by uid/gid.
 */
static unsigned int lttng_sessions_count(uid_t uid, gid_t gid)
{
	unsigned int i = 0;
	struct ltt_session *session;

	DBG("Counting number of available session for UID %d GID %d",
			uid, gid);
	cds_list_for_each_entry(session, &session_list_ptr->head, list) {
		/*
		 * Only list the sessions the user can control.
		 */
		if (!session_access_ok(session, uid, gid)) {
			continue;
		}
		i++;
	}
	return i;
}

/*
 * Process the command requested by the lttng client within the command
 * context structure. This function make sure that the return structure (llm)
 * is set and ready for transmission before returning.
 *
 * Return any error encountered or 0 for success.
 *
 * "sock" is only used for special-case var. len data.
 */
static int process_client_msg(struct command_ctx *cmd_ctx, int sock,
		int *sock_error)
{
	int ret = LTTNG_OK;
	int need_tracing_session = 1;
	int need_domain;

	DBG("Processing client command %d", cmd_ctx->lsm->cmd_type);

	*sock_error = 0;

	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_CREATE_SESSION:
	case LTTNG_DESTROY_SESSION:
	case LTTNG_LIST_SESSIONS:
	case LTTNG_LIST_DOMAINS:
	case LTTNG_START_TRACE:
	case LTTNG_STOP_TRACE:
		need_domain = 0;
		break;
	default:
		need_domain = 1;
	}

	if (opt_no_kernel && need_domain
			&& cmd_ctx->lsm->domain.type == LTTNG_DOMAIN_KERNEL) {
		if (!is_root) {
			ret = LTTNG_ERR_NEED_ROOT_SESSIOND;
		} else {
			ret = LTTNG_ERR_KERN_NA;
		}
		goto error;
	}

	/* Deny register consumer if we already have a spawned consumer. */
	if (cmd_ctx->lsm->cmd_type == LTTNG_REGISTER_CONSUMER) {
		pthread_mutex_lock(&kconsumer_data.pid_mutex);
		if (kconsumer_data.pid > 0) {
			ret = LTTNG_ERR_KERN_CONSUMER_FAIL;
			pthread_mutex_unlock(&kconsumer_data.pid_mutex);
			goto error;
		}
		pthread_mutex_unlock(&kconsumer_data.pid_mutex);
	}

	/*
	 * Check for command that don't needs to allocate a returned payload. We do
	 * this here so we don't have to make the call for no payload at each
	 * command.
	 */
	switch(cmd_ctx->lsm->cmd_type) {
	case LTTNG_LIST_SESSIONS:
	case LTTNG_LIST_TRACEPOINTS:
	case LTTNG_LIST_TRACEPOINT_FIELDS:
	case LTTNG_LIST_DOMAINS:
	case LTTNG_LIST_CHANNELS:
	case LTTNG_LIST_EVENTS:
		break;
	default:
		/* Setup lttng message with no payload */
		ret = setup_lttng_msg(cmd_ctx, 0);
		if (ret < 0) {
			/* This label does not try to unlock the session */
			goto init_setup_error;
		}
	}

	/* Commands that DO NOT need a session. */
	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_CREATE_SESSION:
	case LTTNG_CALIBRATE:
	case LTTNG_LIST_SESSIONS:
	case LTTNG_LIST_TRACEPOINTS:
	case LTTNG_LIST_TRACEPOINT_FIELDS:
		need_tracing_session = 0;
		break;
	default:
		DBG("Getting session %s by name", cmd_ctx->lsm->session.name);
		/*
		 * We keep the session list lock across _all_ commands
		 * for now, because the per-session lock does not
		 * handle teardown properly.
		 */
		session_lock_list();
		cmd_ctx->session = session_find_by_name(cmd_ctx->lsm->session.name);
		if (cmd_ctx->session == NULL) {
			if (cmd_ctx->lsm->session.name != NULL) {
				ret = LTTNG_ERR_SESS_NOT_FOUND;
			} else {
				/* If no session name specified */
				ret = LTTNG_ERR_SELECT_SESS;
			}
			goto error;
		} else {
			/* Acquire lock for the session */
			session_lock(cmd_ctx->session);
		}
		break;
	}

	if (!need_domain) {
		goto skip_domain;
	}

	/*
	 * Check domain type for specific "pre-action".
	 */
	switch (cmd_ctx->lsm->domain.type) {
	case LTTNG_DOMAIN_KERNEL:
		if (!is_root) {
			ret = LTTNG_ERR_NEED_ROOT_SESSIOND;
			goto error;
		}

		/* Kernel tracer check */
		if (kernel_tracer_fd == -1) {
			/* Basically, load kernel tracer modules */
			ret = init_kernel_tracer();
			if (ret != 0) {
				goto error;
			}
		}

		/* Consumer is in an ERROR state. Report back to client */
		if (uatomic_read(&kernel_consumerd_state) == CONSUMER_ERROR) {
			ret = LTTNG_ERR_NO_KERNCONSUMERD;
			goto error;
		}

		/* Need a session for kernel command */
		if (need_tracing_session) {
			if (cmd_ctx->session->kernel_session == NULL) {
				ret = create_kernel_session(cmd_ctx->session);
				if (ret < 0) {
					ret = LTTNG_ERR_KERN_SESS_FAIL;
					goto error;
				}
			}

			/* Start the kernel consumer daemon */
			pthread_mutex_lock(&kconsumer_data.pid_mutex);
			if (kconsumer_data.pid == 0 &&
					cmd_ctx->lsm->cmd_type != LTTNG_REGISTER_CONSUMER &&
					cmd_ctx->session->start_consumer) {
				pthread_mutex_unlock(&kconsumer_data.pid_mutex);
				ret = start_consumerd(&kconsumer_data);
				if (ret < 0) {
					ret = LTTNG_ERR_KERN_CONSUMER_FAIL;
					goto error;
				}
				uatomic_set(&kernel_consumerd_state, CONSUMER_STARTED);
			} else {
				pthread_mutex_unlock(&kconsumer_data.pid_mutex);
			}

			/*
			 * The consumer was just spawned so we need to add the socket to
			 * the consumer output of the session if exist.
			 */
			ret = consumer_create_socket(&kconsumer_data,
					cmd_ctx->session->kernel_session->consumer);
			if (ret < 0) {
				goto error;
			}
		}

		break;
	case LTTNG_DOMAIN_UST:
	{
		/* Consumer is in an ERROR state. Report back to client */
		if (uatomic_read(&ust_consumerd_state) == CONSUMER_ERROR) {
			ret = LTTNG_ERR_NO_USTCONSUMERD;
			goto error;
		}

		if (need_tracing_session) {
			/* Create UST session if none exist. */
			if (cmd_ctx->session->ust_session == NULL) {
				ret = create_ust_session(cmd_ctx->session,
						&cmd_ctx->lsm->domain);
				if (ret != LTTNG_OK) {
					goto error;
				}
			}

			/* Start the UST consumer daemons */
			/* 64-bit */
			pthread_mutex_lock(&ustconsumer64_data.pid_mutex);
			if (consumerd64_bin[0] != '\0' &&
					ustconsumer64_data.pid == 0 &&
					cmd_ctx->lsm->cmd_type != LTTNG_REGISTER_CONSUMER &&
					cmd_ctx->session->start_consumer) {
				pthread_mutex_unlock(&ustconsumer64_data.pid_mutex);
				ret = start_consumerd(&ustconsumer64_data);
				if (ret < 0) {
					ret = LTTNG_ERR_UST_CONSUMER64_FAIL;
					uatomic_set(&ust_consumerd64_fd, -EINVAL);
					goto error;
				}

				uatomic_set(&ust_consumerd64_fd, ustconsumer64_data.cmd_sock);
				uatomic_set(&ust_consumerd_state, CONSUMER_STARTED);
			} else {
				pthread_mutex_unlock(&ustconsumer64_data.pid_mutex);
			}

			/*
			 * Setup socket for consumer 64 bit. No need for atomic access
			 * since it was set above and can ONLY be set in this thread.
			 */
			ret = consumer_create_socket(&ustconsumer64_data,
					cmd_ctx->session->ust_session->consumer);
			if (ret < 0) {
				goto error;
			}

			/* 32-bit */
			if (consumerd32_bin[0] != '\0' &&
					ustconsumer32_data.pid == 0 &&
					cmd_ctx->lsm->cmd_type != LTTNG_REGISTER_CONSUMER &&
					cmd_ctx->session->start_consumer) {
				pthread_mutex_unlock(&ustconsumer32_data.pid_mutex);
				ret = start_consumerd(&ustconsumer32_data);
				if (ret < 0) {
					ret = LTTNG_ERR_UST_CONSUMER32_FAIL;
					uatomic_set(&ust_consumerd32_fd, -EINVAL);
					goto error;
				}

				uatomic_set(&ust_consumerd32_fd, ustconsumer32_data.cmd_sock);
				uatomic_set(&ust_consumerd_state, CONSUMER_STARTED);
			} else {
				pthread_mutex_unlock(&ustconsumer32_data.pid_mutex);
			}

			/*
			 * Setup socket for consumer 64 bit. No need for atomic access
			 * since it was set above and can ONLY be set in this thread.
			 */
			ret = consumer_create_socket(&ustconsumer32_data,
					cmd_ctx->session->ust_session->consumer);
			if (ret < 0) {
				goto error;
			}
		}
		break;
	}
	default:
		break;
	}
skip_domain:

	/* Validate consumer daemon state when start/stop trace command */
	if (cmd_ctx->lsm->cmd_type == LTTNG_START_TRACE ||
			cmd_ctx->lsm->cmd_type == LTTNG_STOP_TRACE) {
		switch (cmd_ctx->lsm->domain.type) {
		case LTTNG_DOMAIN_UST:
			if (uatomic_read(&ust_consumerd_state) != CONSUMER_STARTED) {
				ret = LTTNG_ERR_NO_USTCONSUMERD;
				goto error;
			}
			break;
		case LTTNG_DOMAIN_KERNEL:
			if (uatomic_read(&kernel_consumerd_state) != CONSUMER_STARTED) {
				ret = LTTNG_ERR_NO_KERNCONSUMERD;
				goto error;
			}
			break;
		}
	}

	/*
	 * Check that the UID or GID match that of the tracing session.
	 * The root user can interact with all sessions.
	 */
	if (need_tracing_session) {
		if (!session_access_ok(cmd_ctx->session,
				LTTNG_SOCK_GET_UID_CRED(&cmd_ctx->creds),
				LTTNG_SOCK_GET_GID_CRED(&cmd_ctx->creds))) {
			ret = LTTNG_ERR_EPERM;
			goto error;
		}
	}

	/* Process by command type */
	switch (cmd_ctx->lsm->cmd_type) {
	case LTTNG_ADD_CONTEXT:
	{
		ret = cmd_add_context(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.context.channel_name,
				cmd_ctx->lsm->u.context.event_name,
				&cmd_ctx->lsm->u.context.ctx);
		break;
	}
	case LTTNG_DISABLE_CHANNEL:
	{
		ret = cmd_disable_channel(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.disable.channel_name);
		break;
	}
	case LTTNG_DISABLE_EVENT:
	{
		ret = cmd_disable_event(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.disable.channel_name,
				cmd_ctx->lsm->u.disable.name);
		break;
	}
	case LTTNG_DISABLE_ALL_EVENT:
	{
		DBG("Disabling all events");

		ret = cmd_disable_event_all(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.disable.channel_name);
		break;
	}
	case LTTNG_DISABLE_CONSUMER:
	{
		ret = cmd_disable_consumer(cmd_ctx->lsm->domain.type, cmd_ctx->session);
		break;
	}
	case LTTNG_ENABLE_CHANNEL:
	{
		ret = cmd_enable_channel(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				&cmd_ctx->lsm->u.channel.chan, kernel_poll_pipe[1]);
		break;
	}
	case LTTNG_ENABLE_CONSUMER:
	{
		/*
		 * XXX: 0 means that this URI should be applied on the session. Should
		 * be a DOMAIN enuam.
		 */
		ret = cmd_enable_consumer(cmd_ctx->lsm->domain.type, cmd_ctx->session);
		if (ret != LTTNG_OK) {
			goto error;
		}

		if (cmd_ctx->lsm->domain.type == 0) {
			/* Add the URI for the UST session if a consumer is present. */
			if (cmd_ctx->session->ust_session &&
					cmd_ctx->session->ust_session->consumer) {
				ret = cmd_enable_consumer(LTTNG_DOMAIN_UST, cmd_ctx->session);
			} else if (cmd_ctx->session->kernel_session &&
					cmd_ctx->session->kernel_session->consumer) {
				ret = cmd_enable_consumer(LTTNG_DOMAIN_KERNEL,
						cmd_ctx->session);
			}
		}
		break;
	}
	case LTTNG_ENABLE_EVENT:
	{
		ret = cmd_enable_event(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.enable.channel_name,
				&cmd_ctx->lsm->u.enable.event, kernel_poll_pipe[1]);
		break;
	}
	case LTTNG_ENABLE_ALL_EVENT:
	{
		DBG("Enabling all events");

		ret = cmd_enable_event_all(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.enable.channel_name,
				cmd_ctx->lsm->u.enable.event.type, kernel_poll_pipe[1]);
		break;
	}
	case LTTNG_LIST_TRACEPOINTS:
	{
		struct lttng_event *events;
		ssize_t nb_events;

		nb_events = cmd_list_tracepoints(cmd_ctx->lsm->domain.type, &events);
		if (nb_events < 0) {
			/* Return value is a negative lttng_error_code. */
			ret = -nb_events;
			goto error;
		}

		/*
		 * Setup lttng message with payload size set to the event list size in
		 * bytes and then copy list into the llm payload.
		 */
		ret = setup_lttng_msg(cmd_ctx, sizeof(struct lttng_event) * nb_events);
		if (ret < 0) {
			free(events);
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, events,
				sizeof(struct lttng_event) * nb_events);

		free(events);

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_LIST_TRACEPOINT_FIELDS:
	{
		struct lttng_event_field *fields;
		ssize_t nb_fields;

		nb_fields = cmd_list_tracepoint_fields(cmd_ctx->lsm->domain.type,
				&fields);
		if (nb_fields < 0) {
			/* Return value is a negative lttng_error_code. */
			ret = -nb_fields;
			goto error;
		}

		/*
		 * Setup lttng message with payload size set to the event list size in
		 * bytes and then copy list into the llm payload.
		 */
		ret = setup_lttng_msg(cmd_ctx,
				sizeof(struct lttng_event_field) * nb_fields);
		if (ret < 0) {
			free(fields);
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, fields,
				sizeof(struct lttng_event_field) * nb_fields);

		free(fields);

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_SET_CONSUMER_URI:
	{
		size_t nb_uri, len;
		struct lttng_uri *uris;

		nb_uri = cmd_ctx->lsm->u.uri.size;
		len = nb_uri * sizeof(struct lttng_uri);

		if (nb_uri == 0) {
			ret = LTTNG_ERR_INVALID;
			goto error;
		}

		uris = zmalloc(len);
		if (uris == NULL) {
			ret = LTTNG_ERR_FATAL;
			goto error;
		}

		/* Receive variable len data */
		DBG("Receiving %zu URI(s) from client ...", nb_uri);
		ret = lttcomm_recv_unix_sock(sock, uris, len);
		if (ret <= 0) {
			DBG("No URIs received from client... continuing");
			*sock_error = 1;
			ret = LTTNG_ERR_SESSION_FAIL;
			goto error;
		}

		ret = cmd_set_consumer_uri(cmd_ctx->lsm->domain.type, cmd_ctx->session,
				nb_uri, uris);
		if (ret != LTTNG_OK) {
			goto error;
		}

		/*
		 * XXX: 0 means that this URI should be applied on the session. Should
		 * be a DOMAIN enuam.
		 */
		if (cmd_ctx->lsm->domain.type == 0) {
			/* Add the URI for the UST session if a consumer is present. */
			if (cmd_ctx->session->ust_session &&
					cmd_ctx->session->ust_session->consumer) {
				ret = cmd_set_consumer_uri(LTTNG_DOMAIN_UST, cmd_ctx->session,
						nb_uri, uris);
			} else if (cmd_ctx->session->kernel_session &&
					cmd_ctx->session->kernel_session->consumer) {
				ret = cmd_set_consumer_uri(LTTNG_DOMAIN_KERNEL,
						cmd_ctx->session, nb_uri, uris);
			}
		}

		break;
	}
	case LTTNG_START_TRACE:
	{
		ret = cmd_start_trace(cmd_ctx->session);
		break;
	}
	case LTTNG_STOP_TRACE:
	{
		ret = cmd_stop_trace(cmd_ctx->session);
		break;
	}
	case LTTNG_CREATE_SESSION:
	{
		size_t nb_uri, len;
		struct lttng_uri *uris = NULL;

		nb_uri = cmd_ctx->lsm->u.uri.size;
		len = nb_uri * sizeof(struct lttng_uri);

		if (nb_uri > 0) {
			uris = zmalloc(len);
			if (uris == NULL) {
				ret = LTTNG_ERR_FATAL;
				goto error;
			}

			/* Receive variable len data */
			DBG("Waiting for %zu URIs from client ...", nb_uri);
			ret = lttcomm_recv_unix_sock(sock, uris, len);
			if (ret <= 0) {
				DBG("No URIs received from client... continuing");
				*sock_error = 1;
				ret = LTTNG_ERR_SESSION_FAIL;
				goto error;
			}

			if (nb_uri == 1 && uris[0].dtype != LTTNG_DST_PATH) {
				DBG("Creating session with ONE network URI is a bad call");
				ret = LTTNG_ERR_SESSION_FAIL;
				goto error;
			}
		}

		ret = cmd_create_session_uri(cmd_ctx->lsm->session.name, uris, nb_uri,
			&cmd_ctx->creds);

		break;
	}
	case LTTNG_DESTROY_SESSION:
	{
		ret = cmd_destroy_session(cmd_ctx->session, kernel_poll_pipe[1]);

		/* Set session to NULL so we do not unlock it after free. */
		cmd_ctx->session = NULL;
		break;
	}
	case LTTNG_LIST_DOMAINS:
	{
		ssize_t nb_dom;
		struct lttng_domain *domains;

		nb_dom = cmd_list_domains(cmd_ctx->session, &domains);
		if (nb_dom < 0) {
			/* Return value is a negative lttng_error_code. */
			ret = -nb_dom;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, nb_dom * sizeof(struct lttng_domain));
		if (ret < 0) {
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, domains,
				nb_dom * sizeof(struct lttng_domain));

		free(domains);

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_LIST_CHANNELS:
	{
		int nb_chan;
		struct lttng_channel *channels;

		nb_chan = cmd_list_channels(cmd_ctx->lsm->domain.type,
				cmd_ctx->session, &channels);
		if (nb_chan < 0) {
			/* Return value is a negative lttng_error_code. */
			ret = -nb_chan;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, nb_chan * sizeof(struct lttng_channel));
		if (ret < 0) {
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, channels,
				nb_chan * sizeof(struct lttng_channel));

		free(channels);

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_LIST_EVENTS:
	{
		ssize_t nb_event;
		struct lttng_event *events = NULL;

		nb_event = cmd_list_events(cmd_ctx->lsm->domain.type, cmd_ctx->session,
				cmd_ctx->lsm->u.list.channel_name, &events);
		if (nb_event < 0) {
			/* Return value is a negative lttng_error_code. */
			ret = -nb_event;
			goto error;
		}

		ret = setup_lttng_msg(cmd_ctx, nb_event * sizeof(struct lttng_event));
		if (ret < 0) {
			goto setup_error;
		}

		/* Copy event list into message payload */
		memcpy(cmd_ctx->llm->payload, events,
				nb_event * sizeof(struct lttng_event));

		free(events);

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_LIST_SESSIONS:
	{
		unsigned int nr_sessions;

		session_lock_list();
		nr_sessions = lttng_sessions_count(
				LTTNG_SOCK_GET_UID_CRED(&cmd_ctx->creds),
				LTTNG_SOCK_GET_GID_CRED(&cmd_ctx->creds));

		ret = setup_lttng_msg(cmd_ctx, sizeof(struct lttng_session) * nr_sessions);
		if (ret < 0) {
			session_unlock_list();
			goto setup_error;
		}

		/* Filled the session array */
		cmd_list_lttng_sessions((struct lttng_session *)(cmd_ctx->llm->payload),
			LTTNG_SOCK_GET_UID_CRED(&cmd_ctx->creds),
			LTTNG_SOCK_GET_GID_CRED(&cmd_ctx->creds));

		session_unlock_list();

		ret = LTTNG_OK;
		break;
	}
	case LTTNG_CALIBRATE:
	{
		ret = cmd_calibrate(cmd_ctx->lsm->domain.type,
				&cmd_ctx->lsm->u.calibrate);
		break;
	}
	case LTTNG_REGISTER_CONSUMER:
	{
		struct consumer_data *cdata;

		switch (cmd_ctx->lsm->domain.type) {
		case LTTNG_DOMAIN_KERNEL:
			cdata = &kconsumer_data;
			break;
		default:
			ret = LTTNG_ERR_UND;
			goto error;
		}

		ret = cmd_register_consumer(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.reg.path, cdata);
		break;
	}
	case LTTNG_SET_FILTER:
	{
		struct lttng_filter_bytecode *bytecode;

		if (cmd_ctx->lsm->u.filter.bytecode_len > LTTNG_FILTER_MAX_LEN) {
			ret = LTTNG_ERR_FILTER_INVAL;
			goto error;
		}
		bytecode = zmalloc(cmd_ctx->lsm->u.filter.bytecode_len);
		if (!bytecode) {
			ret = LTTNG_ERR_FILTER_NOMEM;
			goto error;
		}
		/* Receive var. len. data */
		DBG("Receiving var len data from client ...");
		ret = lttcomm_recv_unix_sock(sock, bytecode,
				cmd_ctx->lsm->u.filter.bytecode_len);
		if (ret <= 0) {
			DBG("Nothing recv() from client var len data... continuing");
			*sock_error = 1;
			ret = LTTNG_ERR_FILTER_INVAL;
			goto error;
		}

		if (bytecode->len + sizeof(*bytecode)
				!= cmd_ctx->lsm->u.filter.bytecode_len) {
			free(bytecode);
			ret = LTTNG_ERR_FILTER_INVAL;
			goto error;
		}

		ret = cmd_set_filter(cmd_ctx->session, cmd_ctx->lsm->domain.type,
				cmd_ctx->lsm->u.filter.channel_name,
				cmd_ctx->lsm->u.filter.event_name,
				bytecode);
		break;
	}
	default:
		ret = LTTNG_ERR_UND;
		break;
	}

error:
	if (cmd_ctx->llm == NULL) {
		DBG("Missing llm structure. Allocating one.");
		if (setup_lttng_msg(cmd_ctx, 0) < 0) {
			goto setup_error;
		}
	}
	/* Set return code */
	cmd_ctx->llm->ret_code = ret;
setup_error:
	if (cmd_ctx->session) {
		session_unlock(cmd_ctx->session);
	}
	if (need_tracing_session) {
		session_unlock_list();
	}
init_setup_error:
	return ret;
}

/*
 * Thread managing health check socket.
 */
static void *thread_manage_health(void *data)
{
	int sock = -1, new_sock = -1, ret, i, pollfd, err = -1;
	uint32_t revents, nb_fd;
	struct lttng_poll_event events;
	struct lttcomm_health_msg msg;
	struct lttcomm_health_data reply;

	DBG("[thread] Manage health check started");

	rcu_register_thread();

	/* Create unix socket */
	sock = lttcomm_create_unix_sock(health_unix_sock_path);
	if (sock < 0) {
		ERR("Unable to create health check Unix socket");
		ret = -1;
		goto error;
	}

	ret = lttcomm_listen_unix_sock(sock);
	if (ret < 0) {
		goto error;
	}

	/*
	 * Pass 2 as size here for the thread quit pipe and client_sock. Nothing
	 * more will be added to this poll set.
	 */
	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error;
	}

	/* Add the application registration socket */
	ret = lttng_poll_add(&events, sock, LPOLLIN | LPOLLPRI);
	if (ret < 0) {
		goto error;
	}

	while (1) {
		DBG("Health check ready");

		nb_fd = LTTNG_POLL_GETNB(&events);

		/* Inifinite blocking call, waiting for transmission */
restart:
		ret = lttng_poll_wait(&events, -1);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_thread_quit_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Event on the registration socket */
			if (pollfd == sock) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("Health socket poll error");
					goto error;
				}
			}
		}

		new_sock = lttcomm_accept_unix_sock(sock);
		if (new_sock < 0) {
			goto error;
		}

		DBG("Receiving data from client for health...");
		ret = lttcomm_recv_unix_sock(new_sock, (void *)&msg, sizeof(msg));
		if (ret <= 0) {
			DBG("Nothing recv() from client... continuing");
			ret = close(new_sock);
			if (ret) {
				PERROR("close");
			}
			new_sock = -1;
			continue;
		}

		rcu_thread_online();

		switch (msg.component) {
		case LTTNG_HEALTH_CMD:
			reply.ret_code = health_check_state(&health_thread_cmd);
			break;
		case LTTNG_HEALTH_APP_MANAGE:
			reply.ret_code = health_check_state(&health_thread_app_manage);
			break;
		case LTTNG_HEALTH_APP_REG:
			reply.ret_code = health_check_state(&health_thread_app_reg);
			break;
		case LTTNG_HEALTH_KERNEL:
			reply.ret_code = health_check_state(&health_thread_kernel);
			break;
		case LTTNG_HEALTH_CONSUMER:
			reply.ret_code = check_consumer_health();
			break;
		case LTTNG_HEALTH_ALL:
			reply.ret_code =
				health_check_state(&health_thread_app_manage) &&
				health_check_state(&health_thread_app_reg) &&
				health_check_state(&health_thread_cmd) &&
				health_check_state(&health_thread_kernel) &&
				check_consumer_health();
			break;
		default:
			reply.ret_code = LTTNG_ERR_UND;
			break;
		}

		/*
		 * Flip ret value since 0 is a success and 1 indicates a bad health for
		 * the client where in the sessiond it is the opposite. Again, this is
		 * just to make things easier for us poor developer which enjoy a lot
		 * lazyness.
		 */
		if (reply.ret_code == 0 || reply.ret_code == 1) {
			reply.ret_code = !reply.ret_code;
		}

		DBG2("Health check return value %d", reply.ret_code);

		ret = send_unix_sock(new_sock, (void *) &reply, sizeof(reply));
		if (ret < 0) {
			ERR("Failed to send health data back to client");
		}

		/* End of transmission */
		ret = close(new_sock);
		if (ret) {
			PERROR("close");
		}
		new_sock = -1;
	}

exit:
error:
	if (err) {
		ERR("Health error occurred in %s", __func__);
	}
	DBG("Health check thread dying");
	unlink(health_unix_sock_path);
	if (sock >= 0) {
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
	}
	if (new_sock >= 0) {
		ret = close(new_sock);
		if (ret) {
			PERROR("close");
		}
	}

	lttng_poll_clean(&events);

	rcu_unregister_thread();
	return NULL;
}

/*
 * This thread manage all clients request using the unix client socket for
 * communication.
 */
static void *thread_manage_clients(void *data)
{
	int sock = -1, ret, i, pollfd, err = -1;
	int sock_error;
	uint32_t revents, nb_fd;
	struct command_ctx *cmd_ctx = NULL;
	struct lttng_poll_event events;

	DBG("[thread] Manage client started");

	rcu_register_thread();

	health_code_update(&health_thread_cmd);

	ret = lttcomm_listen_unix_sock(client_sock);
	if (ret < 0) {
		goto error;
	}

	/*
	 * Pass 2 as size here for the thread quit pipe and client_sock. Nothing
	 * more will be added to this poll set.
	 */
	ret = create_thread_poll_set(&events, 2);
	if (ret < 0) {
		goto error;
	}

	/* Add the application registration socket */
	ret = lttng_poll_add(&events, client_sock, LPOLLIN | LPOLLPRI);
	if (ret < 0) {
		goto error;
	}

	/*
	 * Notify parent pid that we are ready to accept command for client side.
	 */
	if (opt_sig_parent) {
		kill(ppid, SIGUSR1);
	}

	health_code_update(&health_thread_cmd);

	while (1) {
		DBG("Accepting client command ...");

		nb_fd = LTTNG_POLL_GETNB(&events);

		/* Inifinite blocking call, waiting for transmission */
	restart:
		health_poll_update(&health_thread_cmd);
		ret = lttng_poll_wait(&events, -1);
		health_poll_update(&health_thread_cmd);
		if (ret < 0) {
			/*
			 * Restart interrupted system call.
			 */
			if (errno == EINTR) {
				goto restart;
			}
			goto error;
		}

		for (i = 0; i < nb_fd; i++) {
			/* Fetch once the poll data */
			revents = LTTNG_POLL_GETEV(&events, i);
			pollfd = LTTNG_POLL_GETFD(&events, i);

			health_code_update(&health_thread_cmd);

			/* Thread quit pipe has been closed. Killing thread. */
			ret = check_thread_quit_pipe(pollfd, revents);
			if (ret) {
				err = 0;
				goto exit;
			}

			/* Event on the registration socket */
			if (pollfd == client_sock) {
				if (revents & (LPOLLERR | LPOLLHUP | LPOLLRDHUP)) {
					ERR("Client socket poll error");
					goto error;
				}
			}
		}

		DBG("Wait for client response");

		health_code_update(&health_thread_cmd);

		sock = lttcomm_accept_unix_sock(client_sock);
		if (sock < 0) {
			goto error;
		}

		/* Set socket option for credentials retrieval */
		ret = lttcomm_setsockopt_creds_unix_sock(sock);
		if (ret < 0) {
			goto error;
		}

		/* Allocate context command to process the client request */
		cmd_ctx = zmalloc(sizeof(struct command_ctx));
		if (cmd_ctx == NULL) {
			PERROR("zmalloc cmd_ctx");
			goto error;
		}

		/* Allocate data buffer for reception */
		cmd_ctx->lsm = zmalloc(sizeof(struct lttcomm_session_msg));
		if (cmd_ctx->lsm == NULL) {
			PERROR("zmalloc cmd_ctx->lsm");
			goto error;
		}

		cmd_ctx->llm = NULL;
		cmd_ctx->session = NULL;

		health_code_update(&health_thread_cmd);

		/*
		 * Data is received from the lttng client. The struct
		 * lttcomm_session_msg (lsm) contains the command and data request of
		 * the client.
		 */
		DBG("Receiving data from client ...");
		ret = lttcomm_recv_creds_unix_sock(sock, cmd_ctx->lsm,
				sizeof(struct lttcomm_session_msg), &cmd_ctx->creds);
		if (ret <= 0) {
			DBG("Nothing recv() from client... continuing");
			ret = close(sock);
			if (ret) {
				PERROR("close");
			}
			sock = -1;
			clean_command_ctx(&cmd_ctx);
			continue;
		}

		health_code_update(&health_thread_cmd);

		// TODO: Validate cmd_ctx including sanity check for
		// security purpose.

		rcu_thread_online();
		/*
		 * This function dispatch the work to the kernel or userspace tracer
		 * libs and fill the lttcomm_lttng_msg data structure of all the needed
		 * informations for the client. The command context struct contains
		 * everything this function may needs.
		 */
		ret = process_client_msg(cmd_ctx, sock, &sock_error);
		rcu_thread_offline();
		if (ret < 0) {
			if (sock_error) {
				ret = close(sock);
				if (ret) {
					PERROR("close");
				}
				sock = -1;
			}
			/*
			 * TODO: Inform client somehow of the fatal error. At
			 * this point, ret < 0 means that a zmalloc failed
			 * (ENOMEM). Error detected but still accept
			 * command, unless a socket error has been
			 * detected.
			 */
			clean_command_ctx(&cmd_ctx);
			continue;
		}

		health_code_update(&health_thread_cmd);

		DBG("Sending response (size: %d, retcode: %s)",
				cmd_ctx->lttng_msg_size,
				lttng_strerror(-cmd_ctx->llm->ret_code));
		ret = send_unix_sock(sock, cmd_ctx->llm, cmd_ctx->lttng_msg_size);
		if (ret < 0) {
			ERR("Failed to send data back to client");
		}

		/* End of transmission */
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
		sock = -1;

		clean_command_ctx(&cmd_ctx);

		health_code_update(&health_thread_cmd);
	}

exit:
error:
	if (err) {
		health_error(&health_thread_cmd);
		ERR("Health error occurred in %s", __func__);
	}
	health_exit(&health_thread_cmd);

	DBG("Client thread dying");
	unlink(client_unix_sock_path);
	if (client_sock >= 0) {
		ret = close(client_sock);
		if (ret) {
			PERROR("close");
		}
	}
	if (sock >= 0) {
		ret = close(sock);
		if (ret) {
			PERROR("close");
		}
	}

	lttng_poll_clean(&events);
	clean_command_ctx(&cmd_ctx);

	rcu_unregister_thread();
	return NULL;
}


/*
 * usage function on stderr
 */
static void usage(void)
{
	fprintf(stderr, "Usage: %s OPTIONS\n\nOptions:\n", progname);
	fprintf(stderr, "  -h, --help                         Display this usage.\n");
	fprintf(stderr, "  -c, --client-sock PATH             Specify path for the client unix socket\n");
	fprintf(stderr, "  -a, --apps-sock PATH               Specify path for apps unix socket\n");
	fprintf(stderr, "      --kconsumerd-err-sock PATH     Specify path for the kernel consumer error socket\n");
	fprintf(stderr, "      --kconsumerd-cmd-sock PATH     Specify path for the kernel consumer command socket\n");
	fprintf(stderr, "      --ustconsumerd32-err-sock PATH Specify path for the 32-bit UST consumer error socket\n");
	fprintf(stderr, "      --ustconsumerd64-err-sock PATH Specify path for the 64-bit UST consumer error socket\n");
	fprintf(stderr, "      --ustconsumerd32-cmd-sock PATH Specify path for the 32-bit UST consumer command socket\n");
	fprintf(stderr, "      --ustconsumerd64-cmd-sock PATH Specify path for the 64-bit UST consumer command socket\n");
	fprintf(stderr, "      --consumerd32-path PATH     Specify path for the 32-bit UST consumer daemon binary\n");
	fprintf(stderr, "      --consumerd32-libdir PATH   Specify path for the 32-bit UST consumer daemon libraries\n");
	fprintf(stderr, "      --consumerd64-path PATH     Specify path for the 64-bit UST consumer daemon binary\n");
	fprintf(stderr, "      --consumerd64-libdir PATH   Specify path for the 64-bit UST consumer daemon libraries\n");
	fprintf(stderr, "  -d, --daemonize                    Start as a daemon.\n");
	fprintf(stderr, "  -g, --group NAME                   Specify the tracing group name. (default: tracing)\n");
	fprintf(stderr, "  -V, --version                      Show version number.\n");
	fprintf(stderr, "  -S, --sig-parent                   Send SIGCHLD to parent pid to notify readiness.\n");
	fprintf(stderr, "  -q, --quiet                        No output at all.\n");
	fprintf(stderr, "  -v, --verbose                      Verbose mode. Activate DBG() macro.\n");
	fprintf(stderr, "      --verbose-consumer             Verbose mode for consumer. Activate DBG() macro.\n");
	fprintf(stderr, "      --no-kernel                    Disable kernel tracer\n");
}

/*
 * daemon argument parsing
 */
static int parse_args(int argc, char **argv)
{
	int c;

	static struct option long_options[] = {
		{ "client-sock", 1, 0, 'c' },
		{ "apps-sock", 1, 0, 'a' },
		{ "kconsumerd-cmd-sock", 1, 0, 'C' },
		{ "kconsumerd-err-sock", 1, 0, 'E' },
		{ "ustconsumerd32-cmd-sock", 1, 0, 'G' },
		{ "ustconsumerd32-err-sock", 1, 0, 'H' },
		{ "ustconsumerd64-cmd-sock", 1, 0, 'D' },
		{ "ustconsumerd64-err-sock", 1, 0, 'F' },
		{ "consumerd32-path", 1, 0, 'u' },
		{ "consumerd32-libdir", 1, 0, 'U' },
		{ "consumerd64-path", 1, 0, 't' },
		{ "consumerd64-libdir", 1, 0, 'T' },
		{ "daemonize", 0, 0, 'd' },
		{ "sig-parent", 0, 0, 'S' },
		{ "help", 0, 0, 'h' },
		{ "group", 1, 0, 'g' },
		{ "version", 0, 0, 'V' },
		{ "quiet", 0, 0, 'q' },
		{ "verbose", 0, 0, 'v' },
		{ "verbose-consumer", 0, 0, 'Z' },
		{ "no-kernel", 0, 0, 'N' },
		{ NULL, 0, 0, 0 }
	};

	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "dhqvVSN" "a:c:g:s:C:E:D:F:Z:u:t",
				long_options, &option_index);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 0:
			fprintf(stderr, "option %s", long_options[option_index].name);
			if (optarg) {
				fprintf(stderr, " with arg %s\n", optarg);
			}
			break;
		case 'c':
			snprintf(client_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'a':
			snprintf(apps_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'd':
			opt_daemon = 1;
			break;
		case 'g':
			opt_tracing_group = optarg;
			break;
		case 'h':
			usage();
			exit(EXIT_FAILURE);
		case 'V':
			fprintf(stdout, "%s\n", VERSION);
			exit(EXIT_SUCCESS);
		case 'S':
			opt_sig_parent = 1;
			break;
		case 'E':
			snprintf(kconsumer_data.err_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'C':
			snprintf(kconsumer_data.cmd_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'F':
			snprintf(ustconsumer64_data.err_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'D':
			snprintf(ustconsumer64_data.cmd_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'H':
			snprintf(ustconsumer32_data.err_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'G':
			snprintf(ustconsumer32_data.cmd_unix_sock_path, PATH_MAX, "%s", optarg);
			break;
		case 'N':
			opt_no_kernel = 1;
			break;
		case 'q':
			lttng_opt_quiet = 1;
			break;
		case 'v':
			/* Verbose level can increase using multiple -v */
			lttng_opt_verbose += 1;
			break;
		case 'Z':
			opt_verbose_consumer += 1;
			break;
		case 'u':
			consumerd32_bin= optarg;
			break;
		case 'U':
			consumerd32_libdir = optarg;
			break;
		case 't':
			consumerd64_bin = optarg;
			break;
		case 'T':
			consumerd64_libdir = optarg;
			break;
		default:
			/* Unknown option or other error.
			 * Error is printed by getopt, just return */
			return -1;
		}
	}

	return 0;
}

/*
 * Creates the two needed socket by the daemon.
 * 	    apps_sock - The communication socket for all UST apps.
 * 	    client_sock - The communication of the cli tool (lttng).
 */
static int init_daemon_socket(void)
{
	int ret = 0;
	mode_t old_umask;

	old_umask = umask(0);

	/* Create client tool unix socket */
	client_sock = lttcomm_create_unix_sock(client_unix_sock_path);
	if (client_sock < 0) {
		ERR("Create unix sock failed: %s", client_unix_sock_path);
		ret = -1;
		goto end;
	}

	/* File permission MUST be 660 */
	ret = chmod(client_unix_sock_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", client_unix_sock_path);
		PERROR("chmod");
		goto end;
	}

	/* Create the application unix socket */
	apps_sock = lttcomm_create_unix_sock(apps_unix_sock_path);
	if (apps_sock < 0) {
		ERR("Create unix sock failed: %s", apps_unix_sock_path);
		ret = -1;
		goto end;
	}

	/* File permission MUST be 666 */
	ret = chmod(apps_unix_sock_path,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", apps_unix_sock_path);
		PERROR("chmod");
		goto end;
	}

end:
	umask(old_umask);
	return ret;
}

/*
 * Check if the global socket is available, and if a daemon is answering at the
 * other side. If yes, error is returned.
 */
static int check_existing_daemon(void)
{
	/* Is there anybody out there ? */
	if (lttng_session_daemon_alive()) {
		return -EEXIST;
	}

	return 0;
}

/*
 * Set the tracing group gid onto the client socket.
 *
 * Race window between mkdir and chown is OK because we are going from more
 * permissive (root.root) to less permissive (root.tracing).
 */
static int set_permissions(char *rundir)
{
	int ret;
	gid_t gid;

	ret = allowed_group();
	if (ret < 0) {
		WARN("No tracing group detected");
		ret = 0;
		goto end;
	}

	gid = ret;

	/* Set lttng run dir */
	ret = chown(rundir, 0, gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", rundir);
		PERROR("chown");
	}

	/* Ensure tracing group can search the run dir */
	ret = chmod(rundir, S_IRWXU | S_IXGRP | S_IXOTH);
	if (ret < 0) {
		ERR("Unable to set permissions on %s", rundir);
		PERROR("chmod");
	}

	/* lttng client socket path */
	ret = chown(client_unix_sock_path, 0, gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", client_unix_sock_path);
		PERROR("chown");
	}

	/* kconsumer error socket path */
	ret = chown(kconsumer_data.err_unix_sock_path, 0, gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", kconsumer_data.err_unix_sock_path);
		PERROR("chown");
	}

	/* 64-bit ustconsumer error socket path */
	ret = chown(ustconsumer64_data.err_unix_sock_path, 0, gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", ustconsumer64_data.err_unix_sock_path);
		PERROR("chown");
	}

	/* 32-bit ustconsumer compat32 error socket path */
	ret = chown(ustconsumer32_data.err_unix_sock_path, 0, gid);
	if (ret < 0) {
		ERR("Unable to set group on %s", ustconsumer32_data.err_unix_sock_path);
		PERROR("chown");
	}

	DBG("All permissions are set");

end:
	return ret;
}

/*
 * Create the lttng run directory needed for all global sockets and pipe.
 */
static int create_lttng_rundir(const char *rundir)
{
	int ret;

	DBG3("Creating LTTng run directory: %s", rundir);

	ret = mkdir(rundir, S_IRWXU);
	if (ret < 0) {
		if (errno != EEXIST) {
			ERR("Unable to create %s", rundir);
			goto error;
		} else {
			ret = 0;
		}
	}

error:
	return ret;
}

/*
 * Setup sockets and directory needed by the kconsumerd communication with the
 * session daemon.
 */
static int set_consumer_sockets(struct consumer_data *consumer_data,
		const char *rundir)
{
	int ret;
	char path[PATH_MAX];

    switch (consumer_data->type) {
	case LTTNG_CONSUMER_KERNEL:
		snprintf(path, PATH_MAX, DEFAULT_KCONSUMERD_PATH, rundir);
		break;
	case LTTNG_CONSUMER64_UST:
		snprintf(path, PATH_MAX, DEFAULT_USTCONSUMERD64_PATH, rundir);
		break;
	case LTTNG_CONSUMER32_UST:
		snprintf(path, PATH_MAX, DEFAULT_USTCONSUMERD32_PATH, rundir);
		break;
	default:
		ERR("Consumer type unknown");
		ret = -EINVAL;
		goto error;
	}

	DBG2("Creating consumer directory: %s", path);

	ret = mkdir(path, S_IRWXU);
	if (ret < 0) {
		if (errno != EEXIST) {
			PERROR("mkdir");
			ERR("Failed to create %s", path);
			goto error;
		}
		ret = -1;
	}

	/* Create the kconsumerd error unix socket */
	consumer_data->err_sock =
		lttcomm_create_unix_sock(consumer_data->err_unix_sock_path);
	if (consumer_data->err_sock < 0) {
		ERR("Create unix sock failed: %s", consumer_data->err_unix_sock_path);
		ret = -1;
		goto error;
	}

	/* File permission MUST be 660 */
	ret = chmod(consumer_data->err_unix_sock_path,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	if (ret < 0) {
		ERR("Set file permissions failed: %s", consumer_data->err_unix_sock_path);
		PERROR("chmod");
		goto error;
	}

error:
	return ret;
}

/*
 * Signal handler for the daemon
 *
 * Simply stop all worker threads, leaving main() return gracefully after
 * joining all threads and calling cleanup().
 */
static void sighandler(int sig)
{
	switch (sig) {
	case SIGPIPE:
		DBG("SIGPIPE caught");
		return;
	case SIGINT:
		DBG("SIGINT caught");
		stop_threads();
		break;
	case SIGTERM:
		DBG("SIGTERM caught");
		stop_threads();
		break;
	default:
		break;
	}
}

/*
 * Setup signal handler for :
 *		SIGINT, SIGTERM, SIGPIPE
 */
static int set_signal_handler(void)
{
	int ret = 0;
	struct sigaction sa;
	sigset_t sigset;

	if ((ret = sigemptyset(&sigset)) < 0) {
		PERROR("sigemptyset");
		return ret;
	}

	sa.sa_handler = sighandler;
	sa.sa_mask = sigset;
	sa.sa_flags = 0;
	if ((ret = sigaction(SIGTERM, &sa, NULL)) < 0) {
		PERROR("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGINT, &sa, NULL)) < 0) {
		PERROR("sigaction");
		return ret;
	}

	if ((ret = sigaction(SIGPIPE, &sa, NULL)) < 0) {
		PERROR("sigaction");
		return ret;
	}

	DBG("Signal handler set for SIGTERM, SIGPIPE and SIGINT");

	return ret;
}

/*
 * Set open files limit to unlimited. This daemon can open a large number of
 * file descriptors in order to consumer multiple kernel traces.
 */
static void set_ulimit(void)
{
	int ret;
	struct rlimit lim;

	/* The kernel does not allowed an infinite limit for open files */
	lim.rlim_cur = 65535;
	lim.rlim_max = 65535;

	ret = setrlimit(RLIMIT_NOFILE, &lim);
	if (ret < 0) {
		PERROR("failed to set open files limit");
	}
}

/*
 * main
 */
int main(int argc, char **argv)
{
	int ret = 0;
	void *status;
	const char *home_path;

	init_kernel_workarounds();

	rcu_register_thread();

	setup_consumerd_path();

	/* Parse arguments */
	progname = argv[0];
	if ((ret = parse_args(argc, argv) < 0)) {
		goto error;
	}

	/* Daemonize */
	if (opt_daemon) {
		int i;

		/*
		 * fork
		 * child: setsid, close FD 0, 1, 2, chdir /
		 * parent: exit (if fork is successful)
		 */
		ret = daemon(0, 0);
		if (ret < 0) {
			PERROR("daemon");
			goto error;
		}
		/*
		 * We are in the child. Make sure all other file
		 * descriptors are closed, in case we are called with
		 * more opened file descriptors than the standard ones.
		 */
		for (i = 3; i < sysconf(_SC_OPEN_MAX); i++) {
			(void) close(i);
		}
	}

	/* Create thread quit pipe */
	if ((ret = init_thread_quit_pipe()) < 0) {
		goto error;
	}

	/* Check if daemon is UID = 0 */
	is_root = !getuid();

	if (is_root) {
		rundir = strdup(DEFAULT_LTTNG_RUNDIR);

		/* Create global run dir with root access */
		ret = create_lttng_rundir(rundir);
		if (ret < 0) {
			goto error;
		}

		if (strlen(apps_unix_sock_path) == 0) {
			snprintf(apps_unix_sock_path, PATH_MAX,
					DEFAULT_GLOBAL_APPS_UNIX_SOCK);
		}

		if (strlen(client_unix_sock_path) == 0) {
			snprintf(client_unix_sock_path, PATH_MAX,
					DEFAULT_GLOBAL_CLIENT_UNIX_SOCK);
		}

		/* Set global SHM for ust */
		if (strlen(wait_shm_path) == 0) {
			snprintf(wait_shm_path, PATH_MAX,
					DEFAULT_GLOBAL_APPS_WAIT_SHM_PATH);
		}

		if (strlen(health_unix_sock_path) == 0) {
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
					DEFAULT_GLOBAL_HEALTH_UNIX_SOCK);
		}

		/* Setup kernel consumerd path */
		snprintf(kconsumer_data.err_unix_sock_path, PATH_MAX,
				DEFAULT_KCONSUMERD_ERR_SOCK_PATH, rundir);
		snprintf(kconsumer_data.cmd_unix_sock_path, PATH_MAX,
				DEFAULT_KCONSUMERD_CMD_SOCK_PATH, rundir);

		DBG2("Kernel consumer err path: %s",
				kconsumer_data.err_unix_sock_path);
		DBG2("Kernel consumer cmd path: %s",
				kconsumer_data.cmd_unix_sock_path);
	} else {
		home_path = get_home_dir();
		if (home_path == NULL) {
			/* TODO: Add --socket PATH option */
			ERR("Can't get HOME directory for sockets creation.");
			ret = -EPERM;
			goto error;
		}

		/*
		 * Create rundir from home path. This will create something like
		 * $HOME/.lttng
		 */
		ret = asprintf(&rundir, DEFAULT_LTTNG_HOME_RUNDIR, home_path);
		if (ret < 0) {
			ret = -ENOMEM;
			goto error;
		}

		ret = create_lttng_rundir(rundir);
		if (ret < 0) {
			goto error;
		}

		if (strlen(apps_unix_sock_path) == 0) {
			snprintf(apps_unix_sock_path, PATH_MAX,
					DEFAULT_HOME_APPS_UNIX_SOCK, home_path);
		}

		/* Set the cli tool unix socket path */
		if (strlen(client_unix_sock_path) == 0) {
			snprintf(client_unix_sock_path, PATH_MAX,
					DEFAULT_HOME_CLIENT_UNIX_SOCK, home_path);
		}

		/* Set global SHM for ust */
		if (strlen(wait_shm_path) == 0) {
			snprintf(wait_shm_path, PATH_MAX,
					DEFAULT_HOME_APPS_WAIT_SHM_PATH, geteuid());
		}

		/* Set health check Unix path */
		if (strlen(health_unix_sock_path) == 0) {
			snprintf(health_unix_sock_path, sizeof(health_unix_sock_path),
					DEFAULT_HOME_HEALTH_UNIX_SOCK, home_path);
		}
	}

	/* Set consumer initial state */
	kernel_consumerd_state = CONSUMER_STOPPED;
	ust_consumerd_state = CONSUMER_STOPPED;

	DBG("Client socket path %s", client_unix_sock_path);
	DBG("Application socket path %s", apps_unix_sock_path);
	DBG("LTTng run directory path: %s", rundir);

	/* 32 bits consumerd path setup */
	snprintf(ustconsumer32_data.err_unix_sock_path, PATH_MAX,
			DEFAULT_USTCONSUMERD32_ERR_SOCK_PATH, rundir);
	snprintf(ustconsumer32_data.cmd_unix_sock_path, PATH_MAX,
			DEFAULT_USTCONSUMERD32_CMD_SOCK_PATH, rundir);

	DBG2("UST consumer 32 bits err path: %s",
			ustconsumer32_data.err_unix_sock_path);
	DBG2("UST consumer 32 bits cmd path: %s",
			ustconsumer32_data.cmd_unix_sock_path);

	/* 64 bits consumerd path setup */
	snprintf(ustconsumer64_data.err_unix_sock_path, PATH_MAX,
			DEFAULT_USTCONSUMERD64_ERR_SOCK_PATH, rundir);
	snprintf(ustconsumer64_data.cmd_unix_sock_path, PATH_MAX,
			DEFAULT_USTCONSUMERD64_CMD_SOCK_PATH, rundir);

	DBG2("UST consumer 64 bits err path: %s",
			ustconsumer64_data.err_unix_sock_path);
	DBG2("UST consumer 64 bits cmd path: %s",
			ustconsumer64_data.cmd_unix_sock_path);

	/*
	 * See if daemon already exist.
	 */
	if ((ret = check_existing_daemon()) < 0) {
		ERR("Already running daemon.\n");
		/*
		 * We do not goto exit because we must not cleanup()
		 * because a daemon is already running.
		 */
		goto error;
	}

	/*
	 * Init UST app hash table. Alloc hash table before this point since
	 * cleanup() can get called after that point.
	 */
	ust_app_ht_alloc();

	/* After this point, we can safely call cleanup() with "goto exit" */

	/*
	 * These actions must be executed as root. We do that *after* setting up
	 * the sockets path because we MUST make the check for another daemon using
	 * those paths *before* trying to set the kernel consumer sockets and init
	 * kernel tracer.
	 */
	if (is_root) {
		ret = set_consumer_sockets(&kconsumer_data, rundir);
		if (ret < 0) {
			goto exit;
		}

		/* Setup kernel tracer */
		if (!opt_no_kernel) {
			init_kernel_tracer();
		}

		/* Set ulimit for open files */
		set_ulimit();
	}
	/* init lttng_fd tracking must be done after set_ulimit. */
	lttng_fd_init();

	ret = set_consumer_sockets(&ustconsumer64_data, rundir);
	if (ret < 0) {
		goto exit;
	}

	ret = set_consumer_sockets(&ustconsumer32_data, rundir);
	if (ret < 0) {
		goto exit;
	}

	if ((ret = set_signal_handler()) < 0) {
		goto exit;
	}

	/* Setup the needed unix socket */
	if ((ret = init_daemon_socket()) < 0) {
		goto exit;
	}

	/* Set credentials to socket */
	if (is_root && ((ret = set_permissions(rundir)) < 0)) {
		goto exit;
	}

	/* Get parent pid if -S, --sig-parent is specified. */
	if (opt_sig_parent) {
		ppid = getppid();
	}

	/* Setup the kernel pipe for waking up the kernel thread */
	if ((ret = utils_create_pipe_cloexec(kernel_poll_pipe)) < 0) {
		goto exit;
	}

	/* Setup the thread apps communication pipe. */
	if ((ret = utils_create_pipe_cloexec(apps_cmd_pipe)) < 0) {
		goto exit;
	}

	/* Init UST command queue. */
	cds_wfq_init(&ust_cmd_queue.queue);

	/*
	 * Get session list pointer. This pointer MUST NOT be free(). This list is
	 * statically declared in session.c
	 */
	session_list_ptr = session_get_list();

	/* Set up max poll set size */
	lttng_poll_set_max_size();

	cmd_init();

	/* Init all health thread counters. */
	health_init(&health_thread_cmd);
	health_init(&health_thread_kernel);
	health_init(&health_thread_app_manage);
	health_init(&health_thread_app_reg);

	/*
	 * Init health counters of the consumer thread. We do a quick hack here to
	 * the state of the consumer health is fine even if the thread is not
	 * started.  This is simply to ease our life and has no cost what so ever.
	 */
	health_init(&kconsumer_data.health);
	health_poll_update(&kconsumer_data.health);
	health_init(&ustconsumer32_data.health);
	health_poll_update(&ustconsumer32_data.health);
	health_init(&ustconsumer64_data.health);
	health_poll_update(&ustconsumer64_data.health);

	/* Create thread to manage the client socket */
	ret = pthread_create(&health_thread, NULL,
			thread_manage_health, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create health");
		goto exit_health;
	}

	/* Create thread to manage the client socket */
	ret = pthread_create(&client_thread, NULL,
			thread_manage_clients, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create clients");
		goto exit_client;
	}

	/* Create thread to dispatch registration */
	ret = pthread_create(&dispatch_thread, NULL,
			thread_dispatch_ust_registration, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create dispatch");
		goto exit_dispatch;
	}

	/* Create thread to manage application registration. */
	ret = pthread_create(&reg_apps_thread, NULL,
			thread_registration_apps, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create registration");
		goto exit_reg_apps;
	}

	/* Create thread to manage application socket */
	ret = pthread_create(&apps_thread, NULL,
			thread_manage_apps, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create apps");
		goto exit_apps;
	}

	/* Create kernel thread to manage kernel event */
	ret = pthread_create(&kernel_thread, NULL,
			thread_manage_kernel, (void *) NULL);
	if (ret != 0) {
		PERROR("pthread_create kernel");
		goto exit_kernel;
	}

	ret = pthread_join(kernel_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join");
		goto error;	/* join error, exit without cleanup */
	}

exit_kernel:
	ret = pthread_join(apps_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join");
		goto error;	/* join error, exit without cleanup */
	}

exit_apps:
	ret = pthread_join(reg_apps_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join");
		goto error;	/* join error, exit without cleanup */
	}

exit_reg_apps:
	ret = pthread_join(dispatch_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join");
		goto error;	/* join error, exit without cleanup */
	}

exit_dispatch:
	ret = pthread_join(client_thread, &status);
	if (ret != 0) {
		PERROR("pthread_join");
		goto error;	/* join error, exit without cleanup */
	}

	ret = join_consumer_thread(&kconsumer_data);
	if (ret != 0) {
		PERROR("join_consumer");
		goto error;	/* join error, exit without cleanup */
	}

exit_client:
exit_health:
exit:
	/*
	 * cleanup() is called when no other thread is running.
	 */
	rcu_thread_online();
	cleanup();
	rcu_thread_offline();
	rcu_unregister_thread();
	if (!ret) {
		exit(EXIT_SUCCESS);
	}
error:
	exit(EXIT_FAILURE);
}