/*
 * Copyright (c) 2012-2015 Varnish Software Group
 * All rights reserved.
 *
 * Author: Kristian Lyngstøl <kristian@bohemians.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <microhttpd.h>

#include "common.h"
#include "plugins.h"
#include "ipc.h"
#include "http.h"
#include "vsb.h"

#define RCV_BUFFER	2 * 1000 * 1024
#define HELP_TEXT							\
	"This is the varnish agent.\n\n"				\
	"GET requests never modify state\n"				\
	"POST requests are not idempotent, and can modify state\n"	\
	"PUT requests are idempotent, and can modify state\n"		\
	"HEAD requests can be performed on all resources that support GET\n" \
	"\nThe following URLs are bound:\n\n"

struct http_listener {
	char *url;
	unsigned int method;
	http_cb_f cb;
	void *data;
	struct http_listener *next;
};

struct http_priv_t {
	int logger;
	/*
	 * XXX: Used exclusively before the main thread is spun up
	 * 
	 * see ipc.c on why this is needed (now).
	 */
	int logger2;
	char *help_page;
	struct http_listener *listener;
};

struct connection_info_struct {
	struct vsb *req_body;
	int authed;
};

struct header_finder_t {
	const char *header;
	char *value;
};

struct http_content_type {
	const char *file_ext;
	const char *content_type;
} http_content_types[] = {
	{ ".html",	"text/html" },
	{ ".js",	"text/javascript" },
	{ ".css",	"text/css" },
	{ ".jpg",	"image/jpeg" },
	{ ".jpeg",	"image/jpeg" },
	{ ".png",	"image/png" },
	{ ".gif",	"image/gif" },
	{ NULL,		NULL }
};

static char *
make_help(struct http_priv_t *http)
{
	struct http_listener *lp;
	char buf[512];
	char *data;

	data = strdup(HELP_TEXT);
	assert(data);

	for (lp = http->listener; lp != NULL; lp = lp->next) {
		snprintf(buf, sizeof(buf), " - %-20s %-3s %-3s %-4s %s\n",
		    lp->url,
		    (lp->method & M_GET) ? "GET" : "",
		    (lp->method & M_PUT) ? "PUT" : "",
		    (lp->method & M_POST) ? "POST" : "",
		    (lp->method & M_DELETE) ? "DELETE" : "");
		/* \0 and newline at the end */
		data = realloc(data, strlen(data) + strlen(buf) + 2);
		assert(data);
		strcat(data, buf);
	}
	strcat(data, "\n");
	return (data);
}

static void
send_auth_response(struct MHD_Connection *connection)
{
	struct http_response *resp;

	resp = http_mkresp(connection, 401, "Authorize, please.\n\n"
	    "If Varnish Agent was installed from packages, the "
	    "/etc/varnish/agent_secret file contains generated "
	    "credentials.\n");
	http_add_header(resp, "WWW-Authenticate",
	    "Basic realm=varnish-agent");
	send_response(resp);
	http_free_resp(resp);
}

void
http_add_header(struct http_response *resp, const char *key,
    const char *value)
{
	struct http_header *hdr;

	assert(key);
	assert(value);

	ALLOC_OBJ(hdr);
	hdr->key = strdup(key);
	assert(hdr->key);
	hdr->value = strdup(value);
	assert(hdr->value);
	hdr->next = resp->headers;
	resp->headers = hdr;
}

static int
get_key(void *cls, enum MHD_ValueKind kind, const char *key,
    const char *value)
{
	struct header_finder_t *finder = cls;

	(void)kind;

	if (!strcasecmp(finder->header, key)) {
		finder->value = strdup(value);
		return (MHD_NO);
	}
	return (MHD_YES);
}

char *
http_get_header(struct MHD_Connection *connection, const char *key)
{
	struct header_finder_t finder;

	finder.header = key;
	finder.value = NULL;
	MHD_get_connection_values(connection, MHD_HEADER_KIND, &get_key,
	    &finder);
	return (finder.value);
}

void
http_free_resp(struct http_response *resp)
{
	struct http_header *hdr, *ohdr;

	for (hdr = resp->headers; hdr;) {
		free(hdr->key);
		free(hdr->value);
		ohdr = hdr;
		hdr = hdr->next;
		free(ohdr);
	}
	free(resp);
}

struct http_response *
http_mkresp(struct MHD_Connection *conn, int status, const char *body)
{
	struct http_response *resp;

	ALLOC_OBJ(resp);
	resp->status = status;
	resp->connection = conn;
	resp->data = body;
	if (resp->data)
		resp->ndata = strlen(resp->data);
	return (resp);
}

int
send_response(struct http_response *resp)
{
	struct MHD_Response *response;
	struct http_header *hdr;
	char *origin;
	int ret;

#if (MHD_VERSION >= 0x00090500)
	response = MHD_create_response_from_buffer(resp->ndata,
	    (void *)resp->data, MHD_RESPMEM_MUST_COPY);
#else
	response = MHD_create_response_from_data(resp->ndata,
	    (void *)resp->data, MHD_NO, MHD_YES);
#endif
	assert(response);
	for (hdr = resp->headers; hdr; hdr = hdr->next)
		MHD_add_response_header(response, hdr->key, hdr->value);
	MHD_add_response_header(response, "Access-Control-Allow-Headers",
	    "Authorization, Origin");
	MHD_add_response_header(response, "Access-Control-Allow-Methods",
	    "GET, POST, PUT, DELETE, OPTIONS");
	origin = http_get_header(resp->connection, "Origin");
	MHD_add_response_header(response, "Access-Control-Allow-Origin",
	    origin ? origin : "*");
	free(origin);
	ret = MHD_queue_response(resp->connection, resp->status, response);
	MHD_destroy_response(response);
	return (ret);
}

int
http_reply(struct MHD_Connection *conn, int status, const char *data)
{
	return (http_reply_len(conn, status, data, data ? strlen(data) : 0));
}

int
http_reply_len(struct MHD_Connection *conn, int status, const char *data,
    unsigned data_size)
{
	struct http_response resp = { conn, NULL, status, data, data_size };
	return (send_response(&resp));
}

static void
request_completed(void *cls, struct MHD_Connection *connection,
    void **con_cls, enum MHD_RequestTerminationCode code)
{
	struct connection_info_struct *con_info =
	    (struct connection_info_struct *)*con_cls;

	(void)cls;
	(void)connection;
	(void)code;

	if (con_info) {
		if (con_info->req_body)
			VSB_delete(con_info->req_body);
		free(con_info);
		*con_cls = NULL;
	}
}

static int
find_listener(struct http_request *request, struct http_priv_t *http)
{
	struct http_listener *lp;
	const char *arg;

	assert(request);
	for (lp = http->listener; lp != NULL; lp = lp->next) {
		if (STARTS_WITH(request->url, lp->url) &&
				(lp->method & request->method)) {
			arg = request->url + strlen(lp->url);
			if (arg[0] == '\0')
				arg = NULL;
			else if (arg[0] != '/')
				continue;
			else
				while (*arg == '/')
					arg++;
			if (arg && *arg == '\0')
				arg = NULL;
			lp->cb(request, arg, lp->data);
			return (1);
		}
	}
	return (0);
}

static void
log_request(struct MHD_Connection *connection,
    const struct http_priv_t *http, const char *method, const char *url)
{
#if MHD_VERSION < 0x00090600
	const union MHD_ConnectionInfo *info;
	const unsigned char *ip;

	info = MHD_get_connection_info(connection,
	    MHD_CONNECTION_INFO_CLIENT_ADDRESS);
	assert(info);

	ip = (unsigned char *)&info->client_addr->sin_addr.s_addr;
	logger(http->logger, "%hhu.%hhu.%hhu.%hhu:%d - %s %s",
	    ip[0], ip[1], ip[2], ip[3], info->client_addr->sin_port,
	    method, url);
#else
	(void)connection;

	logger(http->logger, "%s %s", method, url);
#endif
}

static int
check_auth(struct MHD_Connection *connection, struct agent_core_t *core,
    struct connection_info_struct *con_info)
{
	char *auth, *token;

	AN(con_info);

	if (con_info->authed == 0) {
		auth = http_get_header(connection, "Authorization");

		if (!auth || strncmp(auth, "Basic ", strlen("Basic "))) {
			free(auth);
			return (1);
		}

		token = auth + sizeof "Basic";

		if (!strcmp(VSB_data(core->config->auth_token), token))
			con_info->authed = 1;

		free(auth);
	}

	return (!con_info->authed);
}

static enum http_method
parse_method(const char *method)
{

	AN(method);

#define CMP_METHOD(name) \
	if (!strcmp(method, #name)) \
		return (M_##name);
	CMP_METHOD(GET);
	CMP_METHOD(POST);
	CMP_METHOD(PUT);
	CMP_METHOD(DELETE);
	CMP_METHOD(OPTIONS);
#undef CMP_METHOD

	if (!strcmp(method, "HEAD"))
		return (M_GET);

	return (M_UNKNOWN);
}

static int
answer_to_connection(void *cls, struct MHD_Connection *connection,
    const char *url, const char *method,
    const char *version, const char *upload_data,
    size_t * upload_data_size, void **con_cls)
{
	struct agent_core_t *core = (struct agent_core_t *)cls;
	struct http_priv_t *http;
	struct http_request request;
	struct connection_info_struct *con_info;

	(void)version;

	GET_PRIV(core, http);

	request.method = parse_method(method);
	log_request(connection, http, method, url);

	if (*con_cls == NULL) {
		ALLOC_OBJ(con_info);
		if (!check_auth(connection, core, con_info)) {
			con_info->req_body = VSB_new_auto();
			AN(con_info->req_body);
		}
		*con_cls = con_info;
		return (MHD_YES);
	}
	con_info = *con_cls;
	AN(core->config->auth_token);

	if (core->config->r_arg && request.method != M_GET &&
	    request.method != M_OPTIONS) {
		logger(http->logger,
		    "Read-only mode and not a GET, HEAD or OPTIONS request");
		return (http_reply(connection, 405, "Read-only mode"));
	}

	if (*upload_data_size != 0) {
		if (con_info->req_body)
			AZ(VSB_bcat(con_info->req_body, upload_data,
			    *upload_data_size));
		*upload_data_size = 0;
		return (MHD_YES);
	}

	request.connection = connection;
	request.url = url;

	if (con_info->req_body) {
		AZ(VSB_putc(con_info->req_body, '\0'));
		AZ(VSB_finish(con_info->req_body));
		request.body = VSB_data(con_info->req_body);
		request.bodylen = VSB_len(con_info->req_body) - 1;
	} else {
		request.body = NULL;
		request.bodylen = 0;
	}

	/* We need this for preflight requests (CORS). */
	if (request.method == M_OPTIONS)
		return (http_reply(connection, 200, NULL));

	if (check_auth(connection, core, con_info)) {
		send_auth_response(connection);
		return (MHD_YES);
	}

	if (find_listener(&request, http))
		return (MHD_YES);

	if (request.method == M_GET && !strcmp(url, "/")) {
		if (http->help_page == NULL)
			http->help_page = make_help(http);
		assert(http->help_page);
		return (http_reply(connection, 200, http->help_page));
	}

	return (http_reply(connection, 500, "Failed"));
}

static void *
http_run(void *data)
{
	struct agent_core_t *core = (struct agent_core_t *)data;
	struct http_priv_t *http;
	struct MHD_Daemon *d;
	int port;
	bool is_ipv6 = false;

	struct sockaddr_in6 v6;
	struct sockaddr_in v4;

	port = atoi(core->config->local_port);
	const char* addr = core->config->bind_address;

	assert(port > 0);

	memset(&v4, 0, sizeof(struct sockaddr_in));
	memset(&v6, 0, sizeof(struct sockaddr_in6));

	v4.sin_family = AF_INET;
	v4.sin_port = htons(port);

	v6.sin6_family = AF_INET6;
	v6.sin6_port = htons(port);

	GET_PRIV(core, http);

	int addr_ok = inet_pton(AF_INET, addr, &v4.sin_addr);

	if (!addr_ok) {
		addr_ok = inet_pton(AF_INET6, addr, &v6.sin6_addr);
		is_ipv6 = true;
	}

	assert(addr_ok >= 0);

	if (addr_ok <= 0) {
		warnlog(http->logger2,
		    "Could not extract network address out of %s, Inet returned %d.",
		    addr, addr_ok);
		exit(1);
	}

	logger(http->logger2, "HTTP starting on %s:%i", addr, port);

	// passing an invalid port nr just for spite, mhd should ignore
	// the port arg and just use agent_daemon_addr..
	if (is_ipv6) {
		warnlog(http->logger2, "running ipv6");
		d = MHD_start_daemon(
		    MHD_USE_SELECT_INTERNALLY | MHD_USE_DUAL_STACK, 0, NULL, NULL,
		    &answer_to_connection, data, MHD_OPTION_SOCK_ADDR, &v6,
		    MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
		    MHD_OPTION_END);
	} else {
		d = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, 0, NULL, NULL,
		    &answer_to_connection, data, MHD_OPTION_SOCK_ADDR,
		    &v4, MHD_OPTION_NOTIFY_COMPLETED,
		    request_completed, NULL, MHD_OPTION_END);
	}

	if (!d) {
		warnlog(http->logger2, "HTTP failed to start on %s:%i. "
		    "Agent already running?", addr, port);
		sleep(1);
		exit(1);
	}

	/*
	 * XXX: .....
	 */
	for (;;)
		sleep(100);
	MHD_stop_daemon(d);
	return (NULL);
}

void
http_register_path(struct agent_core_t *core, const char *url,
    unsigned int method, http_cb_f cb, void *data)
{
	struct http_listener *lp;
	struct http_priv_t *http;

	assert(cb);

	ALLOC_OBJ(lp);
	GET_PRIV(core, http);
	lp->url = strdup(url);
	assert(lp->url);
	lp->method = method;
	lp->cb = cb;
	lp->data = data;
	lp->next = http->listener;
	http->listener = lp;
}

void
http_set_content_type(struct http_response *resp, const char *path)
{
	struct http_content_type *cp;
	char *ext;

	ext = strrchr(path, '.');
	if (ext) {
		for (cp = http_content_types; cp->file_ext; cp++) {
			if (!strcmp(ext, cp->file_ext)) {
				http_add_header(resp, "Content-Type",
				    cp->content_type);
				break;
			}
		}
	}
}

static void *
http_start(struct agent_core_t *core, const char *name)
{
	pthread_t *thread;

	(void)name;

	ALLOC_OBJ(thread);
	AZ(pthread_create(thread, NULL, (*http_run), core));
	return (thread);
}

void
http_init(struct agent_core_t *core)
{
	struct agent_plugin_t *plug;
	struct http_priv_t *priv;

	ALLOC_OBJ(priv);
	plug = plugin_find(core, "http");
	priv->logger = ipc_register(core, "logger");
	priv->logger2 = ipc_register(core, "logger");
	plug->data = (void *)priv;
	plug->start = http_start;
}
