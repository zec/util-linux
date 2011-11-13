/*
 *  prlimit - get/set process resource limits.
 *
 *  Copyright (C) 2011 Davidlohr Bueso <dave@gnu.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/resource.h>

#include "c.h"
#include "nls.h"
#include "tt.h"
#include "xalloc.h"
#include "strutils.h"

#ifndef RLIMIT_RTTIME
# define RLIMIT_RTTIME 15
#endif

enum {
	AS,
	CORE,
	CPU,
	DATA,
	FSIZE,
	LOCKS,
	MEMLOCK,
	MSGQUEUE,
	NICE,
	NOFILE,
	NPROC,
	RSS,
	RTPRIO,
	RTTIME,
	SIGPENDING,
	STACK
};

struct prlimit_desc {
	const char *name;
	const char *help;
	const char *unit;
	int resource;
};

static struct prlimit_desc prlimit_desc[] =
{
	[AS]         = { "AS",         N_("address space limit"),                N_("bytes"),     RLIMIT_AS },
	[CORE]       = { "CORE",       N_("max core file size"),                 N_("blocks"),    RLIMIT_CORE },
	[CPU]        = { "CPU",        N_("CPU time"),                           N_("seconds"),   RLIMIT_CPU },
	[DATA]       = { "DATA",       N_("max data size"),                      N_("bytes"),     RLIMIT_DATA },
	[FSIZE]      = { "FSIZE",      N_("max file size"),                      N_("blocks"),    RLIMIT_FSIZE },
	[LOCKS]      = { "LOCKS",      N_("max amount of file locks held"),      NULL,            RLIMIT_LOCKS },
	[MEMLOCK]    = { "MEMLOCK",    N_("max locked-in-memory address space"), N_("bytes"),     RLIMIT_MEMLOCK },
	[MSGQUEUE]   = { "MSGQUEUE",   N_("max bytes in POSIX mqueues"),         N_("bytes"),     RLIMIT_MSGQUEUE },
	[NICE]       = { "NICE",       N_("max nice prio allowed to raise"),     NULL,            RLIMIT_NICE },
	[NOFILE]     = { "NOFILE",     N_("max amount of open files"),           NULL,            RLIMIT_NOFILE },
	[NPROC]      = { "NPROC",      N_("max number of processes"),            NULL,            RLIMIT_NPROC },
	[RSS]        = { "RSS",        N_("max resident set size"),              N_("pages"),     RLIMIT_RSS },
	[RTPRIO]     = { "RTPRIO",     N_("max real-time priority"),             NULL,            RLIMIT_RTPRIO },
	[RTTIME]     = { "RTTIME",     N_("timeout for real-time tasks"),        N_("microsecs"), RLIMIT_RTTIME },
	[SIGPENDING] = { "SIGPENDING", N_("max amount of pending signals"),      NULL,            RLIMIT_SIGPENDING },
	[STACK]      = { "STACK",      N_("max stack size"),                     N_("bytes"),     RLIMIT_STACK }
};

struct prlimit {
	struct rlimit rlim;
	struct prlimit_desc *desc;
	int modify;
};

#define PRLIMIT_EMPTY_LIMIT	{{ 0, 0, }, NULL, 0 }

enum {
	COL_HELP,
	COL_RES,
	COL_SOFT,
	COL_HARD,
	COL_UNITS,
};

/* column names */
struct colinfo {
	const char	*name;	/* header */
	double		whint;	/* width hint (N < 1 is in percent of termwidth) */
	int		flags;	/* TT_FL_* */
	const char      *help;
};

/* columns descriptions */
struct colinfo infos[] = {
	[COL_RES]     = { "RESOURCE",    0.25, TT_FL_TRUNC, N_("resource name") },
	[COL_HELP]    = { "DESCRIPTION", 0.1,  TT_FL_TRUNC, N_("resource description")},
	[COL_SOFT]    = { "SOFT",        0.1,  TT_FL_RIGHT, N_("soft limit")},
	[COL_HARD]    = { "HARD",        1,    TT_FL_RIGHT, N_("hard limit (ceiling)")},
	[COL_UNITS]   = { "UNITS",       0.1,  TT_FL_TRUNC, N_("units")},
};

#define NCOLS ARRAY_SIZE(infos)
#define MAX_RESOURCES ARRAY_SIZE(prlimit_desc)

#define INFINITY_STR	"unlimited"
#define INFINITY_STRLEN	(sizeof(INFINITY_STR) - 1)

#define PRLIMIT_SOFT	(1 << 1)
#define PRLIMIT_HARD	(1 << 2)

/* array with IDs of enabled columns */
static int columns[NCOLS], ncolumns;
static pid_t pid; /* calling process (default) */
static int verbose;

#ifndef HAVE_PRLIMIT
# include <sys/syscall.h>
static int prlimit(pid_t p, int resource,
		   const struct rlimit *new_limit,
		   struct rlimit *old_limit)
{
	return syscall(SYS_prlimit64, p, resource, new_limit, old_limit);
}
#endif

static void __attribute__ ((__noreturn__)) usage(FILE * out)
{
	size_t i;

	fputs(USAGE_HEADER, out);

	fprintf(out,
		_(" %s [options]\n"), program_invocation_short_name);

	fputs(_("\nGeneral Options:\n"), out);
	fputs(_(" -p, --pid <pid>        process id\n"
		" -o, --output <list>    define which output columns to use\n"
		"     --noheadings       don't print headings\n"
		"     --raw              use the raw output format\n"
		"     --verbose          verbose output\n"
		" -h, --help             display this help and exit\n"
		" -V, --version          output version information and exit\n"), out);

	fputs(_("\nResources Options:\n"), out);
	fputs(_(" -c, --core             maximum size of core files created\n"
		" -d, --data             maximum size of a process's data segment\n"
		" -e, --nice             maximum nice priority allowed to raise\n"
		" -f, --fsize            maximum size of files written by the process\n"
		" -i, --sigpending       maximum amount of pending signals\n"
		" -l, --memlock          maximum size a process may lock into memory\n"
		" -m, --rss              maximum resident set size\n"
		" -n, --nofile           maximum amount of open files\n"
		" -q, --msgqueue         maximum bytes in POSIX message queues\n"
		" -r, --rtprio           maximum real-time scheduling priority\n"
		" -s, --stack            maximum stack size\n"
		" -t, --cpu              maximum amount of CPU time in seconds\n"
		" -u, --nproc            maximum number of user processes\n"
		" -v, --as               size of virtual memory\n"
		" -x, --locks            maximum number of file locks\n"
		" -y, --rttime           CPU time in microseconds a process scheduled\n"
		"                        under real-time scheduling\n"), out);

	fputs(_("\nAvailable columns (for --output):\n"), out);

	for (i = 0; i < NCOLS; i++)
		fprintf(out, " %11s  %s\n", infos[i].name, _(infos[i].help));

	fprintf(out, USAGE_MAN_TAIL("prlimit(1)"));

	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static inline int get_column_id(int num)
{
	assert(ARRAY_SIZE(columns) == NCOLS);
	assert(num < ncolumns);
	assert(columns[num] < (int) NCOLS);

	return columns[num];
}

static inline struct colinfo *get_column_info(unsigned num)
{
	return &infos[ get_column_id(num) ];
}

static void add_tt_line(struct tt *tt, struct prlimit *l)
{
	int i;
	struct tt_line *line;

	assert(tt);
	assert(l);

	line = tt_add_line(tt, NULL);
	if (!line) {
		warn(_("failed to add line to output"));
		return;
	}

	for (i = 0; i < ncolumns; i++) {
		char *str = NULL;
		int rc = 0;

		switch (get_column_id(i)) {
		case COL_RES:
			rc = asprintf(&str, "%s", l->desc->name);
			break;
		case COL_HELP:
			rc = asprintf(&str, "%s", l->desc->help);
			break;
		case COL_SOFT:
			rc = l->rlim.rlim_cur == RLIM_INFINITY ?
				asprintf(&str, "%s", "unlimited") :
				asprintf(&str, "%llu", (unsigned long long) l->rlim.rlim_cur);
			break;
		case COL_HARD:
			rc = l->rlim.rlim_max == RLIM_INFINITY ?
				asprintf(&str, "%s", "unlimited") :
				asprintf(&str, "%llu", (unsigned long long) l->rlim.rlim_max);
			break;
		case COL_UNITS:
			str = l->desc->unit ? xstrdup(_(l->desc->unit)) : NULL;
			break;
		default:
			break;
		}

		if (rc || str)
			tt_line_set_data(line, i, str);
	}
}

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	assert(name);

	for (i = 0; i < NCOLS; i++) {
		const char *cn = infos[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);
	return -1;
}

static int show_limits(struct prlimit lims[], size_t n, int tt_flags)
{
	int i;
	struct tt *tt;

	tt = tt_new_table(tt_flags);
	if (!tt) {
		warn(_("failed to initialize output table"));
		return -1;
	}

	for (i = 0; i < ncolumns; i++) {
		struct colinfo *col = get_column_info(i);

		if (!tt_define_column(tt, col->name, col->whint, col->flags)) {
			warnx(_("failed to initialize output column"));
			goto done;
		}
	}

	for (i = 0; (size_t) i < n; i++)
		if (!lims[i].modify) /* only display old limits */
			add_tt_line(tt, &lims[i]);

	tt_print_table(tt);
done:
	tt_free_table(tt);
	return 0;
}


static void do_prlimit(struct prlimit lims[], size_t n, int tt_flags)
{
	size_t i, nshows = 0;

	for (i = 0; i < n; i++) {
		struct rlimit *new = NULL;

		if (lims[i].modify)
			new = &lims[i].rlim;
		else
			nshows++;

		if (verbose && new) {
			printf(_("New %s limit: "), lims[i].desc->name);
			if (new->rlim_cur == RLIM_INFINITY)
				printf("<%s", _("unlimited"));
			else
				printf("<%ju", new->rlim_cur);

			if (new->rlim_max == RLIM_INFINITY)
				printf(":%s>\n", _("unlimited"));
			else
				printf(":%ju>\n", new->rlim_max);
		}

		if (prlimit(pid, lims[i].desc->resource, new, &lims[i].rlim) == -1)
			err(EXIT_FAILURE, _("failed to get resource limits for PID %d"), pid);
	}

	if (nshows)
		show_limits(lims, n, tt_flags);
}



static int get_range(char *str, rlim_t *soft, rlim_t *hard, int *found)
{
	char *end = NULL;

	if (!str)
		return 0;

	*found = errno = 0;
	*soft = *hard = RLIM_INFINITY;

	if (!strcmp(str, INFINITY_STR)) {		/* <unlimited> */
		*found |= PRLIMIT_SOFT | PRLIMIT_HARD;
		return 0;

	} else if (*str == ':') {			/* <:hard> */
		str++;

		if (strcmp(str, INFINITY_STR) != 0) {
			*hard = strtoull(str, &end, 10);

			if (errno || !end || *end || end == str)
				return -1;
		}
		*found |= PRLIMIT_HARD;
		return 0;

	}

	if (strncmp(str, INFINITY_STR, INFINITY_STRLEN) == 0) {
		/* <unlimited> or <unlimited:> */
		end = str + INFINITY_STRLEN;
	} else {
		/* <value> or <soft:> */
		*hard = *soft = strtoull(str, &end, 10);
		if (errno || !end || end == str)
			return -1;
	}

	if (*end == ':' && !*(end + 1))			/* <soft:> */
		*found |= PRLIMIT_SOFT;

	else if (*end == ':') {				/* <soft:hard> */
		str = end + 1;

		if (!strcmp(str, INFINITY_STR))
			*hard =  RLIM_INFINITY;
		else {
			end = NULL;
			errno = 0;
			*hard = strtoull(str, &end, 10);

			if (errno || !end || *end || end == str)
				return -1;
		}
		*found |= PRLIMIT_SOFT | PRLIMIT_HARD;

	} else						/* <value> */
		*found |= PRLIMIT_SOFT | PRLIMIT_HARD;

	return 0;
}


static int parse_prlim(struct rlimit *lim, char *ops, size_t id)
{
	rlim_t soft, hard;
	int found = 0;

	if (get_range(ops, &soft, &hard, &found))
		errx(EXIT_FAILURE, _("failed to parse %s limit"),
		     prlimit_desc[id].name);

	/*
	 * If one of the limits is unknown (default value for not being passed), we need
	 * to get the current limit and use it.
	 * I see no other way other than using prlimit(2).
	 */
	if (found != (PRLIMIT_HARD | PRLIMIT_SOFT)) {
		struct rlimit old;

		if (prlimit(pid, prlimit_desc[id].resource, NULL, &old) == -1)
			errx(EXIT_FAILURE, _("failed to get old %s limit"),
			     prlimit_desc[id].name);

		if (!(found & PRLIMIT_SOFT))
			soft = old.rlim_cur;
		else if (!(found & PRLIMIT_HARD))
			hard = old.rlim_max;
	}

	if (soft > hard && (soft != RLIM_INFINITY || hard != RLIM_INFINITY))
		errx(EXIT_FAILURE, _("the soft limit cannot exceed the ceiling value"));

	lim->rlim_cur = soft;
	lim->rlim_max = hard;

	return 0;
}

/*
 * Add a resource limit to the limits array
 */
static int add_prlim(char *ops, struct prlimit *lim, size_t id)
{
	lim->desc = &prlimit_desc[id];

	if (ops) { /* planning on modifying a limit? */
		lim->modify = 1;
		parse_prlim(&lim->rlim, ops, id);
	}

	return 0;
}

int main(int argc, char **argv)
{
	int opt, tt_flags = 0;
        size_t n = 0;
	struct prlimit lims[MAX_RESOURCES] = { PRLIMIT_EMPTY_LIMIT };

	enum {
		VERBOSE_OPTION = CHAR_MAX + 1,
		RAW_OPTION,
		NOHEADINGS_OPTION
	};

	static const struct option longopts[] = {
		{ "pid",	required_argument, NULL, 'p' },
		{ "output",     required_argument, NULL, 'o' },
		{ "as",         optional_argument, NULL, 'v' },
		{ "core",       optional_argument, NULL, 'c' },
		{ "cpu",        optional_argument, NULL, 't' },
		{ "data",       optional_argument, NULL, 'd' },
		{ "fsize",      optional_argument, NULL, 'f' },
		{ "locks",      optional_argument, NULL, 'x' },
		{ "memlock",    optional_argument, NULL, 'l' },
		{ "msgqueue",   optional_argument, NULL, 'q' },
		{ "nice",       optional_argument, NULL, 'e' },
		{ "nofile",     optional_argument, NULL, 'n' },
		{ "nproc",      optional_argument, NULL, 'u' },
		{ "rss",        optional_argument, NULL, 'm' },
		{ "rtprio",     optional_argument, NULL, 'r' },
		{ "rttime",     optional_argument, NULL, 'y' },
		{ "sigpending", optional_argument, NULL, 'i' },
		{ "stack",      optional_argument, NULL, 's' },
		{ "version",    no_argument, NULL, 'V' },
		{ "help",       no_argument, NULL, 'h' },
		{ "noheadings", no_argument, NULL, NOHEADINGS_OPTION },
		{ "raw",        no_argument, NULL, RAW_OPTION },
		{ "verbose",    no_argument, NULL, VERBOSE_OPTION },
		{ NULL, 0, NULL, 0 }
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	/*
	 * Something is very wrong if this doesn't succeed,
	 * assuming STACK is the last resource, of course.
	 */
	assert(MAX_RESOURCES == STACK + 1);

	while((opt = getopt_long(argc, argv,
				 "c::d::e::f::i::l::m::n::q::r::s::t::u::v::x::y::p:o:vVh",
				 longopts, NULL)) != -1) {
		switch(opt) {
		case 'c':
			add_prlim(optarg, &lims[n++], CORE);
			break;
		case 'd':
			add_prlim(optarg, &lims[n++], DATA);
			break;
		case 'e':
			add_prlim(optarg, &lims[n++], NICE);
			break;
		case 'f':
			add_prlim(optarg, &lims[n++], FSIZE);
			break;
		case 'i':
			add_prlim(optarg, &lims[n++], SIGPENDING);
			break;
		case 'l':
			add_prlim(optarg, &lims[n++], MEMLOCK);
			break;
		case 'm':
			add_prlim(optarg, &lims[n++], RSS);
			break;
		case 'n':
			add_prlim(optarg, &lims[n++], NOFILE);
			break;
		case 'q':
			add_prlim(optarg, &lims[n++], MSGQUEUE);
			break;
		case 'r':
			add_prlim(optarg, &lims[n++], RTPRIO);
			break;
		case 's':
			add_prlim(optarg, &lims[n++], STACK);
			break;
		case 't':
			add_prlim(optarg, &lims[n++], CPU);
			break;
		case 'u':
			add_prlim(optarg, &lims[n++], NPROC);
			break;
		case 'v':
			add_prlim(optarg, &lims[n++], AS);
			break;
		case 'x':
			add_prlim(optarg, &lims[n++], LOCKS);
			break;
		case 'y':
			add_prlim(optarg, &lims[n++], RTTIME);
			break;

		case 'p':
			if (pid) /* we only work one pid at a time */
				errx(EXIT_FAILURE, _("only use one PID at a time"));

			pid = strtol_or_err(optarg, _("cannot parse PID"));
			break;
		case 'h':
			usage(stdout);
			break;
		case 'o':
			ncolumns = string_to_idarray(optarg,
						     columns, ARRAY_SIZE(columns),
						     column_name_to_id);
			if (ncolumns < 0)
				return EXIT_FAILURE;
			break;
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;

		case NOHEADINGS_OPTION:
			tt_flags |= TT_FL_NOHEADINGS;
			break;
		case VERBOSE_OPTION:
			verbose++;
			break;
		case RAW_OPTION:
			tt_flags |= TT_FL_RAW;
			break;

		default:
			usage(stderr);
			break;
		}
	}

	if (argc == 1)
		usage(stderr);

	if (!ncolumns) {
		/* default columns */
		columns[ncolumns++] = COL_RES;
		columns[ncolumns++] = COL_HELP;
		columns[ncolumns++] = COL_SOFT;
		columns[ncolumns++] = COL_HARD;
		columns[ncolumns++] = COL_UNITS;
	}

	if (!n) {
		/* default is to print all resources */
		for (; n < MAX_RESOURCES; n++)
			add_prlim(NULL, &lims[n], n);
	}

	do_prlimit(lims, n, tt_flags);

	return EXIT_SUCCESS;
}