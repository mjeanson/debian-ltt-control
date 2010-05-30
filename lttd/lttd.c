/*
 * lttd
 *
 * Linux Trace Toolkit Daemon
 *
 * This is a simple daemon that reads a few LTTng debugfs channels and saves
 * them in a trace on the virtual file system.
 *
 * CPU hot-plugging is supported using inotify.
 *
 * Copyright 2009-2010 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * Copyright 2010 - Michael Sills-Lavoie <michael.sills-lavoie@polymtl.ca>
 * Copyright 2010 - Oumarou Dicko <oumarou.dicko@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _REENTRANT
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

#include <liblttd/liblttd.h>
#include <liblttd/liblttdvfs.h>

struct liblttd_instance* instance;

static char		*trace_name = NULL;
static char		*channel_name = NULL;
static int		daemon_mode = 0;
static int		append_mode = 0;
static unsigned long	num_threads = 1;
static int		dump_flight_only = 0;
static int		dump_normal_only = 0;
static int		verbose_mode = 0;


/* Args :
 *
 * -t directory		Directory name of the trace to write to. Will be created.
 * -c directory		Root directory of the debugfs trace channels.
 * -d          		Run in background (daemon).
 * -a			Trace append mode.
 * -s			Send SIGUSR1 to parent when ready for IO.
 */
void show_arguments(void)
{
	printf("Please use the following arguments :\n");
	printf("\n");
	printf("-t directory  Directory name of the trace to write to.\n"
				 "              It will be created.\n");
	printf("-c directory  Root directory of the debugfs trace channels.\n");
	printf("-d            Run in background (daemon).\n");
	printf("-a            Append to an possibly existing trace.\n");
	printf("-N            Number of threads to start.\n");
	printf("-f            Dump only flight recorder channels.\n");
	printf("-n            Dump only normal channels.\n");
	printf("-v            Verbose mode.\n");
	printf("\n");
}


/* parse_arguments
 *
 * Parses the command line arguments.
 *
 * Returns 1 if the arguments were correct, but doesn't ask for program
 * continuation. Returns -1 if the arguments are incorrect, or 0 if OK.
 */
int parse_arguments(int argc, char **argv)
{
	int ret = 0;
	int argn = 1;

	if(argc == 2) {
		if(strcmp(argv[1], "-h") == 0) {
			return 1;
		}
	}

	while(argn < argc) {

		switch(argv[argn][0]) {
			case '-':
				switch(argv[argn][1]) {
					case 't':
						if(argn+1 < argc) {
							trace_name = argv[argn+1];
							argn++;
						}
						break;
					case 'c':
						if(argn+1 < argc) {
							channel_name = argv[argn+1];
							argn++;
						}
						break;
					case 'd':
						daemon_mode = 1;
						break;
					case 'a':
						append_mode = 1;
						break;
					case 'N':
						if(argn+1 < argc) {
							num_threads = strtoul(argv[argn+1], NULL, 0);
							argn++;
						}
						break;
					case 'f':
						dump_flight_only = 1;
						break;
					case 'n':
						dump_normal_only = 1;
						break;
					case 'v':
						verbose_mode = 1;
						break;
					default:
						printf("Invalid argument '%s'.\n", argv[argn]);
						printf("\n");
						ret = -1;
				}
				break;
			default:
				printf("Invalid argument '%s'.\n", argv[argn]);
				printf("\n");
				ret = -1;
		}
		argn++;
	}

	if(trace_name == NULL) {
		printf("Please specify a trace name.\n");
		printf("\n");
		ret = -1;
	}

	if(channel_name == NULL) {
		printf("Please specify a channel name.\n");
		printf("\n");
		ret = -1;
	}

	return ret;
}

void show_info(void)
{
	printf("Linux Trace Toolkit Trace Daemon " VERSION "\n");
	printf("\n");
	printf("Reading from debugfs directory : %s\n", channel_name);
	printf("Writing to trace directory : %s\n", trace_name);
	printf("\n");
}


/* signal handling */

static void handler(int signo)
{
	printf("Signal %d received : exiting cleanly\n", signo);
	liblttd_stop_instance(instance);
}

int main(int argc, char ** argv)
{
	int ret = 0;
	struct sigaction act;

	ret = parse_arguments(argc, argv);

	if(ret != 0) show_arguments();
	if(ret < 0) return EINVAL;
	if(ret > 0) return 0;

	show_info();

	/* Connect the signal handlers */
	act.sa_handler = handler;
	act.sa_flags = 0;
	sigemptyset(&(act.sa_mask));
	sigaddset(&(act.sa_mask), SIGTERM);
	sigaddset(&(act.sa_mask), SIGQUIT);
	sigaddset(&(act.sa_mask), SIGINT);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGQUIT, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	if(daemon_mode) {
		ret = daemon(0, 0);

		if(ret == -1) {
			perror("An error occured while daemonizing.");
			exit(-1);
		}
	}

	struct liblttd_callbacks* callbacks =
		liblttdvfs_new_callbacks(trace_name, append_mode, verbose_mode);

	instance = liblttd_new_instance(callbacks, channel_name, num_threads,
					dump_flight_only, dump_normal_only,
					verbose_mode);

	if(!instance) {
		perror("An error occured while creating the liblttd instance");
		return ret;
	}

	liblttd_start_instance(instance);

	return ret;
}

