#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <err.h>
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

#define RESPONSE_ERR_405 "HTTP/1.0 405 METHOD NOT ALLOWED"
#define RESPONSE_ERR_404 "HTTP/1.0 404 PAGE NOT FOUND"
#define RESPONSE_OK "HTTP/1.0 200 OK DATA IN FLIGHT"
#define CONTENT_HTML "Content-Type: text/html; charset=utf-8"

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

Response response_404 = {0};

Response response_405 = {0};

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

void sendHTTP(int socket, Response response) {
	sendAll(socket, (uint8_t*) response.str, response.len);
	if(response.body) sendAll(socket, response.body, response.body_len);
}

char* trimL(char* str) {
	while(isspace(str[0])) {
		str++;
	}

	return str;
}

char* trimR(char* str) {
	size_t len = strlen(str);

	while(len > 0 && isspace(str[len - 1])) {
		len--;
	}

	str[len] = 0;

	return str;
}

char* trim(char* str) {
	return trimL(trimR(str));
}

void handleRequest(int socket) {
	FILE* f = fdopen(socket, "rb");
	if(!f) die("Failed to upcast client socket");

	static char*  buf = NULL;
	static size_t cap = 0;

	Method meth           = METH_INVALID;
	char   path[2048 + 1] = {0};
	size_t content_length = 0;
	bool   headers_done   = false;

	for(;;) {
		ssize_t len = getline((char**) &buf, &cap, f);
		if(len < 0) die("Could not get line from socket");

		trimR(buf);

		if(strlen(buf) == 0) break;

		if(headers_done) continue;

		if(meth == METH_INVALID) {
			char* tok = strtok(buf, " ");
			if(!tok) die("Could not get method token");

#define X(x)                                                                   \
	else if(strcmp(tok, #x) == 0) {                                            \
		meth = METH_##x;                                                       \
	}
			if(0) {
			} else if(strcmp(tok, "GET") == 0) {
				meth = METH_GET;
			} else if(strcmp(tok, "POST") == 0) {
				meth = METH_POST;
			} else {
				die("Failed to parse signature line");
			}

			char* path_tok = strtok(NULL, " ");
			if(!path_tok) die("Could not get path token");

			strncpy(path, path_tok, sizeof(path) - 1);
		} else {
			char* key = strtok(buf, ":");
			if(!key) die("Could not get header key");

			key = trim(key);

			if(strcasecmp(key, "content-length") == 0) {
				char* val = strtok(NULL, "\n");
				if(!val) die("Could not find value of content-length header");

				char* val_end  = NULL;
				content_length = strtoul(val, &val_end, 10);
				if(*val_end != 0)
					die("Could not convert content-length to int");

				headers_done = true;
			}
		}
	}

	log(
		"Got request: %s %s %zu ", //
		methodShow(meth),          //
		path,                      //
		content_length             //
	);

	bool ok = false;

	switch(meth) {
	case METH_GET:
		if(strcasecmp(path, "/favicon.ico") == 0) {
			log("Got favicon request, sending %zu bytes", sizeof(FAVICON));

			sendHTTP(socket, response_favicon);
			ok = true;
		} else if(strcasecmp(path, "/congrats.webp") == 0) {
			log("Got congrats request, sending %zu bytes", sizeof(CONGRATS));

			sendHTTP(socket, response_congrats);
			ok = true;
		} else if(strcasecmp(path, "/") == 0) {
			log("Got mainpage request, sending %zu bytes", sizeof(HOMEPAGE));

			sendHTTP(socket, response_homepage);
			ok = true;
		} else if(strcasecmp(path, "/quit") == 0) {
			log("Quit via request");

			exit(0);
		}

		break;

	case METH_POST:
		if(strcasecmp(path, "/finished") == 0) {
			log("Client finished form");

			const char* res_path = "results.txt";
			FILE*       res_file = fopen(res_path, "a");
			if(!res_file) die("Could not open %s", res_path);

			if(fread(buf, 1, content_length, f) < content_length)
				die("Could not read %zu bytes from socket", content_length);

			if(fwrite(buf, 1, content_length, res_file) < content_length)
				die("Could not write %zu bytes to %s", content_length,
				    res_path);

			fclose(res_file);

			sendHTTP(socket, response_finisher);

			ok = true;
		}
		break;

	default:
		die("Unreachable");
	}

	if(!ok) { sendHTTP(socket, response_404); }

	log("Finished, closing connection...");
}

int main(void) {
	responseInit(&response_congrats);
	headerAdd(RESPONSE_OK, &response_congrats);
	headerFinish(&response_congrats);

	////////////////////

	responseInit(&response_favicon);
	headerAdd(RESPONSE_OK, &response_favicon);
	headerFinish(&response_favicon);

	////////////////////

	responseInit(&response_homepage);
	headerAdd(RESPONSE_OK, &response_homepage);
	headerAdd(CONTENT_HTML, &response_homepage);
	headerFinish(&response_homepage);

	////////////////////

	responseInit(&response_finisher);
	headerAdd(RESPONSE_OK, &response_finisher);
	headerAdd(CONTENT_HTML, &response_finisher);
	headerFinish(&response_finisher);

	////////////////////

	responseInit(&response_405);
	headerAdd(RESPONSE_ERR_405, &response_405);
	headerFinish(&response_405);

	////////////////////

	responseInit(&response_404);
	headerAdd(RESPONSE_ERR_404, &response_404);
	headerFinish(&response_404);

	////////////////////

	struct sockaddr_in server_addr = {
		.sin_family      = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port        = htons(PORTNUM),
	};

	int server_sock;
	int client_sock;

	if((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
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

	log(BOLD("Listening on port %d..."), PORTNUM);

	for(;;) {
		if((client_sock = accept(server_sock, (struct sockaddr*) NULL, NULL)) <
		   0) {
			die("Failed to accept connection from client");
		}

		handleRequest(client_sock);

		close(client_sock);
	}

	return 0;
}
