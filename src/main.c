#define _POSIX_C_SOURCE 199309L

#include <arpa/inet.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define die(...) err(1, __VA_ARGS__)
#define PORTNUM 8080

#define BOLD(str) "[1m" str "[m"

#define RECV_BUFSIZE 1024

#define RESPONSE_ERR_405 "HTTP/1.0 405 METHOD NOT ALLOWED"
#define RESPONSE_OK "HTTP/1.0 200 OK DATA IN FLIGHT"
#define CONTENT_HTML "Content-Type: text/html; charset=utf-8"

const uint8_t FAVICON[] = {
#embed "../res/favicon.ico"
};

const uint8_t HOMEPAGE[] = {
#embed "index.html"
};

const uint8_t FINISHER[] = {
#embed "finished.html"
};

typedef struct {
	char*          str;
	size_t         cap;
	size_t         len;
	const uint8_t* body;
	size_t         body_len;
} Response;

////////////////////////////////////////////////////////////////////////////////

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
	puts("Sending data...");

	ssize_t data_sent = 0;
	while(data_sent < (ssize_t) len) {
		data_sent = send(socket, data + data_sent, len - data_sent, 0);

		if(data_sent < 0) {
			close(socket);
			die("Failed to send data");
		}
	}

	puts("Done sending data!");
}

void sendHTTP(int socket, Response* response) {
	sendAll(socket, (uint8_t*) response->str, response->len);
	if(response->body) sendAll(socket, response->body, response->body_len);
}

void handleRequest(int socket) {
	static char*   buf = NULL;
	static ssize_t len = 0;
	static ssize_t cap = 0;

	// init the buffer for first run
	if(!buf) {
		buf = calloc(RECV_BUFSIZE, sizeof(char));
		if(!buf) die("Failed to allocate memory");
		cap = RECV_BUFSIZE;
	}

	for(;;) {

		ssize_t data_read = recv(socket, buf + len, cap - len - 1, 0);
		printf("Read %ld bytes of data\n", data_read);

		if(data_read < 0) {
			close(socket);
			die("Failed to receive data from user");
		}

		len += data_read;

		if(len >= cap - 1) {
			buf = realloc(buf, sizeof(char) * (cap << 2));
			if(!buf) {
				close(socket);
				die("Failed to resize buffer");
			}

			cap <<= 1;
		} else {
			break;
		}
	}
	buf[len - 1] = '\0';

	printf("Done reading data, total: %ld bytes\n", len);

	printf("Got message from client:\n" BOLD("%.*s") "\n", (int) len, buf);

	char* tok = strtok(buf, " ");

	printf("First token: (%s)\n", tok);

	if(strcmp(tok, "GET") == 0) {
		tok = strtok(NULL, " ");

		printf("Second token: %s\n", tok);

		if(strcmp(tok, "/favicon.ico") == 0) {
			printf(
				"Got request for favicon, sending %zu bytes %zu\n",
				sizeof(FAVICON), sizeof(RESPONSE_OK) - 1
			);

			sendHTTP(socket, &response_favicon);

			printf("Finished, closing connection...\n");
		} else if(strcmp(tok, "/") == 0) {
			printf(
				"Got request for homepage, sending %lu bytes\n",
				sizeof(HOMEPAGE)
			);

			sendHTTP(socket, &response_homepage);

			printf("Finished, closing connection...\n");
		}
	} else if(strcmp(tok, "POST") == 0) {
		tok = strtok(NULL, " ");

		if(strcmp(tok, "/finished") == 0) {
			printf(
				"Got request for finisher page, sending %lu bytes\n",
				sizeof(FINISHER)
			);

			sendHTTP(socket, &response_finisher);

			printf("Finished, closing connection...\n");
		}
	} else {
		sendHTTP(socket, &response_405);
	}

	memset(buf, 0, len * sizeof(char));
	len = 0;
}

int main(void) {
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

	printf(
		BOLD(
			"====================\nServer open!\n"
		) "Listening on port %d...\n",
		PORTNUM
	);

	for(;;) {
		if((client_sock = accept(server_sock, (struct sockaddr*) NULL, NULL)) <
		   0) {
			die("Failed to accept connection from client");
		}

		handleRequest(client_sock);

		close(client_sock);
	}

	close(server_sock);
	printf(BOLD("Server closed!\n====================\n"));

	return 0;
}
