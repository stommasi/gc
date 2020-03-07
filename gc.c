#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netdb.h>

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

struct buffer {
	char *data;
	char *p;
	int nbytes;
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

void
getdata(
	int sock,
	struct buffer *inbuf,
	char *request)
{
	char reqstr[strlen(request) + 2];
	strcpy(reqstr, request);
	strcat(reqstr, "\r\n");
	if (send(sock, reqstr, strlen(reqstr), 0) < 0) {
		fprintf(stderr, "can't send\n");
	}

	inbuf->nbytes = 0;
	int nbytes;
	while ((nbytes = recv(sock, inbuf->data + inbuf->nbytes, BUFSIZE - 1, 0)) > 0) {
		inbuf->nbytes += nbytes;
	}

	*(inbuf->data + inbuf->nbytes) = '\0';
}

struct gopherdata *
parsedata(
	struct buffer *inbuf)
{
	int c = 0; /* character */
	int f = 0; /* field */
	int menuindex = 0;

	struct gopherdata *gd = (struct gopherdata *)malloc(sizeof (struct gopherdata));
	struct gopherdata *gdp = gd;

	for (inbuf->p = inbuf->data; inbuf->p - inbuf->data < inbuf->nbytes; inbuf->p++) {
		if (f == 0) {
			gdp->type = *inbuf->p;
			if (gdp->type == '0' || gdp->type == '1') {
				gdp->menuindex = ++menuindex;
			} else {
				gdp->menuindex = 0;
			}
			++f;
		} else if (*inbuf->p == '\t' || *inbuf->p == '\r') {
			char *str = (char *)malloc((c + 1) * sizeof(char));
			strncpy(str, inbuf->p - c, c);
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
		} else if (*inbuf->p == '\n') {
			c = 0;
			f = 0;
			gdp->next = (struct gopherdata *)malloc(sizeof (struct gopherdata));
			gdp = gdp->next;
		} else {
			++c;
		}
	}

	gdp->next = NULL;

	return gd;
}

void
formatpage(
	struct gopherdata *gd,
	struct buffer *outbuf)
{
	outbuf->p = outbuf->data;
	int menuitem = 0;
	struct gopherdata *gdp;

	for (gdp = gd; gdp != NULL; gdp = gdp->next) {
		char type = gdp->type;
		char *display = gdp->display;
		switch (type) {
		case 'i':
			outbuf->p += sprintf(outbuf->p, "%s\n", display);
			break;
		case '0':
			outbuf->p += sprintf(outbuf->p, "(%d) %s\n", ++menuitem, display);
			break;
		case '1':
			outbuf->p += sprintf(outbuf->p, "(%d) %s/\n", ++menuitem, display);
			break;
		default:
			break;
		}
	}

	outbuf->p += sprintf(outbuf->p, "\n");
	outbuf->nbytes = outbuf->p - outbuf->data;
	outbuf->p = outbuf->data;
}

void
printbuffer(
	struct buffer *outbuf)
{
	struct winsize ws;
	ioctl(1, TIOCGWINSZ, &ws); /* get terminal rows and columns */

	int row = 0;
	int col = 0;
	int max = 0;

	/* Count bytes (max) in 5 less than a screen's worth of rows */
	while (row < ws.ws_row - 5) {
		if (*(outbuf->p + max) == '\n' || col == ws.ws_col) {
			row++;
			col = 0;
		}
		max++;
		col++;
	}

	/* Bytes remaining in document */
	int rem = outbuf->nbytes - (outbuf->p - outbuf->data);

	outbuf->p += write(1, outbuf->p, (rem < max) ? rem : max);

	rem = outbuf->nbytes - (outbuf->p - outbuf->data);
	if (rem > 0) {
		printf(
			"\n--MORE (%.1f%% remaining)--\n",
			((float)rem / (float)outbuf->nbytes) * 100);
	}
}

int 
main()
{
	struct buffer inbuf;
	inbuf.data = (char *)malloc(BUFSIZE);
	inbuf.nbytes = 0;

	struct buffer outbuf;
	outbuf.data = (char *)malloc(BUFSIZE);
	outbuf.nbytes = 0;

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

	rawterm.c_lflag &= ~ICANON; /* don't wait for newline to process input */
	rawterm.c_lflag &= ~ECHO;
	rawterm.c_iflag |= IGNBRK; /* leave ctrl-c to the terminal/debugger */
	rawterm.c_cc[VMIN] = 1; /* wait for one character */
	rawterm.c_cc[VTIME] = 0; /* don't timeout */

	if (tcsetattr(0, TCSAFLUSH, &rawterm) == -1)
		exit(1);

	bool running = true;

	int menui = 0;
	while (running) {
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
				getdata(sock, &inbuf, selector);
				close(sock);
				gd = parsedata(&inbuf);
				*(stack.pages + stack.top) = gd;
				++stack.top;
			} else {
				if (stack.top > 1) {
					--stack.top;
					gd = *(stack.pages + (stack.top - 1));
				}
			}
			formatpage(gd, &outbuf);
		} else if (type == '0') {
			int sock = getconn(host, port);
			getdata(sock, &inbuf, selector);
			close(sock);
			outbuf.data = inbuf.data;
			outbuf.nbytes = inbuf.nbytes;
			outbuf.p = outbuf.data;
		}

		write(1, "\n", 1);
		printbuffer(&outbuf);

		while (read(0, &c, 1)) {
			if (c == 'u') {
				menui = 0;
				selector = NULL;
				type = '1';
			} else if (c == 'q') {
				write(1, "\n", 1);
				running = false;
			} else if (c >= 48 && c <= 57) {
				menui = 0;
				write(1, "Select: ", 8);
				while (c != '\n') {
					if (c >= 48 && c <= 57) {
						menui = (menui * 10) + (c - 48);
						write(1, &c, 1);
					} else if (c == 127) { /* backspace */
						write(1, "\x1b[D\x1b[0K", 7); /* back and erase */
						menui /= 10; /* remove the last digit */
					}
					read(0, &c, 1);
				}
			} else if (c == ' ') {
				menui = 0;
				type = 0;
			} else {
				continue;
			}
			break;
		}
	}

	if (tcsetattr(0, TCSAFLUSH, &origterm) == -1)
		exit(1);

	return 0;
}
