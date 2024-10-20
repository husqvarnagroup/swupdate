/*
 * (C) Copyright 2023
 * Stefano Babic <stefano.babic@swupdate.org>
 *
 * SPDX-License-Identifier:     GPL-2.0-only
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <handler.h>
#include <util.h>
#include <json-c/json.h>
#include "parselib.h"
#include "channel.h"
#include "channel_curl.h"
#include "docker.h"
#include "docker_interface.h"
#include "swupdate_dict.h"

typedef struct {
	const char *url;
	channel_method_t method;
} docker_api_t;

docker_api_t docker_api[] = {
	[DOCKER_IMAGE_LOAD] = {"/images/load", CHANNEL_POST},
	[DOCKER_IMAGE_DELETE] = {"/images/%s", CHANNEL_DELETE},
	[DOCKER_IMAGE_PRUNE] = {"/images/prune", CHANNEL_POST},
	[DOCKER_CONTAINER_CREATE] = {"/containers/create", CHANNEL_POST},
	[DOCKER_CONTAINER_DELETE] = {"/containers/%s", CHANNEL_DELETE},
	[DOCKER_CONTAINER_START] = {"/containers/%s/start", CHANNEL_POST},
	[DOCKER_CONTAINER_STOP] = {"/containers/%s/stop", CHANNEL_POST},
};

static channel_data_t channel_data_defaults = {.debug = true,
					       .unix_socket =(char *) DOCKER_DEFAULT_SOCKET,
					       .retries = 1,
					       .retry_sleep =
						   CHANNEL_DEFAULT_RESUME_DELAY,
					       .format = CHANNEL_PARSE_JSON,
					       .nocheckanswer = false,
					       .nofollow = false,
					       .noipc = true,
					       .range = NULL,
					       .connection_timeout = 0,
					       .headers = NULL,
					       .headers_to_send = NULL,
					       .received_headers = NULL
						};


static const char *docker_base_url(void)
{
	return DOCKER_SOCKET_URL;
}

static void docker_prepare_url(docker_services_t service, char *buf, size_t size)
{
	snprintf(buf, size, "%s%s", docker_base_url(), docker_api[service].url);
}

static channel_t *docker_prepare_channel(channel_data_t *channel_data)
{
	channel_t *channel = channel_new();
	if (!channel) {
		ERROR("New channel cannot be requested");
		return NULL;
	}

	if (channel->open(channel, channel_data) != CHANNEL_OK) {
		channel->close(channel);
		free(channel);
		return NULL;
	}

	return channel;
}

static server_op_res_t evaluate_docker_answer(json_object *json_reply)
{
	if (!json_reply) {
		ERROR("No JSON answer from Docker Daemon");
		return SERVER_EBADMSG;
	}

	/*
	 * Check for errors
	 */
	json_object *json_error = json_get_path_key(json_reply,
			      (const char *[]){"error", NULL});
	if (json_object_get_type(json_error) == json_type_string) {
		ERROR("Image not loaded, daemon reports: %s",
		      json_object_get_string(json_error));
		return SERVER_EBADMSG;
	}

	json_object *json_stream = json_get_path_key(json_reply,
			      (const char *[]){"stream", NULL});
	if (json_object_get_type(json_stream) == json_type_string) {
		INFO("%s", json_object_get_string(json_stream));
		return SERVER_OK;
	}
	return SERVER_EBADMSG;
}

server_op_res_t docker_image_load(int fd, size_t len)
{
	channel_t *channel;
	channel_op_res_t ch_response;
	server_op_res_t result = SERVER_OK;
	char dockerurl[1024];

	channel_data_t channel_data = channel_data_defaults;
	struct dict httpheaders_to_send;

	LIST_INIT(&httpheaders_to_send);
	if (dict_insert_value(&httpheaders_to_send, "Expect", "")) {
		ERROR("Error initializing HTTP Headers");
		return SERVER_EINIT;
	}

	docker_prepare_url(DOCKER_IMAGE_LOAD, dockerurl, sizeof(dockerurl));
	channel_data.url = dockerurl;

	channel_data.read_fifo = fd;
	channel_data.method = docker_api[DOCKER_IMAGE_LOAD].method;
	channel_data.upload_filesize = len;
	channel_data.headers_to_send = &httpheaders_to_send;
	channel_data.content_type = "application/x-tar";
	channel_data.accept_content_type = "application/json";

	channel = docker_prepare_channel(&channel_data);
	if (!channel) {
		return SERVER_EERR;
	}
	ch_response = channel->put_file(channel, &channel_data);

	if ((result = map_channel_retcode(ch_response)) !=
	    SERVER_OK) {
		channel->close(channel);
		free(channel);
		return SERVER_EERR;
	}

	dict_drop_db(&httpheaders_to_send);

	channel->close(channel);
	free(channel);

	return evaluate_docker_answer(channel_data.json_reply);
}

static server_op_res_t docker_send_request(docker_services_t service, char *url, char *setup)
{
	channel_t *channel;
	channel_op_res_t ch_response;
	server_op_res_t result = SERVER_OK;
	channel_data_t channel_data = channel_data_defaults;

	channel_data.url = url;
	channel_data.method = docker_api[service].method;

	channel = docker_prepare_channel(&channel_data);
	if (!channel) {
		return SERVER_EERR;
	}

	if (setup)
		channel_data.request_body = setup;

	ch_response = channel->put(channel, &channel_data);

	if ((result = map_channel_retcode(ch_response)) !=
	    SERVER_OK) {
		channel->close(channel);
		free(channel);
		return SERVER_EERR;
	}

	channel->close(channel);
	free(channel);

	return result;
}

static server_op_res_t docker_simple_post(docker_services_t service, const char *name)
{
	char url[256];

	docker_prepare_url(service, url, sizeof(url));
	if (name) {
		char *tmp=strdup(url);
		snprintf(url, sizeof(url), tmp, name);
		free(tmp);
	}

	return docker_send_request(service, url, NULL);
}

server_op_res_t docker_container_create(const char *name, char *setup)
{
	char url[256];
	
	if (name) {
		snprintf(url, sizeof(url), "%s%s?name=%s",
			 docker_base_url(), docker_api[DOCKER_CONTAINER_CREATE].url, name);
	} else
		docker_prepare_url(DOCKER_CONTAINER_CREATE, url, sizeof(url));

	return docker_send_request(DOCKER_CONTAINER_CREATE, url, setup);
}

server_op_res_t docker_container_remove(const char *name)
{
	return docker_simple_post(DOCKER_CONTAINER_DELETE, name);
}

server_op_res_t docker_container_start(const char *name)
{
	return docker_simple_post(DOCKER_CONTAINER_START, name);
}

server_op_res_t docker_container_stop(const char *name)
{
	return docker_simple_post(DOCKER_CONTAINER_STOP, name);
}

server_op_res_t docker_image_remove(const char *name)
{
	return docker_simple_post(DOCKER_IMAGE_DELETE, name);
}

server_op_res_t docker_image_prune(const char *name)
{
	return docker_simple_post(DOCKER_IMAGE_PRUNE, name);
}
