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

#define cleandie(a)		\
	do {			\
		cleanup();	\
		warn a;		\
		exit(1);	\
	} while (0)

#define cleandiex(a)		\
	do {			\
		cleanup();	\
		warnx a;	\
		exit(1);	\
	} while (0)

#define   rawtty() tcsetattr(STDIN_FILENO, TCSANOW, &rawttystate)
#define unrawtty() tcsetattr(STDIN_FILENO, TCSANOW, &oldttystate)
#define ungetchar(a) ungetc(a, stdin)

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
void draw(void);

row_t lastrow;
col_t lastcol;
hash_t peerhash;
int sock, verbose, ocean[NROWS][NCOLS];
struct termios oldttystate, rawttystate;

#define MARKBOMBED	(1 << 0)
#define MARKRBOMBED	(1 << 1)
#define MARKSHIP	(1 << 2)
#define MARKRSHIP	(1 << 3)

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
			/* NOTREACHED */
		case 'p':
			if ((l = strtoul(optarg, NULL, 10)) > 65535 ||
			    l == 0)
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
			if ((l = strtoul(pos + 1, NULL, 10)) > 65535 ||
			    l == 0)
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
	draw();
	printf("Waiting for remote user to place ships...\n");
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
	printf("Your ships/their bombs\n");
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
			if (OCEAN(i, j) & MARKRBOMBED) {
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
	printf("\nTheir ships/your bombs\n");
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

int
calcjump(int jump, int adj, int max, struct coord tl,
	 struct coord br, int hdir)
{
	row_t r;
	col_t c;

	for (; abs(jump) <= max; jump += adj) {
		for (r = tl.row; r <= br.row; r++)
			for (c = tl.col; c <= br.col; c++)
				if (OCEAN(hdir ? r : r + jump,
					  hdir ? c + jump : c) & MARKSHIP) {
					/* XXX: optimize */
					/* jump += c - topleft.col; */
					goto next;
				}
		/*
		 * There are no conflicts with the new location,
		 * so it is safe to jump there.
		 */
		return jump;
next:
		;
	}

	/* Couldn't be satisified; don't jump any. */
	return 0;
}

void
placeships(void)
{
	int i, ch, jump;
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
					goto procdir;
					/* NOTREACHED */
				default:
					/* ungetchar(c); */
					break;
				}
				break;
			}

			goto nextch;
procdir:
			/* Next character processing. */
			ch = getch();
			switch (ch) {
			case 68: /* Left */
				if (topleft.col == 0)
					break;
				jump = calcjump(-1, -1, topleft.col,
						topleft, botright, 1);
				topleft.col  += jump;
				botright.col += jump;
				break;

			case 67: /* Right */
				if (botright.col == NCOLS - 1)
					break;
				jump = calcjump(1, 1,
						NCOLS - botright.col - 1,
						topleft, botright, 1);
				topleft.col  += jump;
				botright.col += jump;
				break;

			case 65: /* Up */
				if (topleft.row == 0)
					break;
				jump = calcjump(-1, -1, topleft.row,
						topleft, botright, 0);
				topleft.row  += jump;
				botright.row += jump;
				break;
				
			case 66: /* Down */
				if (botright.row == NROWS - 1)
					break;
				jump = calcjump(1, 1,
						NROWS - botright.row - 1,
						topleft, botright, 0);
				topleft.row  += jump;
				botright.row += jump;
				break;

			default:
				/* ungetchar(); x 2 */
				break;
			}

nextch:
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
		/* memcpy(addr, res->ai_addr, sizeof(*addr)); */
		addr->sin_addr.s_addr = ((struct sockaddr_in *)res->
						ai_addr)->sin_addr.s_addr;
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
	if ((sock = accept(sock, (struct sockaddr *)&cliaddr, &clilen)) == -1)
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
debug("Entering main loop");
	for (;;) {
debug("Waiting for bomb");
		procexpected(MSGBOMB);
		draw();
		sendbomb();
	}
}

void
fullread(int fd, char *buf, size_t nbytes)
{
	size_t bytesread = 0, chunksiz;

debug("Trying to read %d bytes", nbytes);

	while (bytesread < nbytes) {
		if ((chunksiz = read(fd, buf + bytesread,
				     nbytes - bytesread)) == -1)
			cleandie(("read"));
		bytesread += chunksiz;
	}
}

const char *
getumsg(int hit)
{
	return hit ? "You hit me you scumbag" : "You missed scumbag";
}

void
procexpected(char expected)
{
	char buf, *hashbuf;
	ssize_t nbytes;
	row_t row;
	col_t col;

	debug("Expecting %c", expected);

	fullread(sock, &buf, 1);

	debug("Expected %c, received %c", expected, buf);

	if (buf != expected && buf != MSGQUIT) {
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
			char msgbuf[3];
			int hit;

			fullread(sock, msgbuf, sizeof(msgbuf));

			buf = tolower(msgbuf[0]);
			if (buf < 'a' || buf > 'a' + NROWS - 1)
				cleandiex(("Received bad message"));
			row = buf - 'a';

			if (!(isdigit(msgbuf[1]) || msgbuf[1] == ' ') ||
			    !isdigit(msgbuf[2]))
				cleandiex(("Received bad message"));
			col = (msgbuf[1] == ' ' ? 0 : (msgbuf[1] - '0') * 10) +
			      msgbuf[2] - '0' - 1;

			OCEAN(row, col) |= MARKRBOMBED;
			hit = OCEAN(row, col) & MARKSHIP;

			sendmessage(MSGSTAT, hit, getumsg(hit));

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
			len = (msgbuf[1] - '0') * 10 + (msgbuf[2] - '0');
			if ((umsgbuf = malloc(len + 1)) == NULL)
				cleandie(("malloc"));

			debug("received status [hit: %c; len: %c%c -> %d]",
				msgbuf[0], msgbuf[1], msgbuf[2], msgbuf[1],
				len);

			fullread(sock, umsgbuf, len);
			umsgbuf[len] = '\0';

			/* We hit a ship. */
			if (msgbuf[0] == MSGSTATHIT)
				OCEAN(lastrow, lastcol) |= MARKRSHIP;

			draw();
			printf("Received message from user: %s\n", umsgbuf);
			printf("Waiting for remote user to bomb.\n");
			break;
		}

		case MSGQUIT:
			close(sock);
			sock = 0;
			cleandiex(("Remote user quit"));
			break;

		case MSGSUNK:
			break;

		case MSGEND:

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
	char buf[MAXMSGLEN];

	va_start(ap, type);
	switch (type) {
	case MSGBOMB:
#if 0
		row = va_arg(ap, int);
		col = va_arg(ap, int);
#endif
		snprintf(buf, sizeof(buf), "%c%c%2d", type,
			 lastrow + 'A', lastcol + 1);
		break;

	case MSGREADY: {
#ifdef HASH
		hash_t hash;

		hash = va_arg(ap, int);
		snprintf(buf, sizeof(buf), "%c%0*d", type,
			 1 + (int)log10(USHRT_MAX), hash);
#else
		snprintf(buf, sizeof(buf), "%c", type);
#endif
		break;
	}

	case MSGSTAT: {
		char stat, *msg;
		int len;

		stat = va_arg(ap, int);
		msg = va_arg(ap, char *);
		if ((len = strlen(msg)) > MAXUMSGLEN)
			len = MAXUMSGLEN;
		snprintf(buf, sizeof(buf), "%c%c%0*d%.*s", type, stat,
			 MAXUMSGNDIGITS, len, MAXUMSGLEN, msg);
		break;
	}

	case MSGQUIT:
		snprintf(buf, sizeof(buf), "%c", type);
		break;

	case MSGSUNK: {
		char *msg;
		int len;

		msg = va_arg(ap, char *);
		if ((len = strlen(msg)) > MAXUMSGLEN)
			len = MAXUMSGLEN;
		snprintf(buf, sizeof(buf), "%c%0*d%.*s", type,
			 MAXUMSGNDIGITS, len, MAXUMSGLEN, msg);
		break;
	}

	case MSGEND: {
#ifdef HASH
		int i, j;
		char coord[4]; /* row, 2-digit col, NUL */

		for (i = 0; i < NROWS; i++)
			for (j = 0; j < NCOLS; j++)
				if (OCEAN(i, j) & MARKSHIP) {
					snprintf(coord, sizeof(coord),
						 "%c%02d", i + 'A', j + '1');
					if (strlcat(buf, coord, sizeof(buf)) <
					    strlen(coord))
						cleandiex(("can't copy coordinates"));
				}
#else
		snprintf(buf, sizeof(buf), "%c", type);
#endif
		break;
	}
	}
	va_end(ap);

	debug("sending [%s]", buf);

	write(sock, buf, strlen(buf));
}

hash_t
calchash(struct coord *p, size_t n)
{
	hash_t val;

	for (; --n; p++)
		val = (val * 100 + p->row * 10 + p->col) % USHRT_MAX;

	return val;
}

int
coordcmp(const void *x, const void *y)
{
	struct coord *a, *b;

	a = (struct coord *)x;
	b = (struct coord *)y;

	if (a->row == b->row) {
		if (a->col == b->col)
			return 0;
		else
			return a->col < b->col ? -1 : 1;
	} else
		return a->row < b->row ? -1 : 1;
}

void
sendready(void)
{
	hash_t h;
	struct coord *coords, *cp;
	int ncoords = 0, i, j;

	for (i = 0; i < NSHIPS; i++)
		ncoords += ships[i].len;

	if ((coords = malloc(sizeof(struct coord) * ncoords)) == NULL)
		cleandie(("malloc"));

	cp = coords;
	for (i = 0; i < NROWS; i++)
		for (j = 0; j < NCOLS; j++)
			if (OCEAN(i, j) & MARKSHIP) {
				cp->row = i;
				cp->col = j;
				if (++cp - coords > ncoords)
					goto skipships;
			}
skipships:

	qsort(coords, cp - coords, sizeof(struct coord), coordcmp);
	h = calchash(coords, cp - coords);
	debug("%hu", h);
	sleep(2);
	sendmessage(MSGREADY, h);
}

void
sendbomb(void)
{
	int ch, nextch;

	draw();
	printf("Where would you like to send a bomb?\n");

	for (;;) {
		printf("Row: ");
		fflush(stdout);
		ch = tolower(getch());

		if ('a' <= ch && ch <= ('a' + NROWS - 1)) {
			while (getch() != '\n')
				;
			break;
		}

		printf("Invalid row\n");
		if (ch != '\n')
			while (getch() != '\n')
				;
	}
	lastrow = ch - 'a';

	for (;;) {
		printf("Column: ");
		fflush(stdout);
		ch = getch();

		if ('1' <= ch && ch <= '9') {
			if (ch == '1') {
				nextch = getch();
				if (nextch == '0')
					ch += 9;
				else
					ungetchar(nextch);
			}
			while (getch() != '\n')
				;
			break;
		}

		printf("Invalid column\n");
		if (ch != '\n')
			while (getch() != '\n')
				;
	}
	lastcol = ch - '1';

	OCEAN(lastrow, lastcol) |= MARKBOMBED;

#if 0
	if (OCEAN(row, col) & MARKBOMBED)
		start over;
#endif

	sendmessage(MSGBOMB);
	procexpected(MSGSTAT);
}

void
cleanup(void)
{
	if (sock)
		sendmessage(MSGQUIT);
	unrawtty();
}
