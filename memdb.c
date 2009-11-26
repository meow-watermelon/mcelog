/* Copyright (C) 2009 Intel Corporation 
   Author: Andi Kleen
   Simple in memory error database for mcelog running in daemon mode

   mcelog is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; version
   2.

   mcelog is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should find a copy of v2 of the GNU General Public License somewhere
   on your Linux system; if not, write to the Free Software Foundation, 
   Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA */
#define _GNU_SOURCE 1
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "mcelog.h"
#include "memutil.h"
#include "config.h"
#include "dmi.h"
#include "memdb.h"
#include "leaky-bucket.h"
#include "trigger.h"
#include "intel.h"
#include "page.h"

struct memdimm {
	struct memdimm *next;
	int channel;			/* -1: unknown */
	int dimm;			/* -1: unknown */
	int socketid;
	struct err_type ce;
	struct err_type uc;
	char *name;
	char *location;
	struct dmi_memdev *memdev;
};

#define SHASH 17

static int md_numdimms;
static struct memdimm *md_dimms[SHASH];

static struct bucket_conf ce_bucket_conf;
static struct bucket_conf uc_bucket_conf;

static int memdb_enabled;

#define FNV32_OFFSET 2166136261
#define FNV32_PRIME 0x01000193
#define O(x) ((x) & 0xff)

/* FNV 1a 32bit, max 16k sockets, 8bit dimm/channel */
static unsigned dimmhash(unsigned socket, int dimm, unsigned ch)
{
        unsigned hash = FNV32_OFFSET;
	hash = (hash ^ O(socket)) * FNV32_PRIME;
	hash = (hash ^ O(socket >> 8)) * FNV32_PRIME;
	hash = (hash ^ O(dimm)) * FNV32_PRIME;
	hash = (hash ^ O(ch)) * FNV32_PRIME;
        return hash % SHASH;
}

/* Search DIMM in hash table */
struct memdimm *get_memdimm(int socketid, int channel, int dimm)
{
	struct memdimm *md;
	unsigned h = dimmhash(socketid, dimm, channel);

	for (md = md_dimms[h]; md; md = md->next) { 
		if (md->socketid == socketid && 
			md->channel == channel && 
			md->dimm == dimm)
			break;	
	}
	if (md)
		return md;

	md = xalloc(sizeof(struct memdimm));
	md->next = md_dimms[h];
	md_dimms[h] = md;
	md->socketid = socketid;
	md->channel = channel;
	md->dimm = dimm;
	md_numdimms++;
	bucket_init(&md->ce.bucket);
	bucket_init(&md->uc.bucket);
	return md;
}

enum {
	NUMLEN  = 30,
	MAX_ENV = 20,
};

static char *number(char *buf, long number)
{
	snprintf(buf, NUMLEN, "%ld", number);
	return buf;
}

static char *format_location(struct memdimm *md)
{
	char numbuf[NUMLEN], numbuf2[NUMLEN];
	char *location;

	asprintf(&location, "SOCKET:%d CHANNEL:%s DIMM:%s [%s%s%s]",
		md->socketid, 
		md->channel == -1 ? "?" : number(numbuf, md->channel),
		md->dimm == -1 ? "?" : number(numbuf2, md->dimm),
		md->location ? md->location : "",
		md->location && md->name ? " " : "",
		md->name ? md->name : ""); 
	return location;
}

/* Run a user defined trigger when a error threshold is crossed. */
void memdb_trigger(char *msg, struct memdimm *md,  time_t t,
		struct err_type *et, struct bucket_conf *bc)
{
	struct leaky_bucket *bucket = &et->bucket;
	char *env[MAX_ENV]; 
	int ei = 0;
	int i;
	char *location = format_location(md);
	char *output = bucket_output(bc, bucket);

	Gprintf("%s: %s\n", msg, output); 
	Gprintf("Location %s\n", location);
	if (bc->trigger == NULL)
		return;
	asprintf(&env[ei++], "PATH=%s", getenv("PATH") ?: "/sbin:/usr/sbin:/bin:/usr/bin");
	asprintf(&env[ei++], "THRESHOLD=%s", output);
	asprintf(&env[ei++], "TOTALCOUNT=%lu", et->count);
	asprintf(&env[ei++], "LOCATION=%s", location);
	if (md->location)
		asprintf(&env[ei++], "DMI_LOCATION=%s", md->location);
	if (md->name)
		asprintf(&env[ei++], "DMI_NAME=%s", md->name);
	if (md->dimm != -1)
		asprintf(&env[ei++], "DIMM=%d", md->dimm);
	if (md->channel != -1)
		asprintf(&env[ei++], "CHANNEL=%d", md->channel);
	asprintf(&env[ei++], "SOCKETID=%d", md->socketid);
	asprintf(&env[ei++], "CECOUNT=%lu", md->ce.count);
	asprintf(&env[ei++], "UCCOUNT=%lu", md->uc.count);
	if (t)
		asprintf(&env[ei++], "LASTEVENT=%lu", t);
	asprintf(&env[ei++], "AGETIME=%u", bc->agetime);
	// XXX human readable version of agetime
	asprintf(&env[ei++], "MESSAGE=%s", msg);
	asprintf(&env[ei++], "THRESHOLD_COUNT=%d", bucket->count + bucket->excess);
	env[ei] = NULL;	
	assert(ei < MAX_ENV);
	run_trigger(bc->trigger, NULL, env);
	for (i = 0; i < ei; i++)
		free(env[i]);
	free(location);
	free(output);
}

/* 
 * A memory error happened, record it in the memdb database and run
 * triggers if needed.
 * ch/dimm == -1: Unspecified DIMM on the channel
 */
void memory_error(struct mce *m, int ch, int dimm, unsigned corr_err_cnt)
{
	time_t t;
	struct memdimm *md;
	char *msg;

	if (!memdb_enabled)
		return;

	t = m->time ? (time_t)m->time : time(NULL);
	md = get_memdimm(m->socketid, ch, dimm);

	if (corr_err_cnt && --corr_err_cnt > 0) {
		/* Lost some errors. Assume they were CE */
		md->ce.count += corr_err_cnt;
		if (__bucket_account(&ce_bucket_conf, &md->ce.bucket, corr_err_cnt, t)) { 
			asprintf(&msg, "Lost DIMM memory error count %d exceeded threshold", 
				 corr_err_cnt);
			memdb_trigger(msg, md, 0, &md->ce, &ce_bucket_conf);
			free(msg);
		}
	}

	if (m->status & MCI_STATUS_UC) { 
		md->uc.count++;
		if (__bucket_account(&uc_bucket_conf, &md->uc.bucket, 1, t))
			memdb_trigger("Uncorrected DIMM memory error count exceeded threshold", 
				      md, t, &md->uc, &uc_bucket_conf);
	} else {
		md->ce.count++;
		if (__bucket_account(&ce_bucket_conf, &md->ce.bucket, 1, t))
			memdb_trigger("Corrected DIMM memory error count exceeded threshold", 
				      md, t, &md->ce, &ce_bucket_conf);
	}
}

/* Compare two dimms for sorting. */
static int cmp_dimm(const void *a, const void *b)
{
	const struct memdimm *ma = *(void **)a;
	const struct memdimm *mb = *(void **)b;
	if (ma->socketid != mb->socketid)
		return ma->socketid - mb->socketid;
	if (ma->channel != mb->channel)
		return ma->channel - mb->channel;
	return ma->dimm - mb->dimm;
}

/* Dump CE or UC errors */
static void dump_errtype(char *name, struct err_type *e, FILE *f, enum printflags flags,
			 struct bucket_conf *bc)
{
	int all = (flags & DUMP_ALL);
	char *s;

	if (e->count || e->bucket.count || all)
		fprintf(f, "%s:\n", name);
	if (e->count || all) {
		fprintf(f, "\t%lu total\n", e->count);
	}
	if (e->bucket.count || all) {
		s = bucket_output(bc, &e->bucket);
		fprintf(f, "\t%s\n", s);  
		free(s);
	}
}

static void dump_bios(struct memdimm *md, FILE *f)
{
	int n = 0;

	if (md->name)
		n += fprintf(f, "DMI_NAME \"%s\"", md->name);
	if (md->location) { 
		if (n > 0)
			fputc(' ', f);
		n += fprintf(f, "DMI_LOCATION \"%s\"", md->location);
	}
	if (n > 0)
		fputc('\n', f);
}

static void dump_dimm(struct memdimm *md, FILE *f, enum printflags flags)
{
	if (md->ce.count + md->uc.count > 0 || (flags & DUMP_ALL)) {
		fprintf(f, "SOCKET %u", md->socketid);
		if (md->channel == -1)
			fprintf(f, " CHANNEL unknown");
		else
			fprintf(f, " CHANNEL %d", md->channel);
		if (md->dimm == -1) 
			fprintf(f, " DIMM unknown");
		else
			fprintf(f, " DIMM %d", md->dimm);
		fputc('\n', f);

		if (flags & DUMP_BIOS)
			dump_bios(md, f);
		dump_errtype("corrected memory errors", &md->ce, f, flags, 
				&ce_bucket_conf);
		dump_errtype("uncorrected memory errors", &md->uc, f, flags, 
				&uc_bucket_conf);
	}
}

/* Sort and dump DIMMs */
void dump_memory_errors(FILE *f, enum printflags flags)
{
	int i, k;
	struct memdimm *md, **da;

	da = xalloc(sizeof(void *) * md_numdimms);
	k = 0;
	for (i = 0; i < SHASH; i++) {
		for (md = md_dimms[i]; md; md = md->next)
			da[k++] = md;
	}
	qsort(da, md_numdimms, sizeof(void *), cmp_dimm);
	for (i = 0; i < md_numdimms; i++)  {
		if (i > 0) 
			fputc('\n', f);
		dump_dimm(da[i], f, flags);
	}
	free(da);
}

void memdb_config(void)
{
	int n;

	n = config_bool("dimm", "dimm-tracking-enabled");
	if (n < 0) 
		memdb_enabled = memory_error_support;
	else
		memdb_enabled = n; 

	config_trigger("dimm", "ce-error", &ce_bucket_conf);
	config_trigger("dimm", "uc-error", &uc_bucket_conf);
}

/* Prepopulate DIMM database from BIOS information */
void prefill_memdb(void)
{
	static int initialized;
	int i;
	int missed = 0;
	unsigned socketid, channel, dimm;

	if (initialized)
		return;
	memdb_config();
	if (!memdb_enabled)
		return;
	initialized = 1;
	if (config_bool("dimm", "dmi-prepopulate") == 0)
		return;
	if (opendmi() < 0)
		return;

	for (i = 0; dmi_dimms[i]; i++) {
		struct memdimm *md;
		struct dmi_memdev *d = dmi_dimms[i];
		char *bl;

		bl = dmi_getstring(&d->header, d->bank_locator);
		if (sscanf(bl + strcspn(bl, "_"), "_Node%u_Channel%u_Dimm%u", &socketid, 
				&channel, &dimm) != 3) {
			missed++;
			continue;
		}

		md = get_memdimm(socketid, channel, dimm);
		if (md->memdev) { 
			/* dups -- likely parse error */
			missed++;
			continue;
		}
		md->memdev = d;
		md->location = bl;
		md->name = dmi_getstring(&d->header, d->device_locator);
	}
	if (missed) { 
		Eprintf("failed to prefill DIMM database from DMI data");
	}
}