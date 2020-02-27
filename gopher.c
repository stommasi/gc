#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>
#include <string.h>

#define BUFSIZE 4096

struct gopherdata {
	char type;
	char *display;
	char *selector;
	char *host;
	char *port;
	struct gopherdata *next;
};

int 
getconn()
{
	char *host = "quux.org";
	char *port = "70";
	int sock;
	struct addrinfo hints, *server, *aip;

	memset(&hints, 0, sizeof hints);

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

	return sock;
}

int 
getdata(
	int sock,
	char *buf,
	const char *request)
{
	if (send(sock, request, strlen(request), 0) < 0) {
		fprintf(stderr, "can't send\n");
		return 1;
	}

	int nbytes;
	int tbytes = 0;
	while ((nbytes = recv(sock, buf + tbytes, BUFSIZE - 1, 0)) > 0) {
		tbytes += nbytes;
	}

	buf[tbytes] = '\0';

	return tbytes;
}

struct gopherdata *
parsedata(
	char *buf,
	int tbytes)
{
	int c = 0; /* character */
	int f = 0; /* field */

	struct gopherdata *gd = malloc(sizeof (struct gopherdata));
	struct gopherdata *gdp = gd;

	for (char *bufp = buf; tbytes > 0; tbytes--) {
		if (f == 0) {
			gdp->type = *bufp;
			++f;
		} else if (*bufp == '\t' || *bufp == '\r') {
			char *str = malloc(c + 1);
			strncpy(str, bufp - c, c);
			*(str + (c + 1)) = '\0';
			switch (f) {
			case 1:
				gdp->display = str;
				break;
			case 2:
				gdp->selector = str;
				break;
			case 3:
				gdp->host = str;
				break;
			case 4:
				gdp->port = str;
				break;
			}
			++f;
			c = 0;
		} else if (*bufp == '\n') {
			c = 0;
			f = 0;
			gdp->next = malloc(sizeof (struct gopherdata));
			gdp = gdp->next;
		} else {
			++c;
		}
		++bufp;
	}

	gdp->next = NULL;

	return gd;
}

void
displaypage(
	struct gopherdata *gd)
{
	char *output = malloc(4096);
	char *op = output;
	int menuitem = 0;
	struct gopherdata *gdp;

	for (gdp = gd; gdp != NULL; gdp = gdp->next) {
		char type = gdp->type;
		char *display = gdp->display;
		switch (type) {
		case 'i':
			op += sprintf(op, "%s\n", display);
			break;
		case '0':
			op += sprintf(op, "[%d]%s\n", ++menuitem, display);
			break;
		case '1':
			op += sprintf(op, "[%d]%s\n", ++menuitem, display);
			break;
		}
	}

	write(1, output, op - output);

	free(output);
}

int 
main()
{
	char buf[BUFSIZE];
	int sock = getconn();

	int tbytes = getdata(sock, buf, "\r\n");

	struct gopherdata *gd = parsedata(buf, tbytes);

	displaypage(gd);

	close(sock);

	return 0;
}
