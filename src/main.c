#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
#include <ev.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#define die(...) err(1, __VA_ARGS__)
#define log(fmt, ...)                                                          \
	fprintf(                                                                   \
		stderr, "%s:%d: " fmt "\n", __FILE__,                                  \
		__LINE__ __VA_OPT__(, ) __VA_ARGS__                                    \
	)
#define BRK __asm__("int3")

#define BOLD(str) "[1m" str "[m"

#ifndef NDEBUG
#define DEBUG(x) x
#else
#define DEBUG(x)
#endif

#define RESPONSE_ERR_400 "HTTP/1.1 400 BAD REQUEST"
#define RESPONSE_ERR_404 "HTTP/1.1 404 PAGE NOT FOUND"
#define RESPONSE_ERR_405 "HTTP/1.1 405 METHOD NOT ALLOWED"
#define RESPONSE_ERR_408 "HTTP/1.1 408 CONNECTION TIMEOUT"
#define RESPONSE_ERR_505 "HTTP/1.1 505 HTTP VERSION UNSUPPORTED"
#define RESPONSE_OK "HTTP/1.1 200 OK DATA IN FLIGHT"

#define CONTENT_HTML "Content-Type: text/html; charset=utf-8"
#define KEEP_ALIVE "Connection: close"

const uint8_t CONGRATS[] = {
#embed "../res/congrats.webp"
};

const uint8_t FAVICON[] = {
#embed "../res/favicon.ico"
};

const uint8_t HOMEPAGE[] = {
#embed "index.html"
};

const uint8_t FINISHER[] = {
#embed "finished.html"
};

////////////////////

// Form replies will be dumped here
#define FORM_DATA_PATH "out.html"
FILE* form_data_file = NULL;

int server_sock;

////////////////////////////////////////////////////////////////////////////////

#define METHODS                                                                \
	X(GET)                                                                     \
	X(POST)

typedef enum {
	METH_INVALID,
#define X(x) METH_##x,
	METHODS
#undef X
} Method;

const char* methodShow(Method meth) {
	switch(meth) {
#define X(x)                                                                   \
	case METH_##x:                                                             \
		return #x;

		METHODS
	default:
		return "INVALID";
	}
#undef X
}

////////////////////////////////////////////////////////////////////////////////

typedef enum {
	REQ_INVALID,
	REQ_PARSE,
	REQ_WRITE,
	REQ_DONE,
} ReadDirective;

typedef struct {
	ev_io         io;
	ev_timer*     timer;
	int           sock;
	ReadDirective dir;

	char*  buf;
	size_t buf_len;
	size_t buf_cap;
	size_t buf_brk;

	int content_length;
} ReqWatcher;

typedef struct {
	ev_timer    timer;
	ReqWatcher* req;
} ReqTimeout;

////////////////////////////////////////////////////////////////////////////////

typedef struct {
	char*          str;
	size_t         cap;
	size_t         len;
	const uint8_t* body;
	size_t         body_len;
} Response;

Response response_congrats = {
	.body     = CONGRATS,
	.body_len = sizeof(CONGRATS),
};

Response response_favicon = {
	.body     = FAVICON,
	.body_len = sizeof(FAVICON),
};

Response response_homepage = {
	.body     = HOMEPAGE,
	.body_len = sizeof(HOMEPAGE),
};

Response response_finisher = {
	.body     = FINISHER,
	.body_len = sizeof(FINISHER),
};

Response response_400 = {0};

Response response_404 = {0};

Response response_405 = {0};

Response response_408 = {0};

Response response_505 = {0};

////////////////////////////////////////////////////////////////////////////////

/* ONLY USE WITH STRING LITERALS */
void responseInit(Response* response) {
	response->str = calloc(1024, sizeof(char));
	if(!response->str) die("Failed to allocate memory");
	response->cap = 1024;
}

void headerAdd(const char* str, Response* response) {
	size_t len = strlen(str) + 1;

	if(len + response->len > response->cap) {
		response->str =
			realloc(response->str, sizeof(char) * response->cap * 1);
		if(!response->str) die("Failed to allocate memory");
		response->cap *= 2;
	}

	for(size_t i = 0; i < len - 1; i++) {
		response->str[i + response->len] = str[i];
	}

	response->len += len;
	response->str[response->len - 1] = '\n';
}

void headerFinish(Response* response) {
	if(response->len + 1 > response->cap) {
		response->str =
			realloc(response->str, sizeof(char) * response->len * 1);
		if(!response->str) die("Failed to allocate memory");
		response->cap *= 2;
	}

	response->str[response->len] = '\n';
	response->len++;
}

void sendAll(int socket, const uint8_t* data, size_t len) {
	log("Sending data...");

	ssize_t data_sent = 0;
	while(data_sent < (ssize_t) len) {
		data_sent = send(socket, data + data_sent, len - data_sent, 0);

		if(data_sent < 0) {
			close(socket);
			die("Failed to send data");
		}
	}

	log("Done sending data!");
}

////////////////////////////////////////////////////////////////////////////////

void sendHTTP(int socket, Response response) {
	sendAll(socket, (uint8_t*) response.str, response.len);
	if(response.body) sendAll(socket, response.body, response.body_len);
}

void killConnection(struct ev_loop* loop, ReqWatcher* watcher) {
	close(watcher->sock);

	ev_timer_stop(loop, watcher->timer);
	free(watcher->timer);

	ev_io_stop(loop, &watcher->io);
	free(watcher->buf);
	free(watcher);
}

ReadDirective parseHTTP(ReqWatcher* req) {
	log("\tparsing...");

	Method meth    = METH_INVALID;
	char*  path    = NULL;
	char*  version = NULL;

	char* header_key   = NULL;
	char* header_value = NULL;

	//////////////////// METHOD

	char* token = strtok(req->buf, " ");
	log("Method: %s", token);

	if(strcmp("GET", token) == 0) {
		meth = METH_GET;
	} else if(strcmp("POST", token) == 0) {
		meth = METH_POST;
	} else {
		sendHTTP(req->sock, response_405);
		return REQ_DONE;
	}

	//////////////////// PATH

	path = strtok(NULL, " ");
	log("Path: %s", path);

	//////////////////// VERSION

	version = strtok(NULL, "\n");
	log("Version: %s", version);

	//////////////////// CONTENT LENGTH

	if(meth == METH_POST) {
		do {
			header_key   = strtok(NULL, " ");
			header_value = strtok(NULL, "\n");
			log("Header: %s, Value: %s", header_key, header_value);
		} while(strcasecmp("content-length:", header_key) != 0);
		log("Content-Length: %s", header_value);

		req->content_length = atoi(header_value);
		if(req->content_length == 0) {
			sendHTTP(req->sock, response_400);
			return REQ_DONE;
		}
	}

	//////////////////// ROUTE

	if(strncmp("HTTP/1.1", version, 8) != 0) {
		log("ERR: 505");
		sendHTTP(req->sock, response_505);
		return REQ_DONE;
	}

	switch(meth) {
	case METH_GET:
		if(strcmp(path, "/") == 0) {
			sendHTTP(req->sock, response_homepage);
		} else if(strcmp(path, "/favicon.ico") == 0) {
			sendHTTP(req->sock, response_favicon);
		} else if(strcmp(path, "/congrats.webp") == 0) {
			sendHTTP(req->sock, response_congrats);
		} else {
			sendHTTP(req->sock, response_404);
		}
		break;

	case METH_POST:
		if(strcmp(path, "/finished") == 0) {
			sendHTTP(req->sock, response_finisher);
		} else {
			sendHTTP(req->sock, response_404);
		}
		break;

	default:
		die("Unreachable: Invalid method after validation");
	}

	return REQ_DONE;
}

#undef NEXT_TOKEN

////////////////////////////////////////////////////////////////////////////////

#define CLEAR_BUFFER                                                           \
	do {                                                                       \
		req->buf_len = 0;                                                      \
		req->buf_brk = 0;                                                      \
	} while(0);

void onReadReady(struct ev_loop* loop, ev_io* w, int revents) {
	if(!(revents & EV_READ)) {
		die("Unreachable: Didn't get EV_READ as revent on read-ready callback");
	}

	ReqWatcher* req = (ReqWatcher*) w;

	ssize_t just_read = read(
		req->sock,                  //
		req->buf + req->buf_len,    //
		req->buf_cap - req->buf_len //
	);

	log("INFO: Read %ld bytes from user", just_read);
	// log("%.*s", (int) just_read, req->buf);

	if(just_read < 0) {
		close(req->sock);
		die("ERR: Failed to read from client");
	}

	req->buf_len += just_read;

	int newlines = 0;

	for(size_t i = req->buf_len - just_read; i < req->buf_len; i++) {
		if(req->buf[i] == '\n') {
			newlines++;

			if(newlines == 2) {
				req->buf_brk = i + 1;

				req->dir = parseHTTP(req);
				log("Found newlines");
				break;
			}
		} else if(req->buf[i] != '\r') {
			newlines = 0;
		}
	}

	switch(req->dir) {
	case REQ_DONE:
		log("Done!\n\n");

		CLEAR_BUFFER;

		killConnection(loop, req);

		break;
	case REQ_WRITE:
		log("\t Writing...");

		ssize_t just_written = fwrite(
			req->buf,      //
			sizeof(char),  //
			req->buf_len,  //
			form_data_file //
		);

		if(just_written < req->content_length) {
			req->content_length -= just_written;
		} else {
			req->dir = REQ_PARSE;
		}

		CLEAR_BUFFER;

		break;

	default:
		die("ERR: Got invalid read directive");

		return;
	}
}

#undef CLEAR_BUFFER

void onTimeout(struct ev_loop* loop, ev_timer* t, int revents) {
	log("WARN: Request timed out, closing connection");

	if(!(revents & EV_TIMER)) {
		die(
			"ERR: Unreachable: Did not get EV_TIMER revent on timeout callback"
		);
	}

	ReqTimeout* timer   = (ReqTimeout*) t;
	ReqWatcher* watcher = timer->req;

	sendHTTP(watcher->sock, response_408);

	killConnection(loop, watcher);
}

void onNewConn(struct ev_loop* loop, ev_io* w, int revents) {
	(void) w;

	log("INFO: Got connection!");

	if(!(revents & EV_READ)) {
		die("Unreachable: Didn't get EV_READ as revent on read-ready callback");
	}

	ReqWatcher* watcher = malloc(sizeof(ReqWatcher));
	ReqTimeout* timer   = malloc(sizeof(ReqTimeout));

	if(!watcher) die("ERR: Failed to allocate memory for new read watcher");

	*watcher = (ReqWatcher) {
		.sock    = accept4(server_sock, NULL, NULL, SOCK_NONBLOCK),
		.dir     = REQ_PARSE,
		.timer   = (ev_timer*) timer,
		.buf     = calloc(1 << 20, sizeof(char)),
		.buf_len = 0,
		.buf_cap = 1 << 20,
	};

	ev_io_init((ev_io*) watcher, onReadReady, watcher->sock, EV_READ);
	ev_io_start(loop, (ev_io*) watcher);

	////////////////////

	*timer = (ReqTimeout) {
		.req = watcher,
	};

#define TIMEOUT 10.0

	ev_timer_init((ev_timer*) timer, onTimeout, TIMEOUT, 0.0);
	ev_timer_start(loop, (ev_timer*) timer);

	log("Started request handler for client with timeout: %f", TIMEOUT);

#undef TIMEOUT
}

int main(void) {
	// Generate HTTP responses

	responseInit(&response_congrats);
	headerAdd(RESPONSE_OK, &response_congrats);
	headerAdd(KEEP_ALIVE, &response_congrats);
	headerFinish(&response_congrats);

	////////////////////

	responseInit(&response_favicon);
	headerAdd(RESPONSE_OK, &response_favicon);
	headerAdd(KEEP_ALIVE, &response_favicon);
	headerFinish(&response_favicon);

	////////////////////

	responseInit(&response_homepage);
	headerAdd(RESPONSE_OK, &response_homepage);
	headerAdd(CONTENT_HTML, &response_homepage);
	headerAdd(KEEP_ALIVE, &response_homepage);
	headerFinish(&response_homepage);

	////////////////////

	responseInit(&response_finisher);
	headerAdd(RESPONSE_OK, &response_finisher);
	headerAdd(CONTENT_HTML, &response_finisher);
	headerAdd(KEEP_ALIVE, &response_finisher);
	headerFinish(&response_finisher);

	////////////////////

	responseInit(&response_400);
	headerAdd(RESPONSE_ERR_400, &response_400);
	headerAdd(KEEP_ALIVE, &response_400);
	headerFinish(&response_400);

	////////////////////

	responseInit(&response_404);
	headerAdd(RESPONSE_ERR_404, &response_404);
	headerAdd(KEEP_ALIVE, &response_404);
	headerFinish(&response_404);

	////////////////////

	responseInit(&response_408);
	headerAdd(RESPONSE_ERR_408, &response_408);
	headerAdd(KEEP_ALIVE, &response_408);
	headerFinish(&response_408);

	////////////////////

	responseInit(&response_505);
	headerAdd(RESPONSE_ERR_505, &response_505);
	headerAdd(KEEP_ALIVE, &response_505);
	headerFinish(&response_505);

	////////////////////
	// Prepare form response outfile

	form_data_file = fopen(FORM_DATA_PATH, "a");

	struct sockaddr_in server_addr = {
		.sin_family      = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port        = htons(PORTNUM),
	};

	if((server_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0) {
		die("Failed to open socket");
	}

	int yes = 1;
	if(setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) <
	   0) {
		die("Failed to set sockopt");
	}

	if(bind(server_sock, (struct sockaddr*) &server_addr, sizeof(server_addr)) <
	   0) {
		die("Failed to bind socket");
	}

	if(listen(server_sock, 1) < 0) die("Failed to listen on socket");

	////////////////////
	// Prepare event loop

	struct ev_loop* event_loop = EV_DEFAULT;

	ev_io conn_watcher = {0};
	ev_io_init(&conn_watcher, onNewConn, server_sock, EV_READ);
	ev_io_start(event_loop, &conn_watcher);

	log(BOLD("Listening on port %d..."), PORTNUM);

	int ev_fail = ev_run(event_loop, 0);

	fclose(form_data_file);

	return ev_fail;
}
