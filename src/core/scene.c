/*
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <string.h>

#include "fader.h"
#include "scene.h"

#define MAGIC0		'S'
#define MAGIC1		'M'
#define VERSION		1
#define SCENE_BYTES	16
#define HEADER_BYTES	4
#define PAYLOAD_BYTES	((1 + SCENE_COUNT) * SCENE_BYTES + 3)
#define ENCODED_BYTES	(HEADER_BYTES + PAYLOAD_BYTES + 2)

_Static_assert(ENCODED_BYTES <= PERSIST_ENCODED_MAX, "persist blob too big");

void scene_default(struct scene *s)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		s->vol[t] = FADER_MAX * 3 / 4;
		s->bal[t] = 0;
	}
	s->mute = 0;
	s->solo = 0;
	s->master_db = MASTER_DB_DEFAULT;
	s->spk_mode = SPK_AUTO;
}

void scene_sanitize(struct scene *s)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		if (s->vol[t] > FADER_MAX) {
			s->vol[t] = FADER_MAX;
		}
		s->bal[t] = (int8_t)sp1m_clampi(s->bal[t], -BALANCE_MAX, BALANCE_MAX);
	}
	s->mute &= ((1u << SP1M_TRACKS) - 1);
	s->solo &= ((1u << SP1M_TRACKS) - 1);
	s->master_db = (int8_t)sp1m_clampi(s->master_db, MASTER_DB_OFF,
					   MASTER_DB_MAX);
	if (s->spk_mode > SPK_OFF) {
		s->spk_mode = SPK_AUTO;
	}
}

void persist_default(struct persist *p)
{
	memset(p, 0, sizeof(*p));
	scene_default(&p->current);
	for (int i = 0; i < SCENE_COUNT; i++) {
		scene_default(&p->slot[i]);
	}
}

float master_db_to_gain(int8_t db)
{
	if (db <= MASTER_DB_OFF) {
		return 0.0f;
	}

	return powf(10.0f, (float)db / 20.0f);
}

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) :
					       (uint16_t)(crc << 1);
		}
	}

	return crc;
}

static uint8_t *put_scene(uint8_t *o, const struct scene *s)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		*o++ = (uint8_t)(s->vol[t] & 0xff);
		*o++ = (uint8_t)(s->vol[t] >> 8);
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		*o++ = (uint8_t)s->bal[t];
	}
	*o++ = s->mute;
	*o++ = s->solo;
	*o++ = (uint8_t)s->master_db;
	*o++ = s->spk_mode;

	return o;
}

static const uint8_t *get_scene(const uint8_t *i, struct scene *s)
{
	for (int t = 0; t < SP1M_TRACKS; t++) {
		s->vol[t] = (uint16_t)(i[0] | (i[1] << 8));
		i += 2;
	}
	for (int t = 0; t < SP1M_TRACKS; t++) {
		s->bal[t] = (int8_t)*i++;
	}
	s->mute = *i++;
	s->solo = *i++;
	s->master_db = (int8_t)*i++;
	s->spk_mode = *i++;
	scene_sanitize(s);

	return i;
}

size_t persist_encode(const struct persist *p, uint8_t *buf, size_t len)
{
	uint8_t *o = buf;
	uint16_t crc;

	if (len < ENCODED_BYTES) {
		return 0;
	}

	*o++ = MAGIC0;
	*o++ = MAGIC1;
	*o++ = VERSION;
	*o++ = PAYLOAD_BYTES;
	o = put_scene(o, &p->current);
	for (int i = 0; i < SCENE_COUNT; i++) {
		o = put_scene(o, &p->slot[i]);
	}
	*o++ = p->slot_valid;
	*o++ = p->led_mode;
	*o++ = p->charge_off;

	crc = crc16_ccitt(0xffff, buf, (size_t)(o - buf));
	*o++ = (uint8_t)(crc & 0xff);
	*o++ = (uint8_t)(crc >> 8);

	return (size_t)(o - buf);
}

bool persist_decode(struct persist *p, const uint8_t *buf, size_t len)
{
	struct persist tmp;
	const uint8_t *i = buf + HEADER_BYTES;
	uint16_t crc;

	/* Deterministic padding: callers may compare whole structs */
	memset(&tmp, 0, sizeof(tmp));

	if (len != ENCODED_BYTES || buf[0] != MAGIC0 || buf[1] != MAGIC1 ||
	    buf[2] != VERSION || buf[3] != PAYLOAD_BYTES) {
		return false;
	}

	crc = crc16_ccitt(0xffff, buf, ENCODED_BYTES - 2);
	if ((buf[ENCODED_BYTES - 2] | (buf[ENCODED_BYTES - 1] << 8)) != crc) {
		return false;
	}

	i = get_scene(i, &tmp.current);
	for (int s = 0; s < SCENE_COUNT; s++) {
		i = get_scene(i, &tmp.slot[s]);
	}
	tmp.slot_valid = *i++ & ((1u << SCENE_COUNT) - 1);
	tmp.led_mode = *i++ & 1u;
	tmp.charge_off = *i++ ? 1 : 0;

	*p = tmp;

	return true;
}
