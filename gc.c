#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <assert.h>
#include <string.h>
#include <ctype.h>

#define BUFSIZE 1024 * 1024

typedef enum {
	false,
	true
} bool;

struct gopherdata {
	char type;
	char *display;
	char *selector;
	char *host;
	char *port;
	int menuindex;
	struct gopherdata *next;
};

struct pagestack {
	struct gopherdata **pages;
	int top;
};

int 
getconn(
	char *host,
	char *port)
{
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
	char *request)
{
	char reqstr[strlen(request) + 2];
	strcpy(reqstr, request);
	strcat(reqstr, "\r\n");
	if (send(sock, reqstr, strlen(reqstr), 0) < 0) {
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
	int menuindex = 0;

	struct gopherdata *gd = (struct gopherdata *)malloc(sizeof (struct gopherdata));
	struct gopherdata *gdp = gd;

	for (char *bufp = buf; tbytes > 1; tbytes--) {
		if (f == 0) {
			gdp->type = *bufp;
			if (gdp->type == '0' || gdp->type == '1') {
				gdp->menuindex = ++menuindex;
			} else {
				gdp->menuindex = 0;
			}
			++f;
		} else if (*bufp == '\t' || *bufp == '\r') {
			char *str = (char *)malloc((c + 1) * sizeof(char));
			strncpy(str, bufp - c, c);
			*(str + c) = '\0';
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
			gdp->next = (struct gopherdata *)malloc(sizeof (struct gopherdata));
			gdp = gdp->next;
		} else {
			++c;
		}
		++bufp;
	}

/* This was causing a segmentation fault for emtpy pages, because gdp->next is
 * not set when the selector does not exist. */
#if 0
	free(gdp->next);
#endif

	gdp->next = NULL;

	return gd;
}

int
formatpage(
	struct gopherdata *gd,
	char *output)
{
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
			op += sprintf(op, "(%d) %s\n", ++menuitem, display);
			break;
		case '1':
			op += sprintf(op, "(%d) %s/\n", ++menuitem, display);
			break;
		default:
			break;
		}
	}

	op += sprintf(op, "\n:");

	return (op - output);
}

int 
main()
{
	char *buf = (char *)malloc(BUFSIZE);
	struct gopherdata *gd = NULL;
	struct pagestack stack;
	stack.pages = (struct gopherdata **)malloc(100 * sizeof(struct gopherdata *));
	stack.top = 0;

	char type = '1';
	char *selector = "\0";
	char *host = "quux.org";
	char *port = "70";
	char c = 0;

	struct termios origterm, rawterm;

	if (tcgetattr(0, &origterm) == -1)
		exit(1);

	rawterm = origterm;

	rawterm.c_lflag &= ~ICANON;
	rawterm.c_lflag &= ECHO;
	rawterm.c_cc[VMIN] = 1;
	rawterm.c_cc[VTIME] = 0;

	if (tcsetattr(0, TCSAFLUSH, &rawterm) == -1)
		exit(1);

	bool running = true;

	int menui = 0;
	while (running) {
		char *output = (char *)malloc(32768);
		int tbytes;

		if (menui) {
			for (struct gopherdata *gdp = gd; gdp != NULL; gdp = gdp->next) {
				if (menui == gdp->menuindex) {
					type = gdp->type;
					selector = gdp->selector;
					host = gdp->host;
					port = gdp->port;
					break;
				}
			}
		}

		if (type == '1') {
			if (selector) {
				int sock = getconn(host, port);
				tbytes = getdata(sock, buf, selector);
				close(sock);
				gd = parsedata(buf, tbytes);
				*(stack.pages + stack.top) = gd;
				++stack.top;
			} else {
				--stack.top;
				gd = *(stack.pages + (stack.top - 1));
			}
			tbytes = formatpage(gd, output);
		} else if (type == '0') {
			int sock = getconn(host, port);
			tbytes = getdata(sock, buf, selector);
			close(sock);
			output = buf;
		}

		char *op = output;
		while (tbytes >= 0) {
			write(1, op, 1000);
			op += 1000;
			tbytes -= 1000;

			read(0, &c, 1);

			if (c == 'u') {
				menui = 0;
				selector = NULL;
				type = '1';
				break;
			} else if (c == 'q') {
				write(1, "\n", 1);
				running = false;
				break;
			} else if (c >= 48 && c <= 57) {
				menui = 0;
				while (c != '\n') {
					if (c >= 48 && c <= 57) {
						menui = (menui * 10) + (c - 48);
					} else {
						menui = 0;
						break;
					}
					read(0, &c, 1);
				}
				if (menui) break;
			} else if (c == 'm') {
				continue;
			}
		}
	}

	if (tcsetattr(0, TCSAFLUSH, &origterm) == -1)
		exit(1);

	return 0;
}
