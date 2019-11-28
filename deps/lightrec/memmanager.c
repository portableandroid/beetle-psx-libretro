/*
 * Copyright (C) 2019 Paul Cercueil <paul@crapouillou.net>
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

#include "config.h"
#include "memmanager.h"

#include <stdlib.h>

#ifdef ENABLE_THREADED_COMPILER
#include <stdatomic.h>

static atomic_uint lightrec_bytes[MEM_TYPE_END];

void lightrec_register(enum mem_type type, unsigned int len)
{
	atomic_fetch_add(&lightrec_bytes[type], len);
}

void lightrec_unregister(enum mem_type type, unsigned int len)
{
	atomic_fetch_sub(&lightrec_bytes[type], len);
}

unsigned int lightrec_get_mem_usage(enum mem_type type)
{
	return atomic_load(&lightrec_bytes[type]);
}

#else /* ENABLE_THREADED_COMPILER */

static unsigned int lightrec_bytes[MEM_TYPE_END];

void lightrec_register(enum mem_type type, unsigned int len)
{
	lightrec_bytes[type] += len;
}

void lightrec_unregister(enum mem_type type, unsigned int len)
{
	lightrec_bytes[type] -= len;
}

unsigned int lightrec_get_mem_usage(enum mem_type type)
{
	return lightrec_bytes[type];
}
#endif /* ENABLE_THREADED_COMPILER */

unsigned int lightrec_get_total_mem_usage(void)
{
	unsigned int i, count;

	for (i = 0, count = 0; i < MEM_TYPE_END; i++)
		count += lightrec_get_mem_usage((enum mem_type)i);

	return count;
}

void * lightrec_malloc(enum mem_type type, unsigned int len)
{
	void *ptr;

	ptr = malloc(len);
	if (!ptr)
		return NULL;

	lightrec_register(type, len);

	return ptr;
}

void * lightrec_calloc(enum mem_type type, unsigned int len)
{
	void *ptr;

	ptr = calloc(1, len);
	if (!ptr)
		return NULL;

	lightrec_register(type, len);

	return ptr;
}

void lightrec_free(enum mem_type type, unsigned int len, void *ptr)
{
	lightrec_unregister(type, len);
	free(ptr);
}

float lightrec_get_average_ipi(void)
{
	unsigned int code_mem = lightrec_get_mem_usage(MEM_FOR_CODE);
	unsigned int native_mem = lightrec_get_mem_usage(MEM_FOR_MIPS_CODE);

	return native_mem ? (float)code_mem / (float)native_mem : 0.0f;
}
