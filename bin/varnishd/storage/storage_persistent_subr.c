/*-
 * Copyright (c) 2008-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Persistent storage method
 *
 * XXX: Before we start the client or maybe after it stops, we should give the
 * XXX: stevedores a chance to examine their storage for consistency.
 *
 * XXX: Do we ever free the LRU-lists ?
 */

#include "config.h"

#include "cache/cache_varnishd.h"

#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>

#include "storage/storage.h"

#include "vrnd.h"
#include "vsha256.h"

#include "storage/storage_persistent.h"

static void smp_msync(const void *addr, size_t length);

/*--------------------------------------------------------------------
 * SIGNATURE functions
 * The signature is SHA256 over:
 *    1. The smp_sign struct up to but not including the length field.
 *    2. smp_sign->length bytes, starting after the smp_sign structure
 *    3. The smp-sign->length field.
 * The signature is stored after the byte-range from step 2.
 */

/*--------------------------------------------------------------------
 * Define a signature by location and identifier.
 */

void
smp_def_sign(const struct smp_sc *sc, struct smp_signctx *ctx,
    uint64_t off, const char *id)
{

	AZ(off & 7);			/* Alignment */
	assert(strlen(id) < sizeof ctx->ss->ident);

	memset(ctx, 0, sizeof *ctx);
	ctx->ss = (void*)(sc->base + off);
	ctx->unique = sc->unique;
	ctx->id = id;
}

/*--------------------------------------------------------------------
 * Check that a signature is good, leave state ready for append
 */
int
smp_chk_sign(struct smp_signctx *ctx)
{
	struct VSHA256Context cx;
	unsigned char sign[VSHA256_LEN];
	int r = 0;

	if (strncmp(ctx->id, ctx->ss->ident, sizeof ctx->ss->ident))
		r = 1;
	else if (ctx->unique != ctx->ss->unique)
		r = 2;
	else if (!ctx->ss->mapped)
		r = 3;
	else {
		VSHA256_Init(&ctx->ctx);
		VSHA256_Update(&ctx->ctx, ctx->ss,
		    offsetof(struct smp_sign, length));
		VSHA256_Update(&ctx->ctx, SIGN_DATA(ctx), ctx->ss->length);
		cx = ctx->ctx;
		VSHA256_Update(&cx, &ctx->ss->length, sizeof(ctx->ss->length));
		VSHA256_Final(sign, &cx);
		if (memcmp(sign, SIGN_END(ctx), sizeof sign))
			r = 4;
	}
	if (r) {
		fprintf(stderr, "CHK(%p %s %p %s) = %d\n",
		    ctx, ctx->id, ctx->ss,
		    r > 1 ? ctx->ss->ident : "<invalid>", r);
	}
	return (r);
}

/*--------------------------------------------------------------------
 * Append data to a signature
 */
static void
smp_append_sign(struct smp_signctx *ctx, const void *ptr, uint32_t len)
{
	struct VSHA256Context cx;
	unsigned char sign[VSHA256_LEN];

	if (len != 0) {
		VSHA256_Update(&ctx->ctx, ptr, len);
		ctx->ss->length += len;
	}
	cx = ctx->ctx;
	VSHA256_Update(&cx, &ctx->ss->length, sizeof(ctx->ss->length));
	VSHA256_Final(sign, &cx);
	memcpy(SIGN_END(ctx), sign, sizeof sign);
}

/*--------------------------------------------------------------------
 * Reset a signature to empty, prepare for appending.
 */

void
smp_reset_sign(struct smp_signctx *ctx)
{

	memset(ctx->ss, 0, sizeof *ctx->ss);
	assert(strlen(ctx->id) < sizeof *ctx->ss);
	strcpy(ctx->ss->ident, ctx->id);
	ctx->ss->unique = ctx->unique;
	ctx->ss->mapped = (uintptr_t)ctx->ss;
	VSHA256_Init(&ctx->ctx);
	VSHA256_Update(&ctx->ctx, ctx->ss,
	    offsetof(struct smp_sign, length));
	smp_append_sign(ctx, NULL, 0);
}

/*--------------------------------------------------------------------
 * Force a write of a signature block to the backing store.
 */

void
smp_sync_sign(const struct smp_signctx *ctx)
{
	smp_msync(ctx->ss, SMP_SIGN_SPACE + ctx->ss->length);
}

/*--------------------------------------------------------------------
 * Create and force a new signature to backing store
 */

static void
smp_new_sign(const struct smp_sc *sc, struct smp_signctx *ctx,
    uint64_t off, const char *id)
{
	smp_def_sign(sc, ctx, off, id);
	smp_reset_sign(ctx);
	smp_sync_sign(ctx);
}

/*--------------------------------------------------------------------
 * Define a signature space by location, size and identifier
 */

static void
smp_def_signspace(const struct smp_sc *sc, struct smp_signspace *spc,
		  uint64_t off, uint64_t size, const char *id)
{
	smp_def_sign(sc, &spc->ctx, off, id);
	spc->start = SIGN_DATA(&spc->ctx);
	spc->size = size - SMP_SIGN_SPACE;
}

/*--------------------------------------------------------------------
 * Check that a signspace's signature space is good, leave state ready
 * for append
 */

int
smp_chk_signspace(struct smp_signspace *spc)
{
	return (smp_chk_sign(&spc->ctx));
}

/*--------------------------------------------------------------------
 * Append data to a signature space
 */

void
smp_append_signspace(struct smp_signspace *spc, uint32_t len)
{
	assert(len <= SIGNSPACE_FREE(spc));
	smp_append_sign(&spc->ctx, SIGNSPACE_FRONT(spc), len);
}

/*--------------------------------------------------------------------
 * Reset a signature space to empty, prepare for appending.
 */

void
smp_reset_signspace(struct smp_signspace *spc)
{
	smp_reset_sign(&spc->ctx);
}

/*--------------------------------------------------------------------
 * Copy the contents of one signspace to another. Prepare for
 * appending.
 */

void
smp_copy_signspace(struct smp_signspace *dst, const struct smp_signspace *src)
{
	assert(SIGNSPACE_LEN(src) <= dst->size);
	smp_reset_signspace(dst);
	memcpy(SIGNSPACE_DATA(dst), SIGNSPACE_DATA(src), SIGNSPACE_LEN(src));
	smp_append_signspace(dst, SIGNSPACE_LEN(src));
	assert(SIGNSPACE_LEN(src) == SIGNSPACE_LEN(dst));
}

/*--------------------------------------------------------------------
 * Create a new signature space and force the signature to backing store.
 */

static void
smp_new_signspace(const struct smp_sc *sc, struct smp_signspace *spc,
		  uint64_t off, uint64_t size, const char *id)
{
	smp_new_sign(sc, &spc->ctx, off, id);
	spc->start = SIGN_DATA(&spc->ctx);
	spc->size = size - SMP_SIGN_SPACE;
}

/*--------------------------------------------------------------------
 * Force a write of a memory block (rounded to nearest pages) to
 * the backing store.
 */

static void
smp_msync(const void *addr, size_t length)
{
	uintptr_t start, end, pagesize;

	pagesize = getpagesize();
	assert(pagesize > 0 && PWR2(pagesize));
	start = RDN2((uintptr_t)addr, pagesize);
	end = RUP2((uintptr_t)addr + length, pagesize);
	assert(start < end);
	AZ(msync((void *)start, end - start, MS_SYNC));
}

/*--------------------------------------------------------------------
 * Initialize a Silo with a valid but empty structure.
 *
 * XXX: more intelligent sizing of things.
 */

void
smp_newsilo(struct smp_sc *sc)
{
	struct smp_ident	*si;

	/* Choose a new random number */
	AZ(VRND_RandomCrypto(&sc->unique, sizeof sc->unique));

	smp_reset_sign(&sc->idn);
	si = sc->ident;

	memset(si, 0, sizeof *si);
	bstrcpy(si->ident, SMP_IDENT_STRING);
	si->byte_order = 0x12345678;
	si->size = sizeof *si;
	si->major_version = 2;
	si->unique = sc->unique;
	si->mediasize = sc->mediasize;
	si->granularity = sc->granularity;
	/*
	 * Aim for cache-line-width
	 */
	si->align = sizeof(void*) * 2;
	sc->align = si->align;

	si->stuff[SMP_BAN1_STUFF] = sc->granularity;
	si->stuff[SMP_BAN2_STUFF] = si->stuff[SMP_BAN1_STUFF] + 1024*1024;
	si->stuff[SMP_SEG1_STUFF] = si->stuff[SMP_BAN2_STUFF] + 1024*1024;
	si->stuff[SMP_SEG2_STUFF] = si->stuff[SMP_SEG1_STUFF] + 1024*1024;
	si->stuff[SMP_SPC_STUFF] = si->stuff[SMP_SEG2_STUFF] + 1024*1024;
	si->stuff[SMP_END_STUFF] = si->mediasize;
	assert(si->stuff[SMP_SPC_STUFF] < si->stuff[SMP_END_STUFF]);

	smp_new_signspace(sc, &sc->ban1, si->stuff[SMP_BAN1_STUFF],
			  smp_stuff_len(sc, SMP_BAN1_STUFF), "BAN 1");
	smp_new_signspace(sc, &sc->ban2, si->stuff[SMP_BAN2_STUFF],
			  smp_stuff_len(sc, SMP_BAN2_STUFF), "BAN 2");
	smp_new_signspace(sc, &sc->seg1, si->stuff[SMP_SEG1_STUFF],
			  smp_stuff_len(sc, SMP_SEG1_STUFF), "SEG 1");
	smp_new_signspace(sc, &sc->seg2, si->stuff[SMP_SEG2_STUFF],
			  smp_stuff_len(sc, SMP_SEG2_STUFF), "SEG 2");

	smp_append_sign(&sc->idn, si, sizeof *si);
	smp_sync_sign(&sc->idn);
}

/*--------------------------------------------------------------------
 * Check if a silo is valid.
 */

int
smp_valid_silo(struct smp_sc *sc)
{
	struct smp_ident	*si;
	int i, j;

	assert(strlen(SMP_IDENT_STRING) < sizeof si->ident);

	i = smp_chk_sign(&sc->idn);
	if (i)
		return (i);

	si = sc->ident;
	if (strcmp(si->ident, SMP_IDENT_STRING))
		return (12);
	if (si->byte_order != 0x12345678)
		return (13);
	if (si->size != sizeof *si)
		return (14);
	if (si->major_version != 2)
		return (15);
	if (si->mediasize != sc->mediasize)
		return (17);
	if (si->granularity != sc->granularity)
		return (18);
	if (si->align < sizeof(void*))
		return (19);
	if (!PWR2(si->align))
		return (20);
	sc->align = si->align;
	sc->unique = si->unique;

	/* XXX: Sanity check stuff[6] */

	assert(si->stuff[SMP_BAN1_STUFF] > sizeof *si + VSHA256_LEN);
	assert(si->stuff[SMP_BAN2_STUFF] > si->stuff[SMP_BAN1_STUFF]);
	assert(si->stuff[SMP_SEG1_STUFF] > si->stuff[SMP_BAN2_STUFF]);
	assert(si->stuff[SMP_SEG2_STUFF] > si->stuff[SMP_SEG1_STUFF]);
	assert(si->stuff[SMP_SPC_STUFF] > si->stuff[SMP_SEG2_STUFF]);
	assert(si->stuff[SMP_END_STUFF] == sc->mediasize);

	assert(smp_stuff_len(sc, SMP_SEG1_STUFF) > 65536);
	assert(smp_stuff_len(sc, SMP_SEG1_STUFF) ==
	  smp_stuff_len(sc, SMP_SEG2_STUFF));

	assert(smp_stuff_len(sc, SMP_BAN1_STUFF) > 65536);
	assert(smp_stuff_len(sc, SMP_BAN1_STUFF) ==
	  smp_stuff_len(sc, SMP_BAN2_STUFF));

	smp_def_signspace(sc, &sc->ban1, si->stuff[SMP_BAN1_STUFF],
			  smp_stuff_len(sc, SMP_BAN1_STUFF), "BAN 1");
	smp_def_signspace(sc, &sc->ban2, si->stuff[SMP_BAN2_STUFF],
			  smp_stuff_len(sc, SMP_BAN2_STUFF), "BAN 2");
	smp_def_signspace(sc, &sc->seg1, si->stuff[SMP_SEG1_STUFF],
			  smp_stuff_len(sc, SMP_SEG1_STUFF), "SEG 1");
	smp_def_signspace(sc, &sc->seg2, si->stuff[SMP_SEG2_STUFF],
			  smp_stuff_len(sc, SMP_SEG2_STUFF), "SEG 2");

	/* We must have one valid BAN table */
	i = smp_chk_signspace(&sc->ban1);
	j = smp_chk_signspace(&sc->ban2);
	if (i && j)
		return (100 + i * 10 + j);

	/* We must have one valid SEG table */
	i = smp_chk_signspace(&sc->seg1);
	j = smp_chk_signspace(&sc->seg2);
	if (i && j)
		return (200 + i * 10 + j);
	return (0);
}
