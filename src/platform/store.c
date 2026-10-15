/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>

#include "safety.h"
#include "store.h"

#define PERSIST_ID	0x5301	/* distinct from ids other SP-1 firmware used */

/* Stay clear of the application above and the bootloader page below. */
BUILD_ASSERT(FIXED_PARTITION_OFFSET(storage_partition) >=
	     FIXED_PARTITION_OFFSET(slot0_partition) +
	     FIXED_PARTITION_SIZE(slot0_partition));
BUILD_ASSERT(FIXED_PARTITION_OFFSET(storage_partition) +
	     FIXED_PARTITION_SIZE(storage_partition) <= 0xff000);

static struct nvs_fs fs;
static bool mounted;

static K_MUTEX_DEFINE(lock);
static struct persist pending;
static uint32_t pending_gen;
static atomic_t busy;
static atomic_t done;
static uint32_t done_gen;
static bool done_ok;

K_THREAD_STACK_DEFINE(store_stack, 1536);
static struct k_work_q store_q;
static struct k_work store_work;

static int save(const struct persist *p)
{
	uint8_t buf[PERSIST_ENCODED_MAX];
	size_t n;
	ssize_t w;

	if (!mounted) {
		return -ENODEV;
	}

	n = persist_encode(p, buf, sizeof(buf));
	if (n == 0) {
		return -EINVAL;
	}

	/* NVS skips the write when the data is unchanged. */
	w = nvs_write(&fs, PERSIST_ID, buf, n);

	return w < 0 ? (int)w : 0;
}

static void store_work_fn(struct k_work *work)
{
	struct persist p;
	uint32_t gen;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&lock, K_FOREVER);
	p = pending;
	gen = pending_gen;
	k_mutex_unlock(&lock);

	err = save(&p);

	k_mutex_lock(&lock, K_FOREVER);
	done_gen = gen;
	done_ok = err == 0;
	k_mutex_unlock(&lock);
	atomic_set(&done, 1);
	atomic_set(&busy, 0);
}

int store_init(void)
{
	const struct device *dev = FIXED_PARTITION_DEVICE(storage_partition);
	struct flash_pages_info info;
	int err;

	k_work_queue_start(&store_q, store_stack, K_THREAD_STACK_SIZEOF(store_stack),
			   K_PRIO_PREEMPT(8), NULL);
	k_thread_name_set(&store_q.thread, "store");
	k_work_init(&store_work, store_work_fn);

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	fs.flash_device = dev;
	fs.offset = FIXED_PARTITION_OFFSET(storage_partition);
	err = flash_get_page_info_by_offs(dev, fs.offset, &info);
	if (err) {
		return err;
	}
	fs.sector_size = info.size;
	fs.sector_count = FIXED_PARTITION_SIZE(storage_partition) / info.size;

	err = nvs_mount(&fs);
	if (err) {
		/* Unreadable (e.g. left over from other firmware): start over */
		safety_wdt_feed();
		err = nvs_clear(&fs);
		safety_wdt_feed();
		if (!err) {
			err = nvs_mount(&fs);
		}
	}
	mounted = err == 0;

	return err;
}

bool store_load(struct persist *out)
{
	uint8_t buf[PERSIST_ENCODED_MAX];
	ssize_t n;

	if (!mounted) {
		return false;
	}

	n = nvs_read(&fs, PERSIST_ID, buf, sizeof(buf));
	if (n <= 0 || n > (ssize_t)sizeof(buf)) {
		return false;
	}

	return persist_decode(out, buf, (size_t)n);
}

void store_request(const struct persist *p, uint32_t gen)
{
	k_mutex_lock(&lock, K_FOREVER);
	pending = *p;
	pending_gen = gen;
	k_mutex_unlock(&lock);

	atomic_set(&busy, 1);
	(void)k_work_submit_to_queue(&store_q, &store_work);
}

bool store_take_done(uint32_t *gen, bool *ok)
{
	if (!atomic_cas(&done, 1, 0)) {
		return false;
	}

	k_mutex_lock(&lock, K_FOREVER);
	*gen = done_gen;
	*ok = done_ok;
	k_mutex_unlock(&lock);

	return true;
}

bool store_busy(void)
{
	return atomic_get(&busy) != 0;
}

int store_save_now(const struct persist *p)
{
	struct k_work_sync sync;

	(void)k_work_flush(&store_work, &sync);

	return save(p);
}
