/* $Id$ */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <termios.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>
#include <err.h>
#include "fscbp.h"

#define PORT 6986
#define Q 1
#define NSHIPS 5
#define NCOLS 10
#define NROWS 10

#define cleandie(a)			\
	do {				\
		cleanup();		\
		warn a;			\
		exit(1);		\
	} while (0)

#define cleandiex(a)			\
	do {				\
		cleanup();		\
		warnx a;		\
		exit(1);		\
	} while (0)

#define   rawtty() tcsetattr(STDIN_FILENO, TCSANOW, &rawttystate)
#define unrawtty() tcsetattr(STDIN_FILENO, TCSANOW, &oldttystate)

#define OCEAN(i, j) ocean[(int)(i)][(int)(j)]

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
void sendready(void);
void mainloop(void);
void procexpected(char);
void sendmessage(char, ...);
void placeships(void);
void cleanup(void);
void setttystate(void);

row_t lastrow;
col_t lastcol;
hash_t peerhash;
int sock;
int verbose;
int ocean[NROWS][NCOLS];
struct termios oldttystate, rawttystate;

#define MARKBOMBED (1<<0)
#define MARKSHIP   (1<<1)
#define MARKRSHIP  (1<<2)
#define MARKHIT    (1<<3)

struct ship {
	struct coord start;
	struct coord end;
	int len;
	char *name;
} ships[NSHIPS] = {
	{{-1,-1}, {-1,-1}, 5, "Battlecruiser"},
	{{-1,-1}, {-1,-1}, 4, "Destroyer"},
	{{-1,-1}, {-1,-1}, 3, "Mayflower"},
	{{-1,-1}, {-1,-1}, 3, "Santa Marie"},
	{{-1,-1}, {-1,-1}, 2, "Voyager"},
};

int
getch(void)
{
	int ch;
	if ((ch = getchar()) == EOF)
		cleandiex((" "));
	return ch;
}

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

void sighandler(int sig)
{
	switch (sig) {
	case SIGINT:
		cleanup();
		exit(1);
		break;
	}
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

	tcgetattr(STDIN_FILENO, &oldttystate);
	memcpy(&rawttystate, &oldttystate, sizeof(rawttystate));
	cfmakeraw(&rawttystate);

	signal(SIGINT, sighandler);

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
	} else
		clisetup(hostname, port);

	debug("Starting main loop");

	placeships();
	sendready();
	procexpected(MSGREADY);

	/* "Server" peer goes first. */
	if (daemon)
		sendbomb();
	mainloop();

	return 0;
}

void
draw(void)
{
	int i, j;

	system("clear");
#if 0
	printf("\017");
	fflush(stdout);
#endif
	printf("Your ships\n");
	printf("    ");
	for (j = 0; j < NCOLS; j++)
		printf("%2d ", j + 1);
	printf("\n   +");
	for (j = 0; j < NCOLS*3; j++)
		printf("-");
	printf("\n");
	for (i = 0; i < NROWS; i++) {
		printf(" %c |", 'A' + i);
		for (j = 0; j < NCOLS; j++) {
			if (OCEAN(i, j) & MARKHIT) {
				if (OCEAN(i, j) & MARKSHIP)
					printf(" X ");
				else
					printf(" o ");
			} else {
				if (OCEAN(i, j) & MARKSHIP)
					printf(" # ");
				else
					printf("   ");
			}
		}
		printf("\n");
	}
	for (i = 0; i < 72; i++)
		printf("-");
	printf("\nTheir ships\n");
	printf("    ");
	for (j = 0; j < NCOLS; j++)
		printf("%2d ", j + 1);
	printf("\n   +");
	for (j = 0; j < NCOLS*3; j++)
		printf("-");
	printf("\n");
	for (i = 0; i < NROWS; i++) {
		printf(" %c |", 'A' + i);
		for (j = 0; j < NCOLS; j++) {
			if (OCEAN(i, j) & MARKBOMBED) {
				if (OCEAN(i, j) & MARKRSHIP)
					printf(" X ");
				else
					printf(" o ");
			} else {
				if (OCEAN(i, j) & MARKRSHIP)
					printf(" # ");
				else
					printf("   ");
			}
		}
		printf("\n");
	}
}

void
placeship(struct coord topleft, struct coord botright)
{
	int i;
	if (topleft.row == botright.row)
		for (i = topleft.col; i <= botright.col; i++)
			OCEAN(topleft.row, i) |= MARKSHIP;
	else
		for (i = topleft.row; i <= botright.row; i++)
			OCEAN(i, topleft.col) |= MARKSHIP;
}

void
unplaceship(struct coord topleft, struct coord botright)
{
	int i;
	if (topleft.row == botright.row)
		for (i = topleft.col; i <= botright.col; i++)
			OCEAN(topleft.row, i) &= ~MARKSHIP;
	else
		for (i = topleft.row; i <= botright.row; i++)
			OCEAN(i, topleft.col) &= ~MARKSHIP;
}

void
findlegal(struct coord *tl, struct coord *br, int len)
{
	row_t r;
	col_t c;
	int start;
	for (r = 0; r < NROWS; r++) {
		for (c = 0; c < NCOLS - (len - 1); c++) {
			for (start = c; start < c + len; start++)
				if (OCEAN(r, start) & MARKSHIP) {
					/* Will be incremented. */
					c = start;
					goto nextcol;
				}
			/* Success. */
			tl->row = r;
			br->row = r;
			tl->col = c;
			br->col = c + len - 1;
			return;
nextcol:
			;
		}
	}

	if (r >= NROWS || c >= NCOLS) {
		/*
		 * Try "sideways."
		 *
		 * (Actually, all we have to do is swap the
		 * arguments in the definition of OCEAN(). and
		 * swap them back in assignment.)
		 */
		for (c = 0; c < NCOLS; c++) {
			for (r = 0; r < NROWS - (len - 1); r++) {
				for (start = r; start < r + len; start++)
					if (OCEAN(start, c) & MARKSHIP) {
						/* Will be incremented. */
						r = start;
						goto nextrow;
					}
				/* Success. */
				tl->row = r;
				br->row = r + len - 1;
				tl->col = c;
				br->col = c;
				return;
nextrow:
				;
			}
		}
	}

	cleandiex(("Can't locate legal starting position"));
}

void
placeships(void)
{
	int i, ch, jump;
	row_t r;
	col_t c;
	struct coord topleft, botright;

	for (i = 0; i < NSHIPS; i++) {
		/* Find legal placement. */
		findlegal(&topleft, &botright, ships[i].len);

		for (;;) {
			placeship(topleft, botright);
			draw();
			unplaceship(topleft, botright);

			printf("Where will the %s go?\n", ships[i].name);
			rawtty();
			ch = getch();
			switch (ch) {
				case 3: /* ^C */
					kill(getpid(), SIGINT);
					break;
				case 32: /* Space */
					break;
				case 13: /* Enter */
					placeship(topleft, botright);
					unrawtty();
					goto placed;
					/* NOTREACHED */
				case 27: /* Esc */
					ch = getch();
					switch (ch) {
					case 91:
						ch = getch();
						switch (ch) {
						case 68: /* Left */
							if (topleft.col == 0)
								break;
							for (jump = 1; jump < topleft.col; jump++) {
								for (r = topleft.row; r <= botright.row; r++)
									for (c = topleft.col; c <= botright.col; c++)
										if (OCEAN(r, c - jump) & MARKSHIP) {
								//			jump += c - topleft.col;
											goto nextleft;
										}
								break;
nextleft:
								;
							}
							if (jump > topleft.col)
								break;
							topleft.col  -= jump;
							botright.col -= jump;
							break;
						case 67: /* Right */
							if (botright.col == NCOLS - 1)
								break;
							for (jump = 1; jump + botright.col < NCOLS; jump++) {
								for (r = topleft.row; r <= botright.row; r++)
									for (c = topleft.col; c <= botright.col; c++)
										if (OCEAN(r, c + jump) & MARKSHIP)
											goto nextright;
								break;
nextright:
								;
							}
							if (jump + botright.col >= NCOLS)
								break;
							topleft.col  += jump;
							botright.col += jump;
							break;
						case 65: /* Up */
							if (topleft.row == 0)
								break;
							for (jump = 1; jump < topleft.row; jump++) {
								for (c = topleft.col; c <= botright.col; c++)
									for (r = topleft.row; r <= botright.row; r++)
										if (OCEAN(r - jump, c) & MARKSHIP)
											goto nextup;
								break;
nextup:
								;
							}
							if (jump > topleft.row)
								break;
							topleft.row  -= jump;
							botright.row -= jump;
							break;
						case 66: /* Down */
							if (botright.row == NROWS - 1)
								break;
							for (jump = 1; jump + botright.row < NROWS; jump++) {
								for (c = topleft.col; c <= botright.col; c++)
									for (r = topleft.row; r <= botright.row; r++)
										if (OCEAN(r + jump, c) & MARKSHIP)
											goto nextdown;
								break;
nextdown:
								;
							}
							if (jump + botright.row >= NROWS)
								break;
							topleft.row  += jump;
							botright.row += jump;
							break;
						default:
							/* putchar(); x 2 */
							break;
						}
						break;
					default:
						/* putchar(c); */
						break;
					}
					break;
				case EOF:
					cleandiex((" "));
			}
			unrawtty();
		}
placed:
		;
	}
}

void
parseaddr(char *hostname, in_port_t port, void *buf)
{
	struct addrinfo hints, *res;
	struct sockaddr_in *addr = (struct sockaddr_in *)buf;
	size_t len;
	char *sport;
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
		hints.ai_family = AF_INET;
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
	debug("Listening on %s:%u", ip, ntohs(servaddr.sin_port));

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
		draw();
		sendbomb();
	}
}

void
fullread(int fd, void *buf, size_t nbytes)
{
	size_t bytesread, chunksiz;
printf("Waiting for %lu bytes\n", nbytes);
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
	row_t row;
	col_t col;

	fullread(sock, &buf, 1);

	if (buf != expected) {
		sendmessage(MSGQUIT);
		cleandiex(("received bad message (%c), quitting", buf));
	}

	switch (buf) {
		case MSGREADY:
#if 0
			nbytes = 1 + (int)log10(USHRT_MAX);
			if ((hashbuf = malloc(nbytes + 1)) == NULL)
				cleandie(("malloc"));
			fullread(sock, hashbuf, nbytes);
			hashbuf[nbytes] = '\0';
			peerhash = atoi(hashbuf);
#endif
			break;

		case MSGBOMB: {
			char msgbuf[2];
			fullread(sock, msgbuf, sizeof(msgbuf));

			buf = tolower(msgbuf[0]);
			if (buf < 'a' || buf > 'a' + NROWS - 1)
				cleandiex(("Received bad message"));
			row = buf - 'a';

			buf = msgbuf[1];
			if (buf < '0' || buf > '9')
				cleandiex(("Received bad message"));
			col = buf - '0';

			break;
		}

		case MSGSTAT: {
			char msgbuf[1 + MAXUMSGNDIGITS], *umsgbuf;
			size_t len;

			fullread(sock, msgbuf, sizeof(msgbuf));
			if (msgbuf[0] != MSGSTATHIT &&
			    msgbuf[0] != MSGSTATMISS)
				cleandiex(("Received bad message"));
			if (!isdigit(msgbuf[1]) || !isdigit(msgbuf[2]))
				cleandiex(("Received bad message"));
			len = msgbuf[1] * 10 + msgbuf[2];
			if ((umsgbuf = malloc(len + 1)) == NULL)
				cleandie(("malloc"));
			fullread(sock, umsgbuf, len);
			umsgbuf[len] = '\0';

			/* We hit a ship. */
			if (msgbuf[0] == MSGSTATHIT)
				OCEAN(lastrow, lastcol) |= MARKRSHIP;

			printf("Received message from user: %s", umsgbuf);
			break;
		}

		case MSGQUIT:
			close(sock);
			sock = 0;
			cleandiex(("Remote user quit"));
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
		snprintf(buf, sizeof(buf), "%c%c%c", type,
			 row + 'A', col + '0');
		break;

	case MSGREADY:
		snprintf(buf, sizeof(buf), "%c", type);
		break;
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
		snprintf(buf, sizeof(buf), "%c", type);
#if 0
		msg = va_arg(ap, char *);
		if ((len = strlen(msg)) > MAXUMSGLEN)
			len = MAXUMSGLEN;
		snprintf(buf, sizeof(buf), "%c%0*d%.*s", type,
			 MAXUMSGNDIGITS, len, MAXUMSGLEN, msg);
#endif
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
sendready(void)
{
	hash_t h;
#if 0
	h = calchash();
#endif
	sendmessage(MSGREADY, h);
}

void
sendbomb(void)
{
	int ch;
	row_t row;
	col_t col;

	draw();
	printf("Where would you like to send a bomb?\n");

	for (;;) {
		printf("Row: ");
		fflush(stdout);
		ch = tolower(getch());
		getch(); /* newline */

		if ('a' <= ch && ch <= ('a' + NROWS - 1))
			break;
		else
			printf("Invalid row\n");
	}
	row = ch - 'a';

	for (;;) {
		printf("Column: ");
		fflush(stdout);
		ch = getch();
		getch();

		if ('0' <= ch && ch <= ('0' + NCOLS -1))
			break;
		else
			printf("Invalid column\n");
	}
	col = ch - '0';

#if 0
	if (OCEAN(row, col) & MARKBOMBED)
		start over;
#endif

	sendmessage(MSGBOMB, row, col);
}

void
cleanup(void)
{
	if (sock)
		sendmessage(MSGQUIT);
	unrawtty();
}
