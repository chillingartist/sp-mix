/*
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "cli.h"
#include "test.h"

static char outbuf[512];
static int calls;
static int last_argc;
static char last_arg1[CLI_LINE_MAX];

static void print(void *ctx, const char *s)
{
	strncat(outbuf, s, sizeof(outbuf) - strlen(outbuf) - 1);
}

static void cmd_scene(struct cli *c, int argc, char **argv)
{
	calls++;
	last_argc = argc;
	strcpy(last_arg1, argv[1]);
}

static const struct cli_cmd cmds[] = {
	{ "scene", "save|load <1-4>", 2, 2, cmd_scene },
};

TEST(test_cli)
{
	struct cli c;
	int v;

	cli_init(&c, cmds, 1, print, NULL);

	cli_input(&c, "scene  save\t2\r\n", 15);
	CHECK(calls == 1 && last_argc == 3 && strcmp(last_arg1, "save") == 0);

	/* Split across reads, with backspace editing */
	cli_input(&c, "scex", 4);
	cli_input(&c, "\bne load 1\n", 11);
	CHECK(calls == 2 && strcmp(last_arg1, "load") == 0);

	outbuf[0] = '\0';
	cli_input(&c, "scene save\n", 11);
	CHECK(calls == 2 && strstr(outbuf, "usage") != NULL);

	outbuf[0] = '\0';
	cli_input(&c, "bogus\n\n\r\n", 9);
	CHECK(strstr(outbuf, "unknown") != NULL);

	outbuf[0] = '\0';
	for (int i = 0; i < 100; i++) {
		cli_input(&c, "x", 1);
	}
	cli_input(&c, "\n", 1);
	CHECK(strstr(outbuf, "too long") != NULL);
	cli_input(&c, "scene load 3\n", 13);
	CHECK(calls == 3);

	CHECK(cli_parse_int("3", 1, 4, &v) && v == 3);
	CHECK(cli_parse_int("-60", -61, 0, &v) && v == -60);
	CHECK(!cli_parse_int("5", 1, 4, &v));
	CHECK(!cli_parse_int("2x", 1, 4, &v));
	CHECK(!cli_parse_int("", 1, 4, &v));
	CHECK(!cli_parse_int("-", -4, 4, &v));
}
