/*
 * SPDX-License-Identifier: MIT
 *
 * Mix state, scene slots and their on-flash encoding.
 */

#ifndef SP1M_SCENE_H_
#define SP1M_SCENE_H_

#include <stddef.h>

#include "sp1m.h"

#define SCENE_COUNT		4
#define MASTER_DB_MAX		0
#define MASTER_DB_MIN		(-60)
/* One step below MASTER_DB_MIN: master off */
#define MASTER_DB_OFF		(MASTER_DB_MIN - 1)
#define MASTER_DB_DEFAULT	(-24)
#define BALANCE_MAX		100

enum spk_mode {
	SPK_AUTO = 0,	/* speaker on whenever no headphones are plugged in */
	SPK_OFF = 1,
};

struct scene {
	uint16_t vol[SP1M_TRACKS];	/* fader units 0..1023 */
	int8_t bal[SP1M_TRACKS];	/* -100..100 */
	uint8_t mute;			/* bit N = track N */
	uint8_t solo;			/* bit N = track N */
	int8_t master_db;		/* MASTER_DB_OFF..MASTER_DB_MAX */
	uint8_t spk_mode;		/* enum spk_mode */
};

struct persist {
	struct scene current;
	struct scene slot[SCENE_COUNT];
	uint8_t slot_valid;		/* bit N = slot N holds a saved scene */
	uint8_t led_mode;
	uint8_t charge_off;
};

#define PERSIST_ENCODED_MAX	128

void scene_default(struct scene *s);
void scene_sanitize(struct scene *s);
void persist_default(struct persist *p);

float master_db_to_gain(int8_t db);

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len);

/* Returns the encoded length, or 0 if @len is too small. */
size_t persist_encode(const struct persist *p, uint8_t *buf, size_t len);

/* Returns false (and leaves @p untouched) for anything not valid. */
bool persist_decode(struct persist *p, const uint8_t *buf, size_t len);

#endif /* SP1M_SCENE_H_ */
