/* $Id$ */

#ifndef FSCBP_H
#define FSCBP_H

#define MAXMSGLEN 128
#define MAXUMSGNDIGITS 2
#define MAXUMSGLEN 100

#define MSGREADY	'r'
#define MSGBOMB		'b'
#define MSGSTAT		's'
#define MSGQUIT		'q'
#define MSGSUNK		'e'

#define MSGSTATHIT	'h'
#define MSGSTATMISS	'm'

struct fsc_generic {
	char type;
	char buf[MAXMSGLEN];
};

struct fsc_ready {
	char type;
};

struct fsc_bomb {
	char type;
	char row, col;
};

struct fsc_stat {
	char type;
	char status;
	int len:MAXUMSGNDIGITS;
	char msg[MAXUMSGLEN];
};

struct fsc_quit {
	char type;
};

struct fsc_sunk {
	char type;
};

#endif /* FSCBP_H */
