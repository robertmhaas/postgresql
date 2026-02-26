/*-------------------------------------------------------------------------
 *
 * pg_collect_advice.h
 *	  definitions and declarations for pg_collect_advice module
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_collect_advice/pg_collect_advice.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_COLLECT_ADVICE_H
#define PG_COLLECT_ADVICE_H

#include "storage/lwlock.h"
#include "utils/dsa.h"

typedef struct pgca_shared_state
{
	LWLock		lock;
	int			dsa_tranche;
	dsa_handle	area;
	dsa_pointer shared_collector;
} pgca_shared_state;

/* GUC variables */
extern bool pg_collect_advice_local_collector;
extern int	pg_collect_advice_local_collection_limit;
extern bool pg_collect_advice_shared_collector;
extern int	pg_collect_advice_shared_collection_limit;

/* Function prototypes */
extern MemoryContext pg_collect_advice_get_mcxt(void);
extern pgca_shared_state *pg_collect_advice_attach(void);
extern dsa_area *pg_collect_advice_dsa_area(void);
extern void pg_collect_advice_save(uint64 queryId, const char *query_string,
								   const char *advice_string);

#endif
