/* $Id$ */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <err.h>
#include "fscbp.h"

#define PORT 6986
#define Q 1

typedef char row_t;
typedef char col_t;
typedef unsigned short hash_t;

struct coord {
	row_t row;
	col_t col;
};

void servsetup(char *, in_port_t);
void clisetup(char *, in_port_t);
void sendbomb(void);
void mainloop(void);
void procexpected(char);
void sendmessage(char, ...);

hash_t peerhash;
int sock;
int verbose;

void
debug(char *fmt, ...)
{
	va_list ap;

	if (verbose == 0)
		return;

	fprintf(stderr, "[debug] ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

void
usage(int status)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-hdv] [-p port] [addr[:port]]\n",
		__progname);
	exit(status);
}

int
main(int argc, char *argv[])
{
	int ch, daemon = 0;
	in_port_t port = PORT;
	char *pos, *hostname = NULL;
	unsigned long l;
	extern char *optarg;
	extern int optind;

	while ((ch = getopt(argc, argv, "dhp:v")) != -1) {
		switch (ch) {
		case 'd':
			daemon = 1;
			break;
		case 'h':
			usage(0);
			/* FALLTHROUGH */
		case 'p':
			if ((l = strtoul(optarg, NULL, 10)) < 0 || l > 65535)
				errx(1, "bad port");
			port = l;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage(1);
		}
	}

	argc -= optind;
	argv += optind;

	if (daemon) {
		if (argc > 1)
			usage(1);
	} else {
		if (argc != 1)
			usage(1);
	}

	debug("Parsed arguments, examining host");

	if (argv[0] == NULL) {
		if (daemon == 0)
			usage(1);
	} else {
		/* Parse port. */
		if ((pos = strchr(argv[0], ':')) != NULL) {
			*pos = '\0';
			if ((l = strtoul(pos + 1, NULL, 10)) < 0 || l > 65535)
				errx(1, "bad port");
			port = l;
		}
		hostname = argv[0];
	}

	if (daemon) {
		servsetup(hostname, port);
		sendbomb();
	} else
		clisetup(hostname, port);

	debug("Starting main loop");
	mainloop();

	return 0;
}

void
parseaddr(char *hostname, in_port_t port, void *buf)
{
	struct addrinfo hints, *res;
	struct sockaddr_in *addr = (struct sockaddr_in *)buf;
	size_t len;
	char *sport, ip[BUFSIZ];
	int ecode;

	memset(addr, 0, sizeof(*addr));
	addr->sin_len = sizeof(*addr);
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);

	if (hostname == NULL) {
		addr->sin_addr.s_addr = htonl(INADDR_ANY);
		return;
	}

	debug("Hostname: %s", hostname);

	switch (inet_pton(AF_INET, hostname, &addr->sin_addr)) {
	case -1:
		err(1, "inet_pton");
		/* FALLTHROUGH */
	case  0:
		debug("Attempting to resolve hostname");
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = PF_INET;
		hints.ai_socktype = SOCK_STREAM;
		len = 2 + (int)log10(port);
		if ((sport = malloc(len)) == NULL)
			err(1, "malloc");
		snprintf(sport, len, "%d", port);
		debug("String port: %s", sport);
		if ((ecode = getaddrinfo(hostname, sport, &hints, &res)) != 0)
			errx(1, "getaddrinfo: %s", gai_strerror(ecode));
		//memcpy(addr, res->ai_addr, sizeof(*addr));
		addr->sin_addr.s_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
		freeaddrinfo(res);
		break;
	case  1:
		break;
	}
}

void
servsetup(char *hostname, in_port_t port)
{
	struct sockaddr_in servaddr, cliaddr;
	socklen_t clilen;
	char ip[BUFSIZ];

	debug("Setting up server");

	parseaddr(hostname, port, &servaddr);

	inet_ntop(AF_INET, &servaddr.sin_addr, ip, sizeof ip);
	debug("Listening on (%d)%s:%u", servaddr.sin_family, ip,
	      ntohs(servaddr.sin_port));

	if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		err(1, "socket");
	if (bind(sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1)
		err(1, "bind");
	if (listen(sock, Q) == -1)
		err(1, "listen");
	if (accept(sock, (struct sockaddr *)&cliaddr, &clilen) == -1)
		err(1, "accept");
}

void
clisetup(char *hostname, in_port_t port)
{
	struct sockaddr_in addr;

	debug("Setting up client");

	parseaddr(hostname, port, &addr);

	debug("Creating socket");

	if ((sock = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		err(1, "socket");
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == -1)
		err(1, "connect");
}

void
mainloop(void)
{
	for (;;) {
		procexpected(MSGBOMB);
		sendbomb();
	}
}

#define cleandie(a)			\
	do {				\
		sendmessage(MSGQUIT);	\
		warn a;			\
		exit(1);		\
	} while (0)

#define cleandiex(a)			\
	do {				\
		sendmessage(MSGQUIT);	\
		warnx a;		\
		exit(1);		\
	} while (0)

void
fullread(int fd, void *buf, size_t nbytes)
{
	size_t bytesread, chunksiz;

	while (bytesread < nbytes) {
		if ((chunksiz = read(fd, buf + bytesread, nbytes - bytesread)) == -1)
			cleandie(("read"));
		bytesread += chunksiz;
	}
}

void
procexpected(char expected)
{
	char buf, *hashbuf;
	ssize_t nbytes;

	fullread(sock, &buf, 1);

	if (buf != expected) {
		sendmessage(MSGQUIT);
		errx(1, "received bad message (%c), quitting", buf);
	}

	switch (buf) {
		case MSGREADY:
			nbytes = 1 + (int)log10(USHRT_MAX);
			if ((hashbuf = malloc(nbytes + 1)) == NULL)
				cleandie(("malloc"));
			fullread(sock, hashbuf, nbytes);
			hashbuf[nbytes] = '\0';
			peerhash = atoi(hashbuf);
			break;
		case MSGBOMB:
			break;
		case MSGSTAT:
			break;
		case MSGQUIT:
			break;
		case MSGSUNK:
			break;
		default:
			cleandiex(("unknown message type; type: %c", buf));
			break;
	}
}

void
sendmessage(char type, ...)
{
	va_list ap;
	char buf[MAXMSGLEN], *msg, stat;
	row_t row;
	col_t col;
	hash_t hash;
	int len;

	va_start(ap, type);
	switch (type) {
	case MSGBOMB:
		row = va_arg(ap, int);
		col = va_arg(ap, int);
		snprintf(buf, sizeof(buf), "%c%d%d", type, row, col);
		break;

	case MSGREADY:
		hash = va_arg(ap, int);
		snprintf(buf, sizeof(buf), "%c%0*d", type,
			 1 + (int)log10(USHRT_MAX), hash);
		break;

	case MSGSTAT:
		stat = va_arg(ap, int);
		msg = va_arg(ap, char *);
		if ((len = strlen(msg)) > MAXUMSGLEN)
			len = MAXUMSGLEN;
		snprintf(buf, sizeof(buf), "%c%c%0*d%.*s", type, stat,
			 MAXUMSGNDIGITS, len, MAXUMSGLEN, msg);
		break;

	case MSGQUIT:
		snprintf(buf, sizeof(buf), "%c", type);
		break;

	case MSGSUNK:
		msg = va_arg(ap, char *);
		if ((len = strlen(msg)) > MAXUMSGLEN)
			len = MAXUMSGLEN;
		snprintf(buf, sizeof(buf), "%c%0*d%.*s", type,
			 MAXUMSGNDIGITS, len, MAXUMSGLEN, msg);
		break;
	}
	va_end(ap);

	write(sock, buf, strlen(buf));
}

hash_t
calchash(struct coord **vec)
{
	hash_t val;
	struct coord **p;

	for (p = vec; *p != NULL; p++)
		val = val * 100 + (*p)->row * 10 + (*p)->col;

	return val;
}

void
sendbomb(void)
{
	row_t row;
	col_t col;

	sendmessage(MSGBOMB, row, col);
}
