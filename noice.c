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
#include <time.h>

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
#define TOUPPER(ch) \
	(((ch) >= 'a' && (ch) <= 'z') ? ((ch) - 'a' + 'A') : (ch))
#define MAX_LEN 1024
#define cur(flag) (flag ? CURSR : EMPTY)

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
	SEL_NEXT,
	SEL_PREV,
	SEL_PGDN,
	SEL_PGUP,
	SEL_HOME,
	SEL_END,
	SEL_CD,
	SEL_CDHOME,
	SEL_TOGGLEDOT,
	SEL_DETAIL,
	SEL_FSIZE,
	SEL_MTIME,
	SEL_REDRAW,
	SEL_RUN,
	SEL_RUNARG,
};

struct key {
	int sym;         /* Key pressed */
	enum action act; /* Action */
	char *run;       /* Program to run */
	char *env;       /* Environment variable to run */
};

#include "config.h"

struct entry {
	char name[PATH_MAX];
	mode_t mode;
	time_t t;
	off_t size;
};

/* Global context */
struct entry *dents;
int ndents, cur;
int idle;
char *opener = NULL;
char *fallback_opener = NULL;
char size_buf[12]; /* Buffer to hold human readable size */
const char* size_units[] = {"B", "K", "M", "G", "T", "P", "E", "Z", "Y"};

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

void (*printptr)(struct entry *ent, int active);
void printmsg(char *);
void printwarn(void);
void printerr(int, char *);

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
		r = write(fd, buf, r);
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

/* Some implementations of dirname(3) may modify `path' and some
 * return a pointer inside `path'. */
char *
xdirname(const char *path)
{
	static char out[PATH_MAX];
	char tmp[PATH_MAX], *p;

	strlcpy(tmp, path, sizeof(tmp));
	p = dirname(tmp);
	if (p == NULL)
		printerr(1, "dirname");
	strlcpy(out, p, sizeof(out));
	return out;
}

void
spawn(char *file, char *arg, char *dir)
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid == 0) {
		if (dir != NULL)
			status = chdir(dir);
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

int
xstricmp(const char *s1, const char *s2)
{
	while (*s2 != 0 && TOUPPER(*s1) == TOUPPER(*s2))
		s1++, s2++;

	/* In case of alphabetically same names, make sure
	   lower case one comes before upper case one */
	if (!*s1 && !*s2)
		return 1;
	return (int) (TOUPPER(*s1) - TOUPPER(*s2));
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
	char errbuf[LINE_MAX];
	size_t len;
	int r;

	r = regcomp(regex, filter, REG_NOSUB | REG_EXTENDED | REG_ICASE);
	if (r != 0) {
		len = COLS;
		if (len > sizeof(errbuf))
			len = sizeof(errbuf);
		regerror(r, regex, errbuf, len);
		printmsg(errbuf);
	}
	return r;
}

void
initfilter(int dot, char **ifilter)
{
	*ifilter = dot ? "." : "^[^.]";
}

int
visible(regex_t *regex, char *file)
{
	return regexec(regex, file, 0, NULL, 0) == 0;
}

int
entrycmp(const void *va, const void *vb)
{
	if (mtimeorder)
		return ((struct entry *)vb)->t - ((struct entry *)va)->t;

	if (sizeorder)
		return ((struct entry *)vb)->size - ((struct entry *)va)->size;

	return xstricmp(((struct entry *)va)->name, ((struct entry *)vb)->name);
}

void
initcurses(void)
{
	if (initscr() == NULL) {
		char *term = getenv("TERM");
		if (term != NULL)
			fprintf(stderr, "error opening terminal: %s\n", term);
		else
			fprintf(stderr, "failed to initialize curses\n");
		exit(1);
	}
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

/* Returns SEL_* if key is bound and 0 otherwise.
 * Also modifies the run and env pointers (used on SEL_{RUN,RUNARG}) */
int
nextsel(char **run, char **env)
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
			return bindings[i].act;
		}
	return 0;
}

char *
readln(void)
{
	static char ln[LINE_MAX];

	timeout(-1);
	echo();
	curs_set(TRUE);
	memset(ln, 0, sizeof(ln));
	wgetnstr(stdscr, ln, sizeof(ln) - 1);
	noecho();
	curs_set(FALSE);
	timeout(1000);
	return ln[0] ? ln : NULL;
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

char *
mkpath(char *dir, char *name, char *out, size_t n)
{
	/* Handle absolute path */
	if (name[0] == '/')
		strlcpy(out, name, n);
	else {
		/* Handle root case */
		if (strcmp(dir, "/") == 0)
			snprintf(out, n, "/%s", name);
		else
			snprintf(out, n, "%s/%s", dir, name);
	}
	return out;
}

void
printent(struct entry *ent, int active)
{
	if (S_ISDIR(ent->mode))
		printw("%s%s/\n", active ? CURSR : EMPTY, ent->name);
	else if (S_ISLNK(ent->mode))
		printw("%s%s@\n", active ? CURSR : EMPTY, ent->name);
	else if (S_ISSOCK(ent->mode))
		printw("%s%s=\n", active ? CURSR : EMPTY, ent->name);
	else if (S_ISFIFO(ent->mode))
		printw("%s%s|\n", active ? CURSR : EMPTY, ent->name);
	else if (ent->mode & S_IXUSR)
		printw("%s%s*\n", active ? CURSR : EMPTY, ent->name);
	else
		printw("%s%s\n", active ? CURSR : EMPTY, ent->name);
}

char*
coolsize(off_t size)
{
	int i = 0;
	long double fsize = (double)size;

	while (fsize > 1024) {
		fsize /= 1024;
		i++;
	}

	snprintf(size_buf, 12, "%.*Lf%s", i, fsize, size_units[i]);
	return size_buf;
}

void
printent_long(struct entry *ent, int active)
{
	static char buf[18];
	const static struct tm *p;
	static char name[PATH_MAX + 2];

	p = localtime(&ent->t);
	strftime(buf, 18, "%b %d %H:%M %Y", p);

	if (active)
		attron(A_REVERSE);

	if (S_ISDIR(ent->mode)) {
		sprintf(name, "%s/", ent->name);
		printw("%s%-32.32s   %-18.18s\n", cur(active), name, buf);
	} else if (S_ISLNK(ent->mode)) {
		sprintf(name, "%s@", ent->name);
		printw("%s%-32.32s   %-18.18s\n", cur(active), name, buf);
	} else if (S_ISSOCK(ent->mode)) {
		sprintf(name, "%s=", ent->name);
		printw("%s%-32.32s   %-18.18s\n", cur(active), name, buf);
	} else if (S_ISFIFO(ent->mode)) {
		sprintf(name, "%s|", ent->name);
		printw("%s%-32.32s   %-18.18s\n", cur(active), name, buf);
	} else if (S_ISBLK(ent->mode))
		printw("%s%-32.32s b %-18.18s\n", cur(active), ent->name, buf);
	else if (S_ISCHR(ent->mode))
		printw("%s%-32.32s c %-18.18s\n", cur(active), ent->name, buf);
	else if (ent->mode & S_IXUSR) {
		sprintf(name, "%s*", ent->name);
		printw("%s%-32.32s   %-18.18s %s\n", cur(active), name,
		       buf, coolsize(ent->size));
	} else
		printw("%s%-32.32s   %-18.18s %s\n", cur(active), ent->name,
		       buf, coolsize(ent->size));

	if (active)
		attroff(A_REVERSE);
}

int
dentfill(char *path, struct entry **dents,
	 int (*filter)(regex_t *, char *), regex_t *re)
{
	char newpath[PATH_MAX];
	DIR *dirp;
	struct dirent *dp;
	struct stat sb;
	int r, n = 0;

	dirp = opendir(path);
	if (dirp == NULL)
		return 0;

	while ((dp = readdir(dirp)) != NULL) {
		/* Skip self and parent */
		if (strcmp(dp->d_name, ".") == 0 ||
		    strcmp(dp->d_name, "..") == 0)
			continue;
		if (filter(re, dp->d_name) == 0)
			continue;
		*dents = xrealloc(*dents, (n + 1) * sizeof(**dents));
		strlcpy((*dents)[n].name, dp->d_name, sizeof((*dents)[n].name));
		/* Get mode flags */
		mkpath(path, dp->d_name, newpath, sizeof(newpath));
		r = lstat(newpath, &sb);
		if (r == -1)
			printerr(1, "lstat");
		(*dents)[n].mode = sb.st_mode;
		(*dents)[n].t = sb.st_mtime;
		(*dents)[n].size = sb.st_size;
		n++;
	}

	/* Should never be null */
	r = closedir(dirp);
	if (r == -1)
		printerr(1, "closedir");
	return n;
}

void
dentfree(struct entry *dents)
{
	free(dents);
}

/* Return the position of the matching entry or 0 otherwise */
int
dentfind(struct entry *dents, int n, char *cwd, char *path)
{
	char tmp[PATH_MAX];
	int i;

	if (path == NULL)
		return 0;
	for (i = 0; i < n; i++) {
		mkpath(cwd, dents[i].name, tmp, sizeof(tmp));
		DPRINTF_S(path);
		DPRINTF_S(tmp);
		if (strcmp(tmp, path) == 0)
			return i;
	}
	return 0;
}

int
populate(char *path, char *oldpath, char *fltr)
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

	dentfree(dents);

	ndents = 0;
	dents = NULL;

	ndents = dentfill(path, &dents, visible, &re);

	qsort(dents, ndents, sizeof(*dents), entrycmp);

	/* Find cur from history */
	cur = dentfind(dents, ndents, path, oldpath);
	return 0;
}

void
redraw(char *path)
{
	char cwd[PATH_MAX], cwdresolved[PATH_MAX];
	size_t ncols;
	int nlines, odd;
	int i;

	nlines = MIN(LINES - 4, ndents);

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
	ncols = COLS;
	if (ncols > PATH_MAX)
		ncols = PATH_MAX;
	strlcpy(cwd, path, ncols);
	cwd[ncols - strlen(CWD) - 1] = '\0';
	if (!realpath(path, cwdresolved)) {
		printmsg("Cannot resolve path");
		return;
	}

	printw(CWD "%s\n\n", cwdresolved);

	/* Print listing */
	odd = ISODD(nlines);
	if (cur < (nlines >> 1)) {
		for (i = 0; i < nlines; i++)
			printptr(&dents[i], i == cur);
	} else if (cur >= ndents - (nlines >> 1)) {
		for (i = ndents - nlines; i < ndents; i++)
			printptr(&dents[i], i == cur);
	} else {
		nlines >>= 1;
		for (i = cur - nlines; i < cur + nlines + odd; i++)
			printptr(&dents[i], i == cur);
	}

	if (showdetail) {
		if (ndents) {
			sprintf(cwd, "%d items [%s]", ndents, dents[cur].name);
			printmsg(cwd);
		} else
			printmsg("0 items");
	}
}

void
browse(char *ipath, char *ifilter)
{
	char path[PATH_MAX], oldpath[PATH_MAX], newpath[PATH_MAX];
	char fltr[LINE_MAX];
	char *bin, *dir, *tmp, *run, *env;
	struct stat sb;
	regex_t re;
	int r, fd;

	strlcpy(path, ipath, sizeof(path));
	strlcpy(fltr, ifilter, sizeof(fltr));
	oldpath[0] = '\0';
begin:
	r = populate(path, oldpath, fltr);
	if (r == -1) {
		printwarn();
		goto nochange;
	}

	for (;;) {
		redraw(path);
nochange:
		switch (nextsel(&run, &env)) {
		case SEL_QUIT:
			dentfree(dents);
			return;
		case SEL_BACK:
			/* There is no going back */
			if (strcmp(path, "/") == 0 ||
			    strcmp(path, ".") == 0 ||
			    strchr(path, '/') == NULL)
				goto nochange;
			dir = xdirname(path);
			if (canopendir(dir) == 0) {
				printwarn();
				goto nochange;
			}
			/* Save history */
			strlcpy(oldpath, path, sizeof(oldpath));
			strlcpy(path, dir, sizeof(path));
			/* Reset filter */
			strlcpy(fltr, ifilter, sizeof(fltr));
			goto begin;
		case SEL_GOIN:
			/* Cannot descend in empty directories */
			if (ndents == 0)
				goto nochange;

			mkpath(path, dents[cur].name, newpath, sizeof(newpath));
			DPRINTF_S(newpath);

			/* Get path info */
			fd = open(newpath, O_RDONLY | O_NONBLOCK);
			if (fd == -1) {
				printwarn();
				goto nochange;
			}
			r = fstat(fd, &sb);
			if (r == -1) {
				printwarn();
				close(fd);
				goto nochange;
			}
			close(fd);
			DPRINTF_U(sb.st_mode);

			switch (sb.st_mode & S_IFMT) {
			case S_IFDIR:
				if (canopendir(newpath) == 0) {
					printwarn();
					goto nochange;
				}
				strlcpy(path, newpath, sizeof(path));
				/* Reset filter */
				strlcpy(fltr, ifilter, sizeof(fltr));
				goto begin;
			case S_IFREG:
				/* If default mime opener is set, use it */
				if (opener) {
					char cmd[MAX_LEN];
					int status;

					snprintf(cmd, MAX_LEN, "%s \"%s\" > /dev/null 2>&1",
						opener, newpath);
					status = system(cmd);
					continue;
				}

				/* Try custom applications */
				bin = openwith(newpath);
				char *execvim = "vim";

				if (bin == NULL) {
					/* If a custom handler application is not set, open
					   plain text files with vim, then try fallback_opener */
					FILE *fp;
					char cmd[MAX_LEN];
					int status;

					snprintf(cmd, MAX_LEN, "file \"%s\"", newpath);
					fp = popen(cmd, "r");
					if (fp == NULL)
						goto nochange;
					if (fgets(cmd, MAX_LEN, fp) == NULL) {
						pclose(fp);
						goto nochange;
					}
					pclose(fp);

					if (strstr(cmd, "ASCII text") != NULL)
						bin = execvim;
					else if (fallback_opener) {
						snprintf(cmd, MAX_LEN, "%s \"%s\" > /dev/null 2>&1",
							fallback_opener, newpath);
						status = system(cmd);
						continue;
					} else {
						printmsg("No association");
						goto nochange;
					}
				}
				exitcurses();
				spawn(bin, newpath, NULL);
				initcurses();
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
				tmp = ifilter;
			/* Check and report regex errors */
			r = setfilter(&re, tmp);
			if (r != 0)
				goto nochange;
			strlcpy(fltr, tmp, sizeof(fltr));
			DPRINTF_S(fltr);
			/* Save current */
			if (ndents > 0)
				mkpath(path, dents[cur].name, oldpath, sizeof(oldpath));
			goto begin;
		case SEL_NEXT:
			if (cur < ndents - 1)
				cur++;
			else if (ndents)
				/* Roll over, set cursor to first entry */
				cur = 0;
			break;
		case SEL_PREV:
			if (cur > 0)
				cur--;
			else if (ndents)
				/* Roll over, set cursor to last entry */
				cur = ndents - 1;
			break;
		case SEL_PGDN:
			if (cur < ndents - 1)
				cur += MIN((LINES - 4) / 2, ndents - 1 - cur);
			break;
		case SEL_PGUP:
			if (cur > 0)
				cur -= MIN((LINES - 4) / 2, cur);
			break;
		case SEL_HOME:
			cur = 0;
			break;
		case SEL_END:
			cur = ndents - 1;
			break;
		case SEL_CD:
			/* Read target dir */
			printprompt("chdir: ");
			tmp = readln();
			if (tmp == NULL) {
				clearprompt();
				goto nochange;
			}
			mkpath(path, tmp, newpath, sizeof(newpath));
			if (canopendir(newpath) == 0) {
				printwarn();
				goto nochange;
			}
			strlcpy(path, newpath, sizeof(path));
			/* Reset filter */
			strlcpy(fltr, ifilter, sizeof(fltr))
			DPRINTF_S(path);
			goto begin;
		case SEL_CDHOME:
			tmp = getenv("HOME");
			if (tmp == NULL) {
				clearprompt();
				goto nochange;
			}
			if (canopendir(tmp) == 0) {
				printwarn();
				goto nochange;
			}
			strlcpy(path, tmp, sizeof(path));
			/* Reset filter */
			strlcpy(fltr, ifilter, sizeof(fltr));
			DPRINTF_S(path);
			goto begin;
		case SEL_TOGGLEDOT:
			showhidden ^= 1;
			initfilter(showhidden, &ifilter);
			strlcpy(fltr, ifilter, sizeof(fltr));
			goto begin;
		case SEL_DETAIL:
			showdetail = !showdetail;
			showdetail ? (printptr = &printent_long) : (printptr = &printent);
			/* Save current */
			if (ndents > 0)
				mkpath(path, dents[cur].name, oldpath, sizeof(oldpath));
			goto begin;
		case SEL_FSIZE:
			sizeorder = !sizeorder;
			mtimeorder = 0;
			/* Save current */
			if (ndents > 0)
				mkpath(path, dents[cur].name, oldpath, sizeof(oldpath));
			goto begin;
		case SEL_MTIME:
			mtimeorder = !mtimeorder;
			sizeorder = 0;
			/* Save current */
			if (ndents > 0)
				mkpath(path, dents[cur].name, oldpath, sizeof(oldpath));
			goto begin;
		case SEL_REDRAW:
			/* Save current */
			if (ndents > 0)
				mkpath(path, dents[cur].name, oldpath, sizeof(oldpath));
			goto begin;
		case SEL_RUN:
			run = xgetenv(env, run);
			exitcurses();
			spawn(run, NULL, path);
			initcurses();
			/* Re-populate as directory content may have changed */
			goto begin;
		case SEL_RUNARG:
			run = xgetenv(env, run);
			exitcurses();
			spawn(run, dents[cur].name, path);
			initcurses();
			break;
		}
		/* Screensaver */
		if (idletimeout != 0 && idle == idletimeout) {
			idle = 0;
			exitcurses();
			spawn(idlecmd, NULL, NULL);
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

	/* Confirm we are in a terminal */
	if (!isatty(0) || !isatty(1)) {
		fprintf(stderr, "stdin or stdout is not a tty\n");
		exit(1);
	}

	if (getuid() == 0)
		showhidden = 1;
	initfilter(showhidden, &ifilter);

	printptr = &printent;

	if (argv[1] != NULL) {
		ipath = argv[1];
	} else {
		ipath = getcwd(cwd, sizeof(cwd));
		if (ipath == NULL)
			ipath = "/";
	}

	/* Get the default desktop mime opener, if set */
	opener = getenv("NOICE_OPENER");

	/* Get the fallback desktop mime opener, if set */
	fallback_opener = getenv("NOICE_FALLBACK_OPENER");

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
