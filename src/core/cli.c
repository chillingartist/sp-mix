/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "cli.h"

void cli_init(struct cli *c, const struct cli_cmd *cmds, size_t ncmds,
	      cli_print_fn print, void *ctx)
{
	c->cmds = cmds;
	c->ncmds = ncmds;
	c->print = print;
	c->ctx = ctx;
	c->len = 0;
	c->overflow = false;
}

void cli_print(struct cli *c, const char *s)
{
	c->print(c->ctx, s);
}

bool cli_parse_int(const char *s, int lo, int hi, int *out)
{
	int v = 0;
	bool neg = false;

	if (*s == '-') {
		neg = true;
		s++;
	}
	if (*s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9' || v > 100000) {
			return false;
		}
		v = v * 10 + (*s - '0');
	}
	if (neg) {
		v = -v;
	}
	if (v < lo || v > hi) {
		return false;
	}
	*out = v;

	return true;
}

static void execute(struct cli *c)
{
	char *argv[CLI_ARGS_MAX + 1];
	int argc = 0;
	char *p = c->line;

	while (*p != '\0') {
		while (*p == ' ' || *p == '\t') {
			*p++ = '\0';
		}
		if (*p == '\0') {
			break;
		}
		if (argc == CLI_ARGS_MAX + 1) {
			cli_print(c, "error: too many arguments\r\n");
			return;
		}
		argv[argc++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t') {
			p++;
		}
	}

	if (argc == 0) {
		return;
	}

	for (size_t i = 0; i < c->ncmds; i++) {
		const struct cli_cmd *cmd = &c->cmds[i];

		if (strcmp(argv[0], cmd->name) != 0) {
			continue;
		}
		if (argc - 1 < cmd->min_args || argc - 1 > cmd->max_args) {
			cli_print(c, "usage: ");
			cli_print(c, cmd->name);
			cli_print(c, " ");
			cli_print(c, cmd->help);
			cli_print(c, "\r\n");
			return;
		}
		cmd->fn(c, argc, argv);
		return;
	}

	cli_print(c, "error: unknown command, try help\r\n");
}

void cli_input(struct cli *c, const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char ch = data[i];

		if (ch == '\r' || ch == '\n') {
			if (c->overflow) {
				cli_print(c, "error: line too long\r\n");
			} else if (c->len > 0) {
				c->line[c->len] = '\0';
				execute(c);
			}
			c->len = 0;
			c->overflow = false;
		} else if (ch == '\b' || ch == 0x7f) {
			if (c->len > 0) {
				c->len--;
			}
		} else if ((unsigned char)ch >= 0x20 || ch == '\t') {
			if (c->len < CLI_LINE_MAX - 1) {
				c->line[c->len++] = ch;
			} else {
				c->overflow = true;
			}
		}
	}
}
