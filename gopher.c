#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>
#include <string.h>

#define BUFSIZE 4096

void zero(void *p, int n)
{
	while (n--) {
		*(char *)p++ = 0;
	}
}

int main()
{
	char *host = "quux.org";
	char *port = "70";
	int sock;
	char buf[BUFSIZE];
	struct addrinfo hints, *server, *aip;

	zero(&hints, sizeof(hints));

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &server) != 0) {
		return 1;
	}

	for (aip = server; aip; aip = aip->ai_next) {
		if ((sock = socket(aip->ai_family, aip->ai_socktype, aip->ai_protocol)) < 0) {
			continue;
		}

		if (connect(sock, aip->ai_addr, aip->ai_addrlen) < 0) {
			close(sock);
			continue;
		}

		break;
	}

	if (!aip) {
		fprintf(stderr, "can't connect\n");
		return 1;
	}

	freeaddrinfo(server);

	if (send(sock, "\r\n", 2, 0) < 0) {
		fprintf(stderr, "can't send\n");
		return 1;
	}

	int nbytes;
	int tbytes = 0;
	while ((nbytes = recv(sock, buf + tbytes, BUFSIZE - 1, 0)) > 0) {
		tbytes += nbytes;
	}

	buf[tbytes] = '\0';

	int c = 0; /* character */
	int f = 0; /* field */
	int r = 0; /* record */
	char *page[100][4] = {0};
	for (char *p = buf; tbytes > 0; tbytes--) {
		if (*p == '\t' || *p == '\r') {
			char *str = malloc(c + 1);
			strncpy(str, p - c, c);
			*(str + (c + 1)) = '\0';
			page[r][f++] = str;
			c = 0;
		} else if (*p == '\n') {
			c = 0;
			f = 0;
			r++;
		} else {
			c++;
		}
		p++;
	}

	char *output = malloc(4096);
	int oi = 0;
	for (int i = 0; i < r; i++) {
		char type = page[i][0][0];
		char *display = &page[i][0][1];
		switch (type) {
		case 'i':
			oi += sprintf(output + oi, "%s\n", display);
			break;
		case '0':
			oi += sprintf(output + oi, "[F] %s\n", display);
			break;
		case '1':
			oi += sprintf(output + oi, "[D] %s\n", display);
			break;
		}
	}

	write(1, output, oi);

	close(sock);

	return 0;
}
