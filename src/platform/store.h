/*
 * SPDX-License-Identifier: MIT
 *
 * Scene and settings storage in the internal flash (NVS, storage_partition).
 * Writes run on a low priority work queue; callers decide when writing is
 * allowed (never while audio streams: erasing stalls the CPU).
 */

#ifndef SP1M_STORE_H_
#define SP1M_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "scene.h"

int store_init(void);

/* Returns false when nothing valid is stored. */
bool store_load(struct persist *out);

/* Queue a write of @p; @gen comes back through store_take_done(). */
void store_request(const struct persist *p, uint32_t gen);

/* A write finished since the last call: its gen, true on success. */
bool store_take_done(uint32_t *gen, bool *ok);

bool store_busy(void);

/* Synchronous write, for the power-off path. */
int store_save_now(const struct persist *p);

#endif /* SP1M_STORE_H_ */
