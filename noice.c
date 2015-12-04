/* See LICENSE file for copyright and license details. */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <curses.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#ifdef DEBUG
#define DEBUG_FD 8
#define DPRINTF_D(x) dprintf(DEBUG_FD, #x "=%d\n", x)
#define DPRINTF_U(x) dprintf(DEBUG_FD, #x "=%u\n", x)
#define DPRINTF_S(x) dprintf(DEBUG_FD, #x "=%s\n", x)
#define DPRINTF_P(x) dprintf(DEBUG_FD, #x "=0x%p\n", x)
#else
#define DPRINTF_D(x)
#define DPRINTF_U(x)
#define DPRINTF_S(x)
#define DPRINTF_P(x)
#endif /* DEBUG */

#define LEN(x) (sizeof(x) / sizeof(*(x)))
#undef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define ISODD(x) ((x) & 1)
#define CONTROL(c) ((c) ^ 0x40)
#define SHIFT(c) ((c) ^ 0x10)

struct assoc {
	char *regex; /* Regex to match on filename */
	char *bin;   /* Program */
};

/* Supported actions */
enum action {
	SEL_QUIT = 1,
	SEL_BACK,
	SEL_GOIN,
	SEL_FLTR,
	SEL_TYPE,
	SEL_NEXT,
	SEL_PREV,
	SEL_PGDN,
	SEL_PGUP,
	SEL_HOME,
	SEL_END,
	SEL_CD,
	SEL_CDHOME,
	SEL_MTIME,
	SEL_REDRAW,
	SEL_RUN,
	SEL_RUNARG,
	SEL_TOGGLEDOT,
};

struct key {
	int sym;         /* Key pressed */
	enum action act; /* Action */
	char *run;       /* Program to run */
	char *env;       /* Environment variable to run */
	char *args;	 /* Extra program arguments */
};

#include "config.h"

struct entry {
	char *name;
	mode_t mode;
	time_t t;
	unsigned long size;
};

/* Global context */
struct entry *dents;
int n, cur;
char *path, *oldpath;
char *fltr;
int idle;
unsigned long totalsize;

/*
 * Layout:
 * .---------
 * | cwd: /mnt/path
 * |
 * |    file0
 * |    file1
 * |  > file2
 * |    file3
 * |    file4
 *      ...
 * |    filen
 * |
 * | Permission denied
 * '------
 */

void printmsg(char *);
void printwarn(void);
void printerr(int, char *);
char *mkpath(char *, char *);
char *printsize(unsigned long size);
char filemode(mode_t mod);

#undef dprintf
int
dprintf(int fd, const char *fmt, ...)
{
	char buf[BUFSIZ];
	int r;
	va_list ap;

	va_start(ap, fmt);
	r = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (r > 0)
		write(fd, buf, r);
	va_end(ap);
	return r;
}

void *
xmalloc(size_t size)
{
	void *p;

	p = malloc(size);
	if (p == NULL)
		printerr(1, "malloc");
	return p;
}

void *
xrealloc(void *p, size_t size)
{
	p = realloc(p, size);
	if (p == NULL)
		printerr(1, "realloc");
	return p;
}

char *
xstrdup(const char *s)
{
	char *p;

	p = strdup(s);
	if (p == NULL)
		printerr(1, "strdup");
	return p;
}

char *
xdirname(const char *path)
{
	char *p, *tmp;

	/* Some implementations of dirname(3) may modify `path' and some
	 * return a pointer inside `path' and we cannot free(3) the
	 * original string if we lose track of it. */
	tmp = xstrdup(path);
	p = dirname(tmp);
	if (p == NULL) {
		free(tmp);
		printerr(1, "dirname");
	}
	/* Make sure this is a malloc(3)-ed string */
	p = xstrdup(p);
	free(tmp);
	return p;
}

void
spawn(const char *file, const char *arg, const char *dir, const char *args)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == 0) {
		if (dir != NULL)
			chdir(dir);
		if (args != NULL)
			execlp(file, file, args, arg, NULL);
		else
			execlp(file, file, arg, NULL);
		_exit(1);
	} else {
		/* Ignore interruptions */
		while (waitpid(pid, &status, 0) == -1)
			DPRINTF_D(status);
		DPRINTF_D(pid);
	}
}

char *
xgetenv(char *name, char *fallback)
{
	char *value;

	if (name == NULL)
		return fallback;
	value = getenv(name);
	return value && value[0] ? value : fallback;
}

char *
openwith(char *file)
{
	regex_t regex;
	char *bin = NULL;
	int i;

	for (i = 0; i < LEN(assocs); i++) {
		if (regcomp(&regex, assocs[i].regex,
			    REG_NOSUB | REG_EXTENDED | REG_ICASE) != 0)
			continue;
		if (regexec(&regex, file, 0, NULL, 0) == 0) {
			bin = assocs[i].bin;
			break;
		}
	}
	DPRINTF_S(bin);

	return bin;
}

int
setfilter(regex_t *regex, char *filter)
{
	char *errbuf;
	int r;

	r = regcomp(regex, filter, REG_NOSUB | REG_EXTENDED | REG_ICASE);
	if (r != 0) {
		errbuf = xmalloc(COLS * sizeof(char));
		regerror(r, regex, errbuf, COLS * sizeof(char));
		printmsg(errbuf);
		free(errbuf);
	}

	return r;
}

int
visible(regex_t *regex, char *file)
{
	return regexec(regex, file, 0, NULL, 0) == 0;
}

int
entrycmp(const void *va, const void *vb)
{
	const struct entry *a, *b;

	a = (struct entry *)va;
	b = (struct entry *)vb;

	if (mtimeorder)
		return b->t - a->t;
	return strcmp(a->name, b->name);
}

void
initcurses(void)
{
	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	curs_set(FALSE); /* Hide cursor */
	timeout(1000); /* One second */
}

void
exitcurses(void)
{
	endwin(); /* Restore terminal */
}

/* Messages show up at the bottom */
void
printmsg(char *msg)
{
	move(LINES - 1, 0);
	printw("%s\n", msg);
}

/* Display warning as a message */
void
printwarn(void)
{
	printmsg(strerror(errno));
}

/* Kill curses and display error before exiting */
void
printerr(int ret, char *prefix)
{
	exitcurses();
	fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
	exit(ret);
}

/* Return the size to print in human readable form */
char *printsize(unsigned long size)
{
	float printsize = (float)size;
	char *strsize;
	int lcount = 0;
	char units[5] = {'B','K','M','G','T'};
	
	while (printsize > 1024 && lcount < 4) {
		printsize = printsize / 1024;
		lcount++;
	}

	strsize = malloc(15);
	sprintf(strsize, "%12.3f%c", printsize, units[lcount]);
	return(strsize);
}

/* Clear the last line */
void
clearprompt(void)
{
	printmsg("");
}

/* Print prompt on the last line */
void
printprompt(char *str)
{
	clearprompt();
	printw(str);
}

/* Returns SEL_* if key is bound and 0 otherwise
   Also modifies the run and env pointers (used on SEL_{RUN,RUNARG}) */
int
nextsel(char **run, char **env, char **args)
{
	int c, i;

	c = getch();
	if (c == -1)
		idle++;
	else
		idle = 0;

	for (i = 0; i < LEN(bindings); i++)
		if (c == bindings[i].sym) {
			*run = bindings[i].run;
			*env = bindings[i].env;
			*args = bindings[i].args;
			return bindings[i].act;
		}

	return 0;
}

char *
readln(void)
{
	char ln[LINE_MAX];

	timeout(-1);
	echo();
	curs_set(TRUE);
	memset(ln, 0, sizeof(ln));
	wgetnstr(stdscr, ln, sizeof(ln) - 1);
	noecho();
	curs_set(FALSE);
	timeout(1000);
	return ln[0] ? strdup(ln) : NULL;
}

/*
 * Read one key and modify the provided string accordingly.
 * Returns 0 when more input is expected and 1 on completion.
 */
int
readmore(char **str)
{
	int c, ret = 0;
	int i;
	char *ln = *str;

	timeout(-1);
	if (ln != NULL)
		i = strlen(ln);
	else
		i = 0;
	DPRINTF_D(i);

	curs_set(TRUE);

	c = getch();
	switch (c) {
	case KEY_ENTER:
	case '\r':
		ret = 1;
		break;
	case KEY_BACKSPACE:
	case CONTROL('H'):
		i--;
		if (i > 0) {
			ln = xrealloc(ln, (i + 1) * sizeof(*ln));
			ln[i] = '\0';
		} else {
			free(ln);
			ln = NULL;
		}
		break;
	default:
		i++;
		ln = xrealloc(ln, (i + 1) * sizeof(*ln));
		ln[i - 1] = c;
		ln[i] = '\0';
	}

	curs_set(FALSE);

	*str = ln;
	timeout(1000);

	return ret;
}

int
canopendir(char *path)
{
	DIR *dirp;

	dirp = opendir(path);
	if (dirp == NULL)
		return 0;
	closedir(dirp);
	return 1;
}

char filemode(mode_t mod)
{
	char cm=0;

	if (S_ISDIR(mod)) {
		cm = '/';
	} else if (S_ISLNK(mod)) {
		cm = '@';
	} else if (S_ISSOCK(mod)) {
		cm = '=';
	} else if (S_ISFIFO(mod)) {
		cm = '|';
	} else if (mod & S_IXUSR) {
		cm = '*';
	}

	return cm;
}

void
printent(struct entry *ent, int active)
{
	char *name, *size;
	unsigned int maxlen = COLS - strlen(CURSR) - 17;
	char cm = 0;
	int row, col;

	getyx(stdscr, row, col);

	/* Copy name locally */
	name = xstrdup(ent->name);

	if ((cm = filemode(ent->mode)) != 0)
		maxlen--;

	/* No text wrapping in entries */
	if (strlen(name) > maxlen)
		name[maxlen] = '\0';

	if (cm == 0)
		mvprintw(row, 0, "%s%s", active ? CURSR : EMPTY, name);
	else
		mvprintw(row, 0, "%s%s%c", active ? CURSR : EMPTY, name, cm);

	if (cm == 0 || cm == '*')
	{
		size = printsize(ent->size);
		mvprintw(row, COLS-16, "%s\n", size);
		free(size);
	}
	else
		printw("\n");

	free(name);
}

int
dentfill(char *path, struct entry **dents,
	 int (*filter)(regex_t *, char *), regex_t *re)
{
	DIR *dirp;
	struct dirent *dp;
	struct stat sb;
	char *newpath;
	int r, n = 0;

	totalsize = 0;
	dirp = opendir(path);
	if (dirp == NULL)
		return 0;

	while ((dp = readdir(dirp)) != NULL) {
		/* Skip self and parent */
		if (strcmp(dp->d_name, ".") == 0
		    || strcmp(dp->d_name, "..") == 0)
			continue;
		if (filter(re, dp->d_name) == 0)
			continue;
		*dents = xrealloc(*dents, (n + 1) * sizeof(**dents));
		(*dents)[n].name = xstrdup(dp->d_name);
		/* Get mode flags */
		newpath = mkpath(path, dp->d_name);
		r = lstat(newpath, &sb);
		if (r == -1)
			printerr(1, "lstat");
		(*dents)[n].mode = sb.st_mode;
		(*dents)[n].t = sb.st_mtime;
		(*dents)[n].size = sb.st_size;

		if (filemode(sb.st_mode) == 0 | filemode(sb.st_mode) == '*')
			totalsize += sb.st_size;
		n++;
	}

	/* Should never be null */
	r = closedir(dirp);
	if (r == -1)
		printerr(1, "closedir");

	return n;
}

void
dentfree(struct entry *dents, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(dents[i].name);
	free(dents);
}

char *
mkpath(char *dir, char *name)
{
	char path[PATH_MAX];

	/* Handle absolute path */
	if (name[0] == '/') {
		strlcpy(path, name, sizeof(path));
	} else {
		/* Handle root case */
		if (strcmp(dir, "/") == 0) {
			strlcpy(path, "/", sizeof(path));
			strlcat(path, name, sizeof(path));
		} else {
			strlcpy(path, dir, sizeof(path));
			strlcat(path, "/", sizeof(path));
			strlcat(path, name, sizeof(path));
		}
	}
	return xstrdup(path);
}

/* Return the position of the matching entry or 0 otherwise */
int
dentfind(struct entry *dents, int n, char *cwd, char *path)
{
	int i;
	char *tmp;

	if (path == NULL)
		return 0;

	for (i = 0; i < n; i++) {
		tmp = mkpath(cwd, dents[i].name);
		DPRINTF_S(path);
		DPRINTF_S(tmp);
		if (strcmp(tmp, path) == 0) {
			free(tmp);
			return i;
		}
		free(tmp);
	}

	return 0;
}

int
populate(void)
{
	regex_t re;
	int r;

	/* Can fail when permissions change while browsing */
	if (canopendir(path) == 0)
		return -1;

	/* Search filter */
	r = setfilter(&re, fltr);
	if (r != 0)
		return -1;

	dentfree(dents, n);

	n = 0;
	dents = NULL;

	n = dentfill(path, &dents, visible, &re);

	qsort(dents, n, sizeof(*dents), entrycmp);

	/* Find cur from history */
	cur = dentfind(dents, n, path, oldpath);
	free(oldpath);
	oldpath = NULL;

	return 0;
}

void
redraw(void)
{
	int nlines, odd;
	char *cwd;
	int i;

	nlines = MIN(LINES - 4, n);

	/* Clean screen */
	erase();

	/* Strip trailing slashes */
	for (i = strlen(path) - 1; i > 0; i--)
		if (path[i] == '/')
			path[i] = '\0';
		else
			break;

	DPRINTF_D(cur);
	DPRINTF_S(path);

	/* No text wrapping in cwd line */
	cwd = xmalloc(COLS * sizeof(char));
	strlcpy(cwd, path, COLS * sizeof(char));
	cwd[COLS - strlen(CWD) - 1] = '\0';

	printw(CWD "%s", cwd);
	mvprintw(0, COLS-16, "%s\n\n", printsize(totalsize));

	/* Print listing */
	odd = ISODD(nlines);
	if (cur < nlines / 2) {
		for (i = 0; i < nlines; i++)
			printent(&dents[i], i == cur);
	} else if (cur >= n - nlines / 2) {
		for (i = n - nlines; i < n; i++)
			printent(&dents[i], i == cur);
	} else {
		for (i = cur - nlines / 2;
		     i < cur + nlines / 2 + odd; i++)
			printent(&dents[i], i == cur);
	}
}

void
browse(const char *ipath, const char *ifilter)
{
	int r, fd;
	regex_t re;
	char *newpath;
	struct stat sb;
	char *name, *bin, *dir, *tmp, *run, *env, *args;
	int nowtyping = 0;

	oldpath = NULL;
	path = xstrdup(ipath);
	fltr = xstrdup(ifilter);
begin:
	/* Path and filter should be malloc(3)-ed strings at all times */
	r = populate();
	if (r == -1) {
		if (!nowtyping) {
			printwarn();
			goto nochange;
		}
	}

	for (;;) {
		redraw();

		/* Handle filter-as-you-type mode */
		if (nowtyping)
			goto moretyping;
nochange:
		switch (nextsel(&run, &env, &args)) {
		case SEL_QUIT:
			free(path);
			free(fltr);
			dentfree(dents, n);
			return;
		case SEL_BACK:
			/* There is no going back */
			if (strcmp(path, "/") == 0 ||
			    strcmp(path, ".") == 0 ||
			    strchr(path, '/') == NULL)
				goto nochange;
			dir = xdirname(path);
			if (canopendir(dir) == 0) {
				free(dir);
				printwarn();
				goto nochange;
			}
			/* Save history */
			oldpath = path;
			path = dir;
			/* Reset filter */
			free(fltr);
			fltr = xstrdup(ifilter);
			goto begin;
		case SEL_GOIN:
			/* Cannot descend in empty directories */
			if (n == 0)
				goto nochange;

			name = dents[cur].name;
			newpath = mkpath(path, name);
			DPRINTF_S(newpath);

			/* Get path info */
			fd = open(newpath, O_RDONLY | O_NONBLOCK);
			if (fd == -1) {
				printwarn();
				free(newpath);
				goto nochange;
			}
			r = fstat(fd, &sb);
			if (r == -1) {
				printwarn();
				close(fd);
				free(newpath);
				goto nochange;
			}
			close(fd);
			DPRINTF_U(sb.st_mode);

			switch (sb.st_mode & S_IFMT) {
			case S_IFDIR:
				if (canopendir(newpath) == 0) {
					printwarn();
					free(newpath);
					goto nochange;
				}
				free(path);
				path = newpath;
				/* Reset filter */
				free(fltr);
				fltr = xstrdup(ifilter);
				goto begin;
			case S_IFREG:
				bin = openwith(newpath);
				if (bin == NULL) {
					printmsg("No association");
					free(newpath);
					goto nochange;
				}
				exitcurses();
				spawn(bin, newpath, NULL, NULL);
				initcurses();
				free(newpath);
				continue;
			default:
				printmsg("Unsupported file");
				goto nochange;
			}
		case SEL_FLTR:
			/* Read filter */
			printprompt("filter: ");
			tmp = readln();
			if (tmp == NULL)
				tmp = xstrdup(ifilter);
			/* Check and report regex errors */
			r = setfilter(&re, tmp);
			if (r != 0) {
				free(tmp);
				goto nochange;
			}
			free(fltr);
			fltr = tmp;
			DPRINTF_S(fltr);
			/* Save current */
			if (n > 0)
				oldpath = mkpath(path, dents[cur].name);
			goto begin;
		case SEL_TYPE:
			nowtyping = 1;
			tmp = NULL;
moretyping:
			printprompt("type: ");
			if (tmp != NULL)
				printw("%s", tmp);
			r = readmore(&tmp);
			DPRINTF_D(r);
			DPRINTF_S(tmp);
			if (r == 1)
				nowtyping = 0;
			/* Check regex errors */
			if (tmp != NULL) {
				r = setfilter(&re, tmp);
				if (r != 0)
					if (nowtyping) {
						goto moretyping;
					} else {
						free(tmp);
						goto nochange;
					}
			}
			/* Copy or reset filter */
			free(fltr);
			if (tmp != NULL)
				fltr = xstrdup(tmp);
			else
				fltr = xstrdup(ifilter);
			/* Save current */
			if (n > 0)
				oldpath = mkpath(path, dents[cur].name);
			if (!nowtyping)
				free(tmp);
			goto begin;
		case SEL_NEXT:
			if (cur < n - 1)
				cur++;
			break;
		case SEL_PREV:
			if (cur > 0)
				cur--;
			break;
		case SEL_PGDN:
			if (cur < n - 1)
				cur += MIN((LINES - 4) / 2, n - 1 - cur);
			break;
		case SEL_PGUP:
			if (cur > 0)
				cur -= MIN((LINES - 4) / 2, cur);
			break;
		case SEL_HOME:
			cur = 0;
			break;
		case SEL_END:
			cur = n - 1;
			break;
		case SEL_CD:
			/* Read target dir */
			printprompt("chdir: ");
			tmp = readln();
			if (tmp == NULL) {
				clearprompt();
				goto nochange;
			}
			newpath = mkpath(path, tmp);
			free(tmp);
			if (canopendir(newpath) == 0) {
				free(newpath);
				printwarn();
				goto nochange;
			}
			free(path);
			path = newpath;
			free(fltr);
			fltr = xstrdup(ifilter); /* Reset filter */
			DPRINTF_S(path);
			goto begin;
		case SEL_CDHOME:
			tmp = getenv("HOME");
			if (tmp == NULL) {
				clearprompt();
				goto nochange;
			}
			newpath = mkpath(path, tmp);
			if (canopendir(newpath) == 0) {
				free(newpath);
				printwarn();
				goto nochange;
			}
			free(oldpath);
			oldpath = path;
			path = newpath;
			free(fltr);
			fltr = xstrdup(ifilter); /* Reset filter */
			DPRINTF_S(path);
			goto begin;	
		case SEL_MTIME:
			mtimeorder = !mtimeorder;
			/* Save current */
			if (n > 0)
				oldpath = mkpath(path, dents[cur].name);
			goto begin;
		case SEL_REDRAW:
			/* Save current */
			if (n > 0)
				oldpath = mkpath(path, dents[cur].name);
			goto begin;
		case SEL_RUN:
			run = xgetenv(env, run);
			exitcurses();
			spawn(run, NULL, path, args);
			initcurses();
			break;
		case SEL_RUNARG:
			name = dents[cur].name;
			run = xgetenv(env, run);
			exitcurses();
			spawn(run, name, path, args);
			initcurses();
			break;
		case SEL_TOGGLEDOT:
			if (strcmp(fltr, ifilter) != 0) {
				free(fltr);
				fltr = xstrdup(ifilter); /* Reset filter */
			} else {
				free(fltr);
				fltr = xstrdup(".");
			}
			goto begin;
		}
		/* Screensaver */
		if (idletimeout != 0 && idle == idletimeout) {
			idle = 0;
			exitcurses();
			spawn(idlecmd, NULL, NULL, NULL);
			initcurses();
		}
	}
}

void
usage(char *argv0)
{
	fprintf(stderr, "usage: %s [dir]\n", argv0);
	exit(1);
}

int
main(int argc, char *argv[])
{
	char cwd[PATH_MAX], *ipath;
	char *ifilter;

	if (argc > 2)
		usage(argv[0]);
#ifdef DEBUG
	fprintf(stderr, "Debugging on\n");
#endif


	/* Confirm we are in a terminal */
	if (!isatty(0) || !isatty(1)) {
		fprintf(stderr, "stdin or stdout is not a tty\n");
		exit(1);
	}

	if (getuid() == 0)
		ifilter = ".";
	else
		ifilter = "^[^.]"; /* Hide dotfiles */

	if (argv[1] != NULL) {
		ipath = argv[1];
	} else {
		ipath = getcwd(cwd, sizeof(cwd));
		if (ipath == NULL)
			ipath = "/";
	}

	signal(SIGINT, SIG_IGN);

	/* Test initial path */
	if (canopendir(ipath) == 0) {
		fprintf(stderr, "%s: %s\n", ipath, strerror(errno));
		exit(1);
	}

	/* Set locale before curses setup */
	setlocale(LC_ALL, "");

	initcurses();

	browse(ipath, ifilter);

	exitcurses();

	exit(0);
}
