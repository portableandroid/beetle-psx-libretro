/*
 * Copyright (C) 2015 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "blockcache.h"
#include "debug.h"
#include "lightrec-private.h"
#include "memmanager.h"

#include <stdbool.h>
#include <stdlib.h>

/* Must be power of two */
#define TINY_LUT_SIZE 0x100
#define LUT_SIZE 0x4000

struct blockcache {
	struct block * tiny_lut[TINY_LUT_SIZE];
	struct block * lut[LUT_SIZE];
};

struct block * lightrec_find_block(struct blockcache *cache, u32 pc)
{
	struct block *block;

	pc = kunseg(pc);

	block = cache->tiny_lut[(pc >> 2) & (TINY_LUT_SIZE - 1)];
	if (likely(block && block->kunseg_pc == pc))
		return block;

	block = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	for (block = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	     block; block = block->next) {
		if (block->kunseg_pc == pc) {
			cache->tiny_lut[(pc >> 2) & (TINY_LUT_SIZE - 1)] = block;
			return block;
		}
	}

	return NULL;
}

static void remove_from_code_lut(struct blockcache *cache, struct block *block)
{
	struct lightrec_state *state = block->state;

	/* Use state->get_next_block in the code LUT, which basically
	 * calls back get_next_block_func(), until the compiler
	 * overrides this. This is required, as a NULL value in the code
	 * LUT means an outdated block. */
	if (block->map == &state->maps[PSX_MAP_KERNEL_USER_RAM])
		state->code_lut[block->kunseg_pc >> 2] = state->get_next_block;
}

void lightrec_mark_for_recompilation(struct blockcache *cache,
				     struct block *block)
{
	block->flags |= BLOCK_SHOULD_RECOMPILE;

	remove_from_code_lut(cache, block);
}

void lightrec_register_block(struct blockcache *cache, struct block *block)
{
	u32 pc = block->kunseg_pc;
	struct block *old;

	old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	if (old)
		block->next = old;

	cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = block;
	cache->tiny_lut[(pc >> 2) & (TINY_LUT_SIZE - 1)] = block;

	remove_from_code_lut(cache, block);
}

void lightrec_unregister_block(struct blockcache *cache, struct block *block)
{
	u32 pc = block->kunseg_pc;
	struct block *old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];

	if (block->map == &block->state->maps[PSX_MAP_KERNEL_USER_RAM])
		block->state->code_lut[pc >> 2] = NULL;

	cache->tiny_lut[(pc >> 2) & (TINY_LUT_SIZE - 1)] = NULL;

	if (old == block) {
		cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = old->next;
		return;
	}

	for (; old; old = old->next) {
		if (old->next == block) {
			old->next = block->next;
			return;
		}
	}

	pr_err("Block at PC 0x%x is not in cache\n", block->pc);
}

void lightrec_free_block_cache(struct blockcache *cache)
{
	struct block *block, *next;
	unsigned int i;

	for (i = 0; i < LUT_SIZE; i++) {
		for (block = cache->lut[i]; block; block = next) {
			next = block->next;
			lightrec_free_block(block);
		}
	}

	lightrec_free(MEM_FOR_LIGHTREC, sizeof(*cache), cache);
}

struct blockcache * lightrec_blockcache_init(void)
{
	return lightrec_calloc(MEM_FOR_LIGHTREC, sizeof(struct blockcache));
}

bool lightrec_block_is_outdated(struct block *block)
{
	return (block->map == &block->state->maps[PSX_MAP_KERNEL_USER_RAM]) &&
		!block->state->code_lut[block->kunseg_pc >> 2];
}
