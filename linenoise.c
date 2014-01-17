/* clir.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 * 
 *   http://github.com/antirez/clir
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2013, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * CHA (Cursor Horizontal Absolute)
 *    Sequence: ESC [ n G
 *    Effect: moves cursor to column n
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (Cursor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward of n chars
*
* When multi line mode is enabled, we also use an additional escape
* sequence. However multi line editing is disabled by default.
	*
* CUU (Cursor Up)
	*    Sequence: ESC [ n A
	*    Effect: moves cursor up of n chars.
	*
* CUD (Cursor Down)
	*    Sequence: ESC [ n B
	*    Effect: moves cursor down of n chars.
	*
	* The following are used to clear the screen: ESC [ H ESC [ 2 J
	* This is actually composed of two sequences:
	*
	* cursorhome
	*    Sequence: ESC [ H
	*    Effect: moves the cursor to upper left corner
	*
* ED2 (Clear entire screen)
	*    Sequence: ESC [ 2 J
	*    Effect: clear the whole screen
	* 
	*/

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include "clir.h"

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
#define LINENOISE_MAX_COMMAND_LEN 128
	static char *unsupported_term[] = {"dumb","cons25",NULL};
static clirCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
char **history = NULL;

/* The clirState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct clirState {
	int fd;             /* Terminal file descriptor. */
	char *buf;          /* Edited line buffer. */
	size_t buflen;      /* Edited line buffer size. */
	const char *prompt; /* Prompt to display. */
	size_t plen;        /* Prompt length. */
	size_t pos;         /* Current cursor position. */
	size_t oldpos;      /* Previous refresh cursor position. */
	size_t len;         /* Current edited line length. */
	size_t cols;        /* Number of columns in terminal. */
	size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
	int history_index;  /* The history index we are currently editing. */
	size_t word_pos;    /* Position of the last word's first character. */
};

static void clirAtExit(void);
int clirHistoryAdd(const char *line);
static void refreshLine(struct clirState *cs);

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void clirSetMultiLine(int ml) {
	mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int isUnsupportedTerm(void) {
	char *term = getenv("TERM");
	int j;

	if (term == NULL) return 0;
	for (j = 0; unsupported_term[j]; j++)
		if (!strcasecmp(term,unsupported_term[j])) return 1;
	return 0;
}

/* Raw mode: 1960 magic shit. */
static int enableRawMode(int fd) {
	struct termios raw;

	if (!isatty(STDIN_FILENO)) goto fatal;
	if (!atexit_registered) {
		atexit(clirAtExit);
		atexit_registered = 1;
	}
	if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

	raw = orig_termios;  /* modify the original mode */
	/* input modes: no break, no CR to NL, no parity check, no strip char,
	 * no start/stop output control. */
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	/* output modes - disable post processing */
	raw.c_oflag &= ~(OPOST);
	/* control modes - set 8 bit chars */
	raw.c_cflag |= (CS8);
	/* local modes - choing off, canonical off, no extended functions,
	 * no signal chars (^Z,^C) */
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	/* control chars - set return condition: min number of bytes and timer.
	 * We want read to return every single byte, without timeout. */
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

	/* put terminal in raw mode after flushing */
	if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
	rawmode = 1;
	return 0;

fatal:
	errno = ENOTTY;
	return -1;
}

static void disableRawMode(int fd) {
	/* Don't even check the return value as it's too late. */
	if (rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
		rawmode = 0;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int getColumns(void) {
	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) return 80;
	return ws.ws_col;
}

/* Clear the screen. Used to handle ctrl+cs */
void clirClearScreen(void) {
	if (write(STDIN_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
		/* nothing to do, just to avoid warning. */
	}
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void clirBeep(void) {
	fprintf(stderr, "\x7");
	fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by clirAddCompletion(). */
static void freeCompletions(clirCompletions *cc) {
	size_t i;
	for (i = 0; i < cc->len; i++)
		free(cc->cvec[i]);
	if (cc->cvec != NULL)
		free(cc->cvec);
}

static int completeWord(struct clirState *cs) {
	clirCompletions cc = { 0, NULL };
	size_t pos = cs->word_pos;

	completionCallback(cs->buf, &cc); // Add completions

	int valid[cc.len];
	size_t valid_c = 0;
	size_t valid_p = 0;
	size_t valid_i;
	for (valid_i = 0; valid_i < cc.len; valid_i++) { valid[valid_i] = 1; }

	if (cc.len == 0) {
		clirBeep();
	} else {
		size_t comp_i, char_i;
		for (comp_i = 0; comp_i < cc.len; comp_i++) { // 'foreach' completion
			for (char_i = 0, pos = cs->word_pos; cc.cvec[comp_i][char_i] != '\0' && cs->word_pos + char_i < cs->pos; char_i++, pos++) {
				if (cc.cvec[comp_i][char_i] != cs->buf[pos]) {
					valid[comp_i] = 0;
				}
			}
			if (valid[comp_i] == 1) {
				valid_p = cs->pos;
				valid_i = comp_i;
				valid_c++;
			}
			//printf(" '%s'=%i", cc.cvec[comp_i], valid[comp_i]);
		}
		if (valid_c == 1) {
			//printf(" %zu %zu", valid_i, valid_p);
			while (cc.cvec[valid_i][valid_p] != '\0') {
				clirEditInsert(cs, cc.cvec[valid_i][valid_p]);
				valid_p++;
			}
			clirEditInsert(cs, ' ');
		}
		else if (valid_c > 1) {
			//clirEditInsert(cs, '\n');
			printf("\r\n");
			for (comp_i = 0; comp_i < cc.len; comp_i++) {
				if (valid[comp_i] == 1) {
					printf(" '%s'", cc.cvec[comp_i]);
				}
			}
			printf("\r\n");
			//clirEditInsert(cs, '\n');
			return 1;
		}
	}
	return 0;
}

/* This is an helper function for clirEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 * 
 * The state of the editing is encapsulated into the pointed clirState
 * structure as described in the structure definition. */
static int completeLine(struct clirState *cs) {
	clirCompletions cc = { 0, NULL };
	int nread, nwritten;
	char c = 0;

	completionCallback(cs->buf, &cc);
	if (cc.len == 0) {
		clirBeep();
	} else {
		size_t stop = 0, i = 0;

		while (!stop) {
			/* Show completion or original buffer */
			if (i < cc.len) {
				struct clirState saved = *cs;

				cs->len = cs->pos = strlen(cc.cvec[i]);
				cs->buf = cc.cvec[i];
				refreshLine(cs);
				cs->len = saved.len;
				cs->pos = saved.pos;
				cs->buf = saved.buf;
			} else {
				refreshLine(cs);
			}

			nread = read(cs->fd, &c, 1);
			if (nread <= 0) {
				freeCompletions(&cc);
				return -1;
			}

			switch(c) {
				case 9: /* tab */
					i = (i+1) % (cc.len+1);
					if (i == cc.len) clirBeep();
					break;
				case 27: /* escape */
					/* Re-show original buffer */
					if (i < cc.len) refreshLine(cs);
					stop = 1;
					break;
				default:
					/* Update buffer and return */
					if (i < cc.len) {
						nwritten = snprintf(cs->buf, cs->buflen, "%s", cc.cvec[i]);
						cs->len = cs->pos = nwritten;
					}
					stop = 1;
					break;
			}
		}
	}

	freeCompletions(&cc);
	return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void clirSetCompletionCallback(clirCompletionCallback *fn) {
	completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void clirAddCompletion(clirCompletions *cc, char *str) {
	size_t len = strlen(str);
	char *copy = malloc(len+1);
	memcpy(copy, str, len+1);
	cc->cvec = realloc(cc->cvec,sizeof(char*)*(cc->len+1));
	cc->cvec[cc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshSingleLine(struct clirState *cs) {
	char seq[64];
	size_t plen = strlen(cs->prompt);
	int fd = cs->fd;
	char *buf = cs->buf;
	size_t len = cs->len;
	size_t pos = cs->pos;

	while((plen+pos) >= cs->cols) {
		buf++;
		len--;
		pos--;
	}
	while (plen+len > cs->cols) {
		len--;
	}

	/* Cursor to left edge */
	snprintf(seq, 64, "\x1b[0G");
	if (write(fd,seq,strlen(seq)) == -1) return;
	/* Write the prompt and the current buffer content */
	if (write(fd,cs->prompt,strlen(cs->prompt)) == -1) return;
	if (write(fd,buf,len) == -1) return;
	/* Erase to right */
	snprintf(seq, 64, "\x1b[0K");
	if (write(fd,seq,strlen(seq)) == -1) return;
	/* Move cursor to original position. */
	snprintf(seq, 64, "\x1b[0G\x1b[%dC", (int)(pos+plen));
	if (write(fd,seq,strlen(seq)) == -1) return;
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void refreshMultiLine(struct clirState *cs) {
	char seq[64];
	int plen = strlen(cs->prompt);
	int rows = (plen+cs->len+cs->cols-1)/cs->cols; /* rows used by current buf. */
	int rpos = (plen+cs->oldpos+cs->cols)/cs->cols; /* cursor relative row. */
	int rpos2; /* rpos after refresh. */
	int old_rows = cs->maxrows;
	int fd = cs->fd, j;

	/* Update maxrows if needed. */
	if (rows > (int)cs->maxrows) cs->maxrows = rows;

#ifdef LN_DEBUG
	FILE *fp = fopen("/tmp/debug.txt","a");
	fprintf(fp,"[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d",
			(int)cs->len,(int)cs->pos,(int)cs->oldpos,plen,rows,rpos,(int)cs->maxrows,old_rows);
#endif

	/* First step: clear all the lines used before. To do so start by
	 * going to the last row. */
	if (old_rows-rpos > 0) {
#ifdef LN_DEBUG
		fprintf(fp,", go down %d", old_rows-rpos);
#endif
		snprintf(seq,64,"\x1b[%dB", old_rows-rpos);
		if (write(fd,seq,strlen(seq)) == -1) return;
	}

	/* Now for every row clear it, go up. */
	for (j = 0; j < old_rows-1; j++) {
#ifdef LN_DEBUG
		fprintf(fp,", clear+up");
#endif
		snprintf(seq,64,"\x1b[0G\x1b[0K\x1b[1A");
		if (write(fd,seq,strlen(seq)) == -1) return;
	}

	/* Clean the top line. */
#ifdef LN_DEBUG
	fprintf(fp,", clear");
#endif
	snprintf(seq,64,"\x1b[0G\x1b[0K");
	if (write(fd,seq,strlen(seq)) == -1) return;

	/* Write the prompt and the current buffer content */
	if (write(fd,cs->prompt,strlen(cs->prompt)) == -1) return;
	if (write(fd,cs->buf,cs->len) == -1) return;

	/* If we are at the very end of the screen with our prompt, we need to
	 * emit a newline and move the prompt to the first column. */
	if (cs->pos &&
			cs->pos == cs->len &&
			(cs->pos+plen) % cs->cols == 0)
	{
#ifdef LN_DEBUG
		fprintf(fp,", <newline>");
#endif
		if (write(fd,"\n",1) == -1) return;
		snprintf(seq,64,"\x1b[0G");
		if (write(fd,seq,strlen(seq)) == -1) return;
		rows++;
		if (rows > (int)cs->maxrows) cs->maxrows = rows;
	}

	/* Move cursor to right position. */
	rpos2 = (plen+cs->pos+cs->cols)/cs->cols; /* current cursor relative row. */
#ifdef LN_DEBUG
	fprintf(fp,", rpos2 %d", rpos2);
#endif
	/* Go up till we reach the expected positon. */
	if (rows-rpos2 > 0) {
#ifdef LN_DEBUG
		fprintf(fp,", go-up %d", rows-rpos2);
#endif
		snprintf(seq,64,"\x1b[%dA", rows-rpos2);
		if (write(fd,seq,strlen(seq)) == -1) return;
	}
	/* Set column. */
#ifdef LN_DEBUG
	fprintf(fp,", set col %d", 1+((plen+(int)cs->pos) % (int)cs->cols));
#endif
	snprintf(seq,64,"\x1b[%dG", 1+((plen+(int)cs->pos) % (int)cs->cols));
	if (write(fd,seq,strlen(seq)) == -1) return;

	cs->oldpos = cs->pos;

#ifdef LN_DEBUG
	fprintf(fp,"\n");
	fclose(fp);
#endif
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void refreshLine(struct clirState *cs) {
	if (mlmode)
		refreshMultiLine(cs);
	else
		refreshSingleLine(cs);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int clirEditInsert(struct clirState *cs, int c) {
	if (cs->len < cs->buflen) {
		if (cs->len == cs->pos) {
			cs->buf[cs->pos] = c;
			cs->pos++;
			cs->len++;
			cs->buf[cs->len] = '\0';
			if ((!mlmode && cs->plen+cs->len < cs->cols) /* || mlmode */) {
				/* Avoid a full update of the line in the
				 * trivial case. */
				if (write(cs->fd,&c,1) == -1) return -1;
			} else {
				refreshLine(cs);
			}
		} else {
			memmove(cs->buf+cs->pos+1,cs->buf+cs->pos,cs->len-cs->pos);
			cs->buf[cs->pos] = c;
			cs->len++;
			cs->pos++;
			cs->buf[cs->len] = '\0';
			refreshLine(cs);
		}
	}
	return 0;
}

/* Move cursor on the left. */
void clirEditMoveLeft(struct clirState *cs) {
	if (cs->pos > 0) {
		cs->pos--;
		refreshLine(cs);
	}
}

/* Move cursor on the right. */
void clirEditMoveRight(struct clirState *cs) {
	if (cs->pos != cs->len) {
		cs->pos++;
		refreshLine(cs);
	}
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void clirEditHistoryNext(struct clirState *cs, int dir) {
	if (history_len > 1) {
		/* Update the current history entry before to
		 * overwrite it with the next one. */
		free(history[history_len - 1 - cs->history_index]);
		history[history_len - 1 - cs->history_index] = strdup(cs->buf);
		/* Show the new entry */
		cs->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
		if (cs->history_index < 0) {
			cs->history_index = 0;
			return;
		} else if (cs->history_index >= history_len) {
			cs->history_index = history_len-1;
			return;
		}
		strncpy(cs->buf,history[history_len - 1 - cs->history_index],cs->buflen);
		cs->buf[cs->buflen-1] = '\0';
		cs->len = cs->pos = strlen(cs->buf);
		refreshLine(cs);
	}
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void clirEditDelete(struct clirState *cs) {
	if (cs->len > 0 && cs->pos < cs->len) {
		memmove(cs->buf+cs->pos,cs->buf+cs->pos+1,cs->len-cs->pos-1);
		cs->len--;
		cs->buf[cs->len] = '\0';
		refreshLine(cs);
	}
}

/* Backspace implementation. */
void clirEditBackspace(struct clirState *cs) {
	if (cs->pos > 0 && cs->len > 0) {
		memmove(cs->buf+cs->pos-1,cs->buf+cs->pos,cs->len-cs->pos);
		cs->pos--;
		cs->len--;
		cs->buf[cs->len] = '\0';
		refreshLine(cs);
	}
}

/* Delete the previosu word, maintaining the cursor at the start of the
 * current word. */
void clirEditDeletePrevWord(struct clirState *cs) {
	size_t old_pos = cs->pos;
	size_t diff;

	while (cs->pos > 0 && cs->buf[cs->pos-1] == ' ')
		cs->pos--;
	while (cs->pos > 0 && cs->buf[cs->pos-1] != ' ')
		cs->pos--;
	diff = old_pos - cs->pos;
	memmove(cs->buf+cs->pos,cs->buf+old_pos,cs->len-old_pos+1);
	cs->len -= diff;
	refreshLine(cs);
}

/* This function is the core of the line editing capability of clir.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int clirEdit(int fd, char *buf, size_t buflen, const char *prompt)
{
	struct clirState cs;

	/* Populate the clir state that we pass to functions implementing
	 * specific editing functionalities. */
	cs.fd = fd;
	cs.buf = buf;
	cs.buflen = buflen;
	cs.prompt = prompt;
	cs.plen = strlen(prompt);
	cs.oldpos = cs.pos = 0;
	cs.len = 0;
	cs.cols = getColumns();
	cs.maxrows = 0;
	cs.history_index = 0;
	cs.word_pos = 0;

	/* Buffer starts empty. */
	buf[0] = '\0';
	buflen--; /* Make sure there is always space for the nulterm */

	/* The latest history entry is always our current buffer, that
	 * initially is just an empty string. */
	clirHistoryAdd("");

	if (write(fd,prompt,cs.plen) == -1) return -1;
	while(1) {
		char c;
		int nread;
		char seq[2], seq2[2];

		nread = read(fd, &c, 1);
		if (nread <= 0) return cs.len;

		/* Only autocomplete when the callback is set. It returns < 0 when
		 * there was an error reading from fd. Otherwise it will return the
		 * character that should be handled next. */
		if (c == 9 && completionCallback != NULL) { /* tab key */

			//c = completeLine(&cs);
			c = completeWord(&cs);

			/* Return on errors */
			if (c < 0) return (int)cs.len;
			/* Read next character when 0 */
			if (c == 0) continue;
			/* List all the completions */
			if (c > 0) {
				refreshLine(&cs);
				continue;
			}
		}

		switch(c) {
			case 13:    /* enter */
				history_len--;
				free(history[history_len]);
				return (int)cs.len;
			case 3:     /* ctrl-c */
				errno = EAGAIN;
				return -1;
			case 127:   /* backspace */
			case 8:     /* ctrl-h */
				clirEditBackspace(&cs);
				break;
			case 4:     /* ctrl-d, remove char at right of cursor, or of the
										 line is empty, act as end-of-file. */
				if (cs.len > 0) {
					clirEditDelete(&cs);
				} else {
					history_len--;
					free(history[history_len]);
					return -1;
				}
				break;
			case 20:    /* ctrl-t, swaps current character with previous. */
				if (cs.pos > 0 && cs.pos < cs.len) {
					int aux = buf[cs.pos-1];
					buf[cs.pos-1] = buf[cs.pos];
					buf[cs.pos] = aux;
					if (cs.pos != cs.len-1) cs.pos++;
					refreshLine(&cs);
				}
				break;
			case 2:     /* ctrl-b */
				clirEditMoveLeft(&cs);
				break;
			case 6:     /* ctrl-f */
				clirEditMoveRight(&cs);
				break;
			case 16:    /* ctrl-p */
				clirEditHistoryNext(&cs, LINENOISE_HISTORY_PREV);
				break;
			case 14:    /* ctrl-n */
				clirEditHistoryNext(&cs, LINENOISE_HISTORY_NEXT);
				break;
			case 27:    /* escape sequence */
				/* Read the next two bytes representing the escape sequence. */
				if (read(fd,seq,2) == -1) break;

				if (seq[0] == 91) {
					switch(seq[1]) {
						case 65: /* up arrow */
							clirEditHistoryNext(&cs,LINENOISE_HISTORY_PREV);
							break;
						case 66: /* down arrow */
							clirEditHistoryNext(&cs,LINENOISE_HISTORY_NEXT);
							break;
						case 67: /* right arrow */
							clirEditMoveRight(&cs);
							break;
						case 68: /* left arrow */
							clirEditMoveLeft(&cs);
							break;
						case 90: /* shift tab */
							// todo: complete line from history
							break;
						default:
							if (seq[1] > 48 && seq[1] < 55) {
								/* Read the next two bytes continuing the escape sequence. */
								if (read(fd,seq2,2) == -1) break;

								if (seq[1] == 51 && seq2[0] == 126) {
									/* delete key. */
									clirEditDelete(&cs);
								} else if (seq[1] == 49 && seq2[0] == 59) { // modifier key
									char seq3[1];
									if (read(fd,seq3,1) == -1) break;

									if (seq2[1] == 53) { // ctrl key
										if (seq3[0] == 67) {
											while (!isspace(buf[cs.pos]) && cs.pos < cs.len) {
												cs.pos++;
											}
											while (isspace(buf[cs.pos]) && cs.pos <= cs.len) {
												cs.pos++;
											}
											refreshLine(&cs);
										} else if (seq3[0] == 68) {
											if (cs.pos > 0) { cs.pos--; }
											while (isspace(buf[cs.pos]) && cs.pos > 0) {
												cs.pos--;
											}
											while (!isspace(buf[cs.pos-1]) && cs.pos > 0) {
												cs.pos--;
											}
											refreshLine(&cs);
										}
									}
								}
							}
							break;
					}
				} else if (seq[0] == 79 && seq[1] == 72) {
					/* home button */
					if (cs.pos > 0) {
						cs.pos = 0;
						refreshLine(&cs);
					}
				} else if (seq[0] == 79 && seq[1] == 70) {
					/* end button */
					if (cs.pos != cs.len) {
						cs.pos = cs.len;
						refreshLine(&cs);
					}
				}
				break;
			case 21: /* ctrl-u, delete the whole line. */
				buf[0] = '\0';
				cs.pos = cs.len = 0;
				refreshLine(&cs);
				break;
			case 11: /* ctrl-k, delete from current to end of line. */
				buf[cs.pos] = '\0';
				cs.len = cs.pos;
				refreshLine(&cs);
				break;
			case 1:  /* ctrl-a, go to the start of the line */
				cs.pos = 0;
				refreshLine(&cs);
				break;
			case 5:  /* ctrl-e, go to the end of the line */
				cs.pos = cs.len;
				refreshLine(&cs);
				break;
			case 12: /* ctrl-l, clear screen */
				clirClearScreen();
				refreshLine(&cs);
				break;
			case 23: /* ctrl-w, delete previous word */
				clirEditDeletePrevWord(&cs);
				break;
			default:
				if (clirEditInsert(&cs, c)) return -1;
				if (c == ' ') cs.word_pos = cs.pos;
				break;
		}
	}
	return cs.len;
}

/* This function calls the line editing function clirEdit() using
 * the STDIN file descriptor set in raw mode. */
static int clirRaw(char *buf, size_t buflen, const char *prompt) {
	int fd = STDIN_FILENO;
	int count;

	if (buflen == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!isatty(STDIN_FILENO)) {
		if (fgets(buf, buflen, stdin) == NULL) return -1;
		count = strlen(buf);
		if (count && buf[count-1] == '\n') {
			count--;
			buf[count] = '\0';
		}
	} else {
		if (enableRawMode(fd) == -1) return -1;
		count = clirEdit(fd, buf, buflen, prompt);
		disableRawMode(fd);
		printf("\n");
	}
	return count;
}

/* The high level function that is the main API of the clir library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *clir(const char *prompt) {
	char buf[LINENOISE_MAX_LINE];
	int count;

	if (isUnsupportedTerm()) {
		size_t len;

		printf("%s",prompt);
		fflush(stdout);
		if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
		len = strlen(buf);
		while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
			len--;
			buf[len] = '\0';
		}
		return strdup(buf);
	} else {
		count = clirRaw(buf,LINENOISE_MAX_LINE,prompt);
		if (count == -1) return NULL;
		return strdup(buf);
	}
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void freeHistory(void) {
	if (history) {
		int j;

		for (j = 0; j < history_len; j++)
			free(history[j]);
		free(history);
	}
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void clirAtExit(void) {
	disableRawMode(STDIN_FILENO);
	freeHistory();
}

/* Using a circular buffer is smarter, but a bit more complex to handle. */
int clirHistoryAdd(const char *line) {
	char *linecopy;

	if (history_max_len == 0) return 0;
	if (history == NULL) {
		history = malloc(sizeof(char*)*history_max_len);
		if (history == NULL) return 0;
		memset(history,0,(sizeof(char*)*history_max_len));
	}
	linecopy = strdup(line);
	if (!linecopy) return 0;
	if (history_len == history_max_len) {
		free(history[0]);
		memmove(history,history+1,sizeof(char*)*(history_max_len-1));
		history_len--;
	}
	if (history_len && !strcmp(history[history_len-1], line)) return 0;
	history[history_len] = linecopy;
	history_len++;
	return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int clirHistorySetMaxLen(int len) {
	char **new;

	if (len < 1) return 0;
	if (history) {
		int tocopy = history_len;

		new = malloc(sizeof(char*)*len);
		if (new == NULL) return 0;

		/* If we can't copy everything, free the elements we'll not use. */
		if (len < tocopy) {
			int j;

			for (j = 0; j < tocopy-len; j++) free(history[j]);
			tocopy = len;
		}
		memset(new,0,sizeof(char*)*len);
		memcpy(new,history+(history_len-tocopy), sizeof(char*)*tocopy);
		free(history);
		history = new;
	}
	history_max_len = len;
	if (history_len > history_max_len)
		history_len = history_max_len;
	return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int clirHistorySave(char *filename) {
	FILE *fp = fopen(filename,"w");
	int j;

	if (fp == NULL) return -1;
	for (j = 0; j < history_len; j++)
		fprintf(fp,"%s\n",history[j]);
	fclose(fp);
	return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int clirHistoryLoad(char *filename) {
	FILE *fp = fopen(filename,"r");
	char buf[LINENOISE_MAX_LINE];

	if (fp == NULL) return -1;

	while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
		char *p;

		p = strchr(buf,'\r');
		if (!p) p = strchr(buf,'\n');
		if (p) *p = '\0';
		clirHistoryAdd(buf);
	}
	fclose(fp);
	return 0;
}
