/*
 * Author: Christian Storm
 * Copyright (C) 2016, Siemens AG
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <util.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <getopt.h>
#include <json-c/json.h>
#include "pctl.h"
#include "suricatta/suricatta.h"
#include "suricatta/server.h"
#include "suricatta_private.h"
#include "parselib.h"
#include "swupdate_settings.h"
#include <network_ipc.h>

static bool enable = true;
static bool trigger = false;
static struct option long_options[] = {
    {"enable", no_argument, NULL, 'e'},
    {"disable", no_argument, NULL, 'd'},
    {NULL, 0, NULL, 0}};
static sem_t suricatta_enable_sema;

void suricatta_print_help(void)
{
	fprintf(
	    stdout,
	    "\tsuricatta arguments (mandatory arguments are marked with '*'):\n"
	    "\t  -e, --enable      Daemon enabled at startup (default).\n"
	    "\t  -d, --disable     Daemon disabled at startup.\n"
	    );
	server.help();
}

static server_op_res_t suricatta_enable(ipc_message *msg)
{
	struct json_object *json_root;
	json_object *json_data;

	json_root = server_tokenize_msg(msg->data.procmsg.buf,
					sizeof(msg->data.procmsg.buf));
	if (!json_root) {
		msg->type = NACK;
		ERROR("Wrong JSON message, see documentation");
		return SERVER_EERR;
	}

	json_data = json_get_path_key(
	    json_root, (const char *[]){"enable", NULL});
	if (json_data) {
		enable = json_object_get_boolean(json_data);
		if (sem_post(&suricatta_enable_sema))
			ERROR("sem_post enable failled");
		TRACE ("suricatta mode %sabled", enable ? "en" : "dis");
	}
	else {
	  /*
	   * check if polling of server is requested via IPC (trigger)
	   * This allows to force to check if an update is available
	   * on the server. This is useful in case the device is not always
	   * online, and it just checks for update (and then update should run
	   * immediately) just when online.
	   */
	  json_data = json_get_path_key(
	      json_root, (const char *[]){"trigger", NULL});
	  if (json_data) {
	    trigger = json_object_get_boolean(json_data);
	    if (sem_post(&suricatta_enable_sema))
		    ERROR("sem_post trigger failled");
	    TRACE ("suricatta polling trigger received, checking on server");
	  }

	}

	msg->type = ACK;

	return SERVER_OK;
}

static server_op_res_t suricatta_ipc(int fd)
{
	ipc_message msg;
	server_op_res_t result = SERVER_OK;
	int ret;

	ret = read(fd, &msg, sizeof(msg));
	if (ret != sizeof(msg))
		return SERVER_EERR;
	switch (msg.data.procmsg.cmd) {
	case CMD_ENABLE:
		result = suricatta_enable(&msg);
		break;
	default:
		result = server.ipc(&msg);
		break;
	}

	if (write(fd, &msg, sizeof(msg)) != sizeof(msg)) {
		TRACE("IPC ERROR: sending back msg");
	}

	/* Send ipc back */
	return result;
}

static int suricatta_settings(void *elem, void  __attribute__ ((__unused__)) *data)
{
	get_field(LIBCFG_PARSER, elem, "enable",
		&enable);

	return 0;
}

int suricatta_wait(int seconds)
{
	struct timespec tp;
	int retval;
	int enable_entry = enable;

	clock_gettime(CLOCK_REALTIME, &tp);
	int t_entry = tp.tv_sec;

	tp.tv_sec += seconds;
	DEBUG("Sleeping for %d seconds.", seconds);
	retval = sem_timedwait(&suricatta_enable_sema, &tp);

	if (retval) {
		if (errno != ETIMEDOUT) {
			TRACE("Suricatta awakened because of: %s", strerror(errno));
			return 0;
		}
		/* else: Suricatta awakened because timeout expired */
	} else {
		/* suricatta_enable_sema unlocked */
		time_t t_wake = time(NULL);

		TRACE("Suricatta woke up for IPC at %ld seconds", t_wake - t_entry);
		/*
		 * Note: enable works as trigger, too.
		 * After enable is set, suricatta will try to contact
		 * the server to check for pending action
		 * This is done by resetting the number of seconds to
		 * wait for.
		 */
		if (trigger || (enable && !enable_entry))
			return 0;
		else
			return seconds - (t_wake - t_entry);
	}

	return 0;
}

static void *ipc_thread(void __attribute__ ((__unused__)) *data)
{
	fd_set readfds;
	int retval;

	while (1) {
		FD_ZERO(&readfds);
		FD_SET(sw_sockfd, &readfds);
		retval = select(sw_sockfd + 1, &readfds, NULL, NULL, NULL);

		if (retval < 0) {
			TRACE("Suricatta IPC awakened because of: %s", strerror(errno));
			return 0;
		}

		if (retval && FD_ISSET(sw_sockfd, &readfds)) {
			if (suricatta_ipc(sw_sockfd) != SERVER_OK) {
				DEBUG("Handling IPC failed!");
			}
		}
	}
}

int start_suricatta(const char *cfgfname, int argc, char *argv[])
{
	int action_id;
	sigset_t sigpipe_mask;
	sigset_t saved_mask;
	int choice = 0;
	char **serverargv;

	sigemptyset(&sigpipe_mask);
	sigaddset(&sigpipe_mask, SIGPIPE);
	sigprocmask(SIG_BLOCK, &sigpipe_mask, &saved_mask);

	/*
	 * Temporary copy the command line argument
	 * to pass unchanged to the server instance.
	 * getopt() will change them when called here
	 */
	serverargv = (char **)malloc(argc * sizeof(char *));
	if (!serverargv) {
		ERROR("OOM starting suricatta, exiting !");
		exit(EXIT_FAILURE);
	}
	for (int i = 0; i < argc; i++) {
		serverargv[i] = argv[i];
	}

	/*
	 * First check for common properties that do not depend
	 * from server implementation
	 */
	if (cfgfname) {
		swupdate_cfg_handle handle;
		swupdate_cfg_init(&handle);
		if (swupdate_cfg_read_file(&handle, cfgfname) == 0) {
			read_module_settings(&handle, "suricatta", suricatta_settings, NULL);
		}
		swupdate_cfg_destroy(&handle);
	}
	optind = 1;
	opterr = 0;

	while ((choice = getopt_long(argc, argv, "de",
				     long_options, NULL)) != -1) {
		switch (choice) {
		case 'e':
			enable = true;
			break;
		case 'd':
			enable = false;
			break;
		case '?':
			break;
		}
	}

	if (sem_init(&suricatta_enable_sema, 0, 0)) {
		ERROR("Initialising suricatta enable semaphore failed");
		exit(EXIT_FAILURE);
	}

	/*
	 * Start ipc thread here, because the following server.start might block
	 */
	start_thread(ipc_thread, NULL);

	/*
	 * Now start a specific implementation of the server
	 */
	if (server.start(cfgfname, argc, serverargv) != SERVER_OK) {
		exit(EXIT_FAILURE);
	}
	free(serverargv);

	TRACE("Server initialized, entering suricatta main loop.");
	while (true) {
		if (enable || trigger) {
			trigger = false;
			switch (server.has_pending_action(&action_id)) {
			case SERVER_UPDATE_AVAILABLE:
				DEBUG("About to process available update.");
				server.install_update();
				break;
			case SERVER_ID_REQUESTED:
				server.send_target_data();
				trigger = true;
				break;
			case SERVER_EINIT:
				break;
			case SERVER_OK:
			default:
				DEBUG("No pending action to process.");
				break;
			}
		}

		for (int wait_seconds = server.get_polling_interval();
			 wait_seconds > 0;
			 wait_seconds = min(wait_seconds, (int)server.get_polling_interval())) {
			wait_seconds = suricatta_wait(wait_seconds);
		}

		TRACE("Suricatta awakened.");
	}
}
