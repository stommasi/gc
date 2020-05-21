/*--------------------------------------------------------------------
 * gc: gopher client
 *--------------------------------------------------------------------*/

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

/* This struct represents a Gopher line, which consists of the following
 * fields:
 * [type][display]\t[selector]\t[host]\t[port]\r\n */
struct gopherdata {
    char type;
    char *display;
    char *selector;
    char *host;
    char *port;
    int menuindex;
    struct gopherdata *next;
};

/* Stack of pages, i.e. groups of Gopher lines, kept to allow the user to go up
 * and down the browsing history without having to send re-requests to the
 * server. */
struct pagestack {
    struct gopherdata **pages;
    int top;
};

struct buffer {
    char *data;
    char *p;
    int nbytes;
};

/*--------------------------------------------------------------------
 * getconn
 *
 * Connect to a specific host and port and return the socket's file
 * descriptor.
 *--------------------------------------------------------------------*/
int getconn(char *host, char *port)
{
    int sock;
    struct addrinfo hints, *server, *aip;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* any address family */
    hints.ai_socktype = SOCK_STREAM; /* TCP socket */
    if (getaddrinfo(host, port, &hints, &server) != 0)
        return 1;
    /* Try addresses until socket connection established */
    for (aip = server; aip; aip = aip->ai_next) {
        if ((sock = socket(aip->ai_family, aip->ai_socktype, aip->ai_protocol)) < 0)
            continue;
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

/*--------------------------------------------------------------------
 * getdata
 *
 * Send the Gopher selector as a request string to the server.
 * Hopefully receive back a stream of data to store in inbuf->data.
 *--------------------------------------------------------------------*/
void getdata(int sock, struct buffer *inbuf, char *request)
{
    char reqstr[strlen(request) + 2];
    strcpy(reqstr, request);
    strcat(reqstr, "\r\n");
    if (send(sock, reqstr, strlen(reqstr), 0) < 0)
        fprintf(stderr, "can't send\n");
    inbuf->nbytes = 0;
    int nbytes;
    while ((nbytes = recv(sock, inbuf->data + inbuf->nbytes, BUFSIZE - 1, 0)) > 0)
        inbuf->nbytes += nbytes;
    *(inbuf->data + inbuf->nbytes) = '\0';
}

/*--------------------------------------------------------------------
 * parsedata
 *
 * Go through, one character at a time, the raw data sent by the
 * server. Store the various string fields of each line in the
 * appropriate members of a heap-allocated gopherdata struct. Allocate
 * a new struct for each line, and have each point to the next in a
 * linked list. Return a pointer to the head of the list.
 *--------------------------------------------------------------------*/
struct gopherdata *parsedata(struct buffer *inbuf)
{
    int c = 0; /* character */
    int f = 0; /* field */
    int menuindex = 0;
    /* Head of linked list of Gopher data lines */
    struct gopherdata *gd = (struct gopherdata *)malloc(sizeof(struct gopherdata));
    struct gopherdata *gdp = gd;
    /* Iterate over each character of the data */
    for (inbuf->p = inbuf->data; inbuf->p - inbuf->data < inbuf->nbytes; inbuf->p++) {
        if (f == 0) {
            gdp->type = *inbuf->p;
            /* Store the number of menu items on a Gopher page */
            if (gdp->type == '0' || gdp->type == '1') {
                gdp->menuindex = ++menuindex;
            } else {
                gdp->menuindex = 0;
            }
            ++f;
        /* If we have reached the end of a field, store the string on the heap
         * and have the appropriate struct member point to it */
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
            /* Linked list */
            gdp->next = (struct gopherdata *)malloc(sizeof(struct gopherdata));
            gdp = gdp->next;
        } else {
            ++c;
        }
    }
    gdp->next = NULL;
    return gd;
}

/*--------------------------------------------------------------------
 * formatpage
 *
 * Start from the head of a linked list of Gopher lines and proceed
 * to sprintf each of their display strings to the output buffer.
 *--------------------------------------------------------------------*/
void formatpage(struct gopherdata *gd, struct buffer *outbuf)
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

/*--------------------------------------------------------------------
 * printbuffer
 *
 * Write as much of the output buffer to the screen, beginning
 * wherever the last call to this function left off, as will fit into
 * 5 lines less than the height of the user's terminal.
 *--------------------------------------------------------------------*/
void printbuffer(struct buffer *outbuf)
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

/*--------------------------------------------------------------------
 * main
 *
 * Put the terminal in raw mode and start the main loop.
 *--------------------------------------------------------------------*/
int main()
{
    /* Data received from server */
    struct buffer inbuf;
    inbuf.data = (char *)malloc(BUFSIZE);
    inbuf.nbytes = 0;
    /* Data formatted for output */
    struct buffer outbuf;
    outbuf.data = (char *)malloc(BUFSIZE);
    outbuf.nbytes = 0;
    /* Initialize empty linked list of Gopher lines */
    struct gopherdata *gd = NULL;
    /* Stack of pointers to pages (point to first lines) */
    struct pagestack stack;
    stack.pages = (struct gopherdata **)malloc(100 * sizeof(struct gopherdata *));
    stack.top = 0;
    /* Select a default page */
    char type = '1';
    char *selector = "\0";
    char *host = "quux.org";
    char *port = "70";
    char c = 0;
    /* Put the terminal in raw mode */
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
    /* Main loop */
    bool running = true;
    int menui = 0;
    while (running) {
        /* Check if selected link exists and collect its metadata */
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
        /* If the link points to another Gopher menu, request the menu, parse
         * it, put it on the stack of pages, and format it */
        if (type == '1') {
            if (selector) {
                int sock = getconn(host, port);
                getdata(sock, &inbuf, selector);
                close(sock);
                gd = parsedata(&inbuf);
                *(stack.pages + stack.top) = gd;
                ++stack.top;
            /* If there is no selector, then the user actually chose to go back
             * a page with 'u' and we should just descend the stack */
            } else {
                if (stack.top > 1) {
                    --stack.top;
                    gd = *(stack.pages + (stack.top - 1));
                }
            }
            formatpage(gd, &outbuf);
        /* If the link points to a text file, then get the data and just fill
         * the output buffer with it as is */
        } else if (type == '0') {
            int sock = getconn(host, port);
            getdata(sock, &inbuf, selector);
            close(sock);
            outbuf.data = inbuf.data;
            outbuf.nbytes = inbuf.nbytes;
            outbuf.p = outbuf.data;
        }
        /* Write the output buffer to the screen */
        write(1, "\n", 1);
        printbuffer(&outbuf);
        /* Get user input */
        while (read(0, &c, 1)) {
            if (c == 'u') { /* "up", or go back a page */
                menui = 0;
                selector = NULL;
                type = '1';
            } else if (c == 'q') { /* quit */
                write(1, "\n", 1);
                running = false;
            } else if (c >= 48 && c <= 57) { /* Gopher item # selection */
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
            /* Scroll to next page */
            } else if (c == ' ') {
                menui = 0;
                type = 0;
            } else {
                continue;
            }
            break;
        }
    }
    /* Restore original terminal settings before exiting */
    if (tcsetattr(0, TCSAFLUSH, &origterm) == -1)
        exit(1);
    return 0;
}
