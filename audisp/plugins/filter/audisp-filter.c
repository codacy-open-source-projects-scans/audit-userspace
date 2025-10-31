/* audisp-filter.c --
 * Copyright 2024 Red Hat Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *   Attila Lakatos <alakatos@redhat.com>
 *
 */

#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>

#ifdef HAVE_LIBCAP_NG
#include <cap-ng.h>
#endif
#include "auparse.h"
#include "common.h"
#include "libaudit.h"
#include "auplugin.h"

struct filter_rule {
	char* expr;
	int lineno;
	struct filter_rule* next;
};

struct filter_list {
	struct filter_rule* head;
	struct filter_rule* tail;
};

enum {
	ALLOWLIST,
	BLOCKLIST
};

struct filter_conf {
	int mode; /* allowlist or blocklist */
	const char* binary; /* external program that will receive filter audit events */
	char** binary_args; /* arguments for external program */
	const char* config_file; /* file containing audit expressions */
	int only_check; /* just verify the syntax of the config_file and exit */
};

/* Global Data */
static volatile int stop = 0;
static volatile int hup = 0;
static int pipefd[2];
static int errors = 0;
static struct filter_list list;
pid_t cpid = -1;

static struct filter_conf config = {
	.mode = -1,
	.binary = NULL,
	.binary_args = NULL,
	.config_file = NULL,
	.only_check = 0
};

static void reload_config(void);

static void handle_event(auparse_state_t* au, auparse_cb_event_t cb_event_type,
			 void* user_data)
{
	static int rules_loaded = 0;
	char *error = NULL;

	if (!rules_loaded || hup) {
		if (hup) {
			reload_config();
			hup = 0;
			ausearch_clear(au);
		}
		ausearch_set_stop(au, AUSEARCH_STOP_EVENT);
		for (struct filter_rule *rule = list.head; rule;
							rule = rule->next) {
			int rc = ausearch_add_expression(au, rule->expr, &error,
							AUSEARCH_RULE_OR);
			if (rc)
				syslog(LOG_ERR,
					"Failed adding '%s' to ausearch (%s)",
					rule->expr, error);
			free(error);
			error = NULL;
		}
		rules_loaded = 1;
	}

        int rc, forward_event;

	// Determine whether to forward or drop the event
	rc = ausearch_cur_event(au);
	if (rc > 0) { /* matched */
		forward_event = (config.mode == ALLOWLIST) ? 0 : 1;
	} else if (rc == 0) { /* not matched */
		forward_event = (config.mode == ALLOWLIST) ? 1 : 0;
	} else {
		syslog(LOG_ERR, "The ausearch_next_event returned %d", rc);
		return;
	}

	if (forward_event) {
		const int records = auparse_get_num_records(au);
		for (int i = 0; i < records; i++) {
			auparse_goto_record_num(au, i);
			const char* txt = auparse_get_record_text(au);

			// Need to add new line character to signal end of the current record
			if (write(pipefd[1], txt, strlen(txt)) == -1 ||
			    write(pipefd[1], "\n", 1) == -1) {
				syslog(LOG_ERR, "Failed to write to pipe");
				return;
			}
		}
	}
}

static void free_args()
{
	if (config.binary_args) {
		for (int i = 0; config.binary_args[i] != NULL; i++) {
			free(config.binary_args[i]);
		}
		free(config.binary_args);
	}
}

static int parse_args(int argc, const char* argv[])
{
	if (argc == 3 && (strcmp("--check", argv[1]) == 0)) {
		config.config_file = argv[2];
		config.only_check = 1;
		return 0;
	}

	if (argc <= 3) {
		syslog(LOG_ERR, "Not enough command line arguments");
		return 1;
	}

	if (strcasecmp(argv[1], "allowlist") == 0)
		config.mode = ALLOWLIST;
	else if (strcasecmp(argv[1], "blocklist") == 0)
		config.mode = BLOCKLIST;
	else {
		syslog(LOG_ERR,
			"Invalid mode '%s' specified, possible values are: allowlist, "
			"blocklist.",
			argv[1]);
		return 1;
	}

	config.config_file = argv[2];
	config.binary = argv[3];

	argc -= 3;
	argv += 3;

	config.binary_args = malloc(sizeof(char*) * (argc + 1)); /* +1 is for the last NULL */
	if (!config.binary_args)
		return 1;

	for (int i = 0; i < argc; i++) {
		config.binary_args[i] = strdup(argv[i]);
		if (!config.binary_args[i]) {
			while (i > 0) {
				free(config.binary_args[--i]);
			}
			free(config.binary_args);
			return 1;
		}
	}
	config.binary_args[argc] = NULL;

	return 0;
}

static char* get_line(FILE* f, char* buf, unsigned size, int* lineno,
	const char* file)
{
	int too_long = 0;

	while (fgets_unlocked(buf, size, f)) {
		/* remove newline */
		char* ptr = strchr(buf, 0x0a);
		if (ptr) {
			if (!too_long) {
				*ptr = 0;
				return buf;
			}
			// Reset and start with the next line
			too_long = 0;
			*lineno = *lineno + 1;
		} else {
			// If a line is too long skip it.
			// Only output 1 warning
			if (!too_long)
				syslog(LOG_WARNING,
				       "Skipping line %d in %s: too long",
				       *lineno, file);
			too_long = 1;
		}
	}
	return NULL;
}

// static void print_rules(struct filter_list* list) {
// 	struct filter_rule* rule;
// 	int count = 0;
//
// 	for (rule = list->head; rule != NULL; rule = rule->next, count++) {
// 		printf("Rule %d on line %d: %s\n", count, rule->lineno, rule->expr);
// 	}
// }

static void reset_rules(struct filter_list* list)
{
	list->head = list->tail = NULL;
}

static void free_rule(struct filter_rule* rule)
{
	free(rule->expr);
}

static void free_rules(struct filter_list* list)
{
	struct filter_rule* current = list->head, * to_delete;
	while (current != NULL) {
		to_delete = current;
		current = current->next;
		free_rule(to_delete);
		free(to_delete);
	}
}

static void append_rule(struct filter_list* list, struct filter_rule* rule)
{
	if (list->head == NULL) {
		list->head = list->tail = rule;
	} else {
		list->tail->next = rule;
		list->tail = rule;
	}
}

static struct filter_rule* parse_line(char* line, int lineno)
{
	struct filter_rule* rule;
	auparse_state_t* au;
	const char* buf[] = { NULL };
	char* error = NULL;

	/* dummy instance of the audit parsing library, we use it to
	validate search expressions that will be added to the filter engine */
	if ((au = auparse_init(AUSOURCE_BUFFER_ARRAY, buf)) == NULL) {
		syslog(LOG_ERR, "The auparse_init failed");
		return NULL;
	}

	// Skip whitespace
	while (*line == ' ')
		line++;

	// Empty line or it's a comment
	if (!*line || *line == '#') {
		auparse_destroy(au);
		return NULL;
	}

	if ((rule = malloc(sizeof(struct filter_rule))) == NULL) {
		auparse_destroy(au);
		return NULL;
	}
	rule->lineno = lineno;
	rule->next = NULL;

	if ((rule->expr = strdup(line)) == NULL) {
		auparse_destroy(au);
		free(rule);
		return NULL;
	}

	if (ausearch_add_expression(au, rule->expr, &error, AUSEARCH_RULE_OR) != 0) {
		syslog(LOG_ERR, "Invalid expression: %s (%s)", rule->expr, error);
		free_rule(rule);
		free(rule);
		rule = NULL;
		errors++;
	}

	auparse_destroy(au);
	return rule;
}

/*
 * Load rules from config into our linked list
 */
static int load_rules(struct filter_list* list)
{
	int fd, lineno = 0;
	struct stat st;
	char buf[1024];
	FILE* f;

	reset_rules(list);
	errors = 0;

	/* open the file */
	if ((fd = open(config.config_file, O_RDONLY)) < 0) {
		if (errno != ENOENT) {
			syslog(LOG_ERR, "Error opening config file (%s)",
			       strerror(errno));
			return 1;
		}
		syslog(LOG_ERR, "Config file %s doesn't exist, skipping",
			config.config_file);
		return 1;
	}

	if (fstat(fd, &st) < 0) {
		syslog(LOG_ERR, "Error fstat'ing config file (%s)", strerror(errno));
		close(fd);
		return 1;
	}
	if (st.st_uid != 0) {
		syslog(LOG_ERR, "Error - %s isn't owned by root", config.config_file);
		close(fd);
		return 1;
	}
	if ((st.st_mode & S_IWOTH) == S_IWOTH) {
		syslog(LOG_ERR, "Error - %s is world writable", config.config_file);
		close(fd);
		return 1;
	}
	if (!S_ISREG(st.st_mode)) {
		syslog(LOG_ERR, "Error - %s is not a regular file", config.config_file);
		close(fd);
		return 1;
	}

	/* it's ok, read line by line */
	f = fdopen(fd, "rm");
	if (f == NULL) {
		syslog(LOG_ERR, "Error - fdopen failed (%s)", strerror(errno));
		close(fd);
		return 1;
	}

	while (get_line(f, buf, sizeof(buf), &lineno, config.config_file)) {
		lineno++;
		struct filter_rule* rule;
		if ((rule = parse_line(buf, lineno)) == NULL)
			continue;

		append_rule(list, rule);
	}
	fclose(f);

	return errors;
}

/*
 * SIGCHLD handler: reap exiting processes
 */
static void child_handler(int sig)
{
	while (waitpid(-1, NULL, WNOHANG) > 0)
		; /* empty */
	stop = 1;
	auplugin_stop();
}

/*
 * SIGTERM handler
 *
 * Only honor the signal if it comes from the parent process so that other
 * tasks (cough, systemctl, cough) can't make the plugin exit without
 * the dispatcher in agreement. Otherwise it will restart the plugin.
 */
static void term_handler(int sig, siginfo_t *info, void *ucontext)
{
	if (info && info->si_pid != getppid())
		return;
	kill(cpid, sig);
	stop = 1;
	auplugin_stop();
}

/*
 * SIGHUP handler: re-read config
 */
static void hup_handler(int sig)
{
	kill(cpid, sig);
	hup = 1;
}

static void reload_config(void)
{
	hup = 0;
	struct filter_list new_list;

	/* load new rules */
	if (load_rules(&new_list)) {
		syslog(LOG_INFO, "The rules were not reloaded because of a syntax error");
		free_rules(&new_list);
		return;
	}

	/* remove unused previous rules */
	free_rules(&list);
	list = new_list;
	syslog(LOG_INFO, "Successfully reloaded rules");
}

int main(int argc, const char* argv[])
{
	struct sigaction sa;

	/* validate args */
	if (parse_args(argc, argv))
		return 1;

	/* create a list of rules from config file */
	if (load_rules(&list)) {
		free_rules(&list);
		free_args();
		return 1;
	}

	/* validate the ruleset and exit */
	if (config.only_check) {
		free_rules(&list);
		free_args();
		return 0;
	}

	/* Register sighandlers */
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	/* Set handler for the ones we care about */
	sa.sa_handler = hup_handler;
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = child_handler;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_sigaction = term_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGTERM, &sa, NULL);

#ifdef HAVE_LIBCAP_NG
	// Drop capabilities
	capng_clear(CAPNG_SELECT_BOTH);
	if (capng_apply(CAPNG_SELECT_BOTH))
		syslog(LOG_WARNING,
			"audisp-filter: unable to drop capabilities, continuing with "
			"elevated privileges");
#endif

	if (pipe(pipefd) == -1) {
		syslog(LOG_ERR, "audisp-filter: unable to open a pipe (%s)",
			strerror(errno));
		return -1;
	}

	cpid = fork();
	if (cpid == -1) {
		syslog(LOG_ERR, "audisp-filter: unable to create fork (%s)", strerror(errno));
		return -1;
	}

	if (cpid == 0) {
		/* Child reads filtered input*/

		close(pipefd[1]);
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);

		execve(config.binary, config.binary_args, NULL);
		syslog(LOG_ERR, "audisp-filter: execve failed (%s)", strerror(errno));
		exit(1);
	} else {
		/* Parent reads input and forwards data after filters have been applied
		 */
		close(pipefd[0]);

		if (auplugin_init(0, 128, AUPLUGIN_Q_IN_MEMORY, NULL)) {
			syslog(LOG_ERR,
			       "audisp-filter: failed to init auplugin");
			kill(cpid, SIGTERM);
			return -1;
		}
		auplugin_event_feed(handle_event, 1, NULL);
	}

	free_rules(&list);
	free_args();
	return 0;
}

