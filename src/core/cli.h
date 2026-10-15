/*
 * SPDX-License-Identifier: MIT
 *
 * Line based command interpreter for the CDC serial port. Bytes go in,
 * complete lines are split into words and dispatched through a table.
 */

#ifndef SP1M_CLI_H_
#define SP1M_CLI_H_

#include <stddef.h>

#include "sp1m.h"

#define CLI_LINE_MAX	64
#define CLI_ARGS_MAX	4

struct cli;

typedef void (*cli_print_fn)(void *ctx, const char *s);

struct cli_cmd {
	const char *name;
	const char *help;
	uint8_t min_args;	/* not counting the command itself */
	uint8_t max_args;
	void (*fn)(struct cli *c, int argc, char **argv);
};

struct cli {
	const struct cli_cmd *cmds;
	size_t ncmds;
	cli_print_fn print;
	void *ctx;
	char line[CLI_LINE_MAX];
	size_t len;
	bool overflow;
};

void cli_init(struct cli *c, const struct cli_cmd *cmds, size_t ncmds,
	      cli_print_fn print, void *ctx);

/* Feed received bytes; complete lines are executed immediately. */
void cli_input(struct cli *c, const char *data, size_t len);

void cli_print(struct cli *c, const char *s);

/* Parses a decimal integer in [lo, hi]. */
bool cli_parse_int(const char *s, int lo, int hi, int *out);

#endif /* SP1M_CLI_H_ */
