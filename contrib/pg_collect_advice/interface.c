/*-------------------------------------------------------------------------
 *
 * interface.c
 *	  interface routines for the plan advice collector
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_collect_advice/interface.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pg_collect_advice.h"

#include "funcapi.h"
#include "optimizer/planner.h"
#include "storage/dsm_registry.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Shared memory pointers */
static pgca_shared_state *pgca_state = NULL;
static dsa_area *pgca_dsa_area = NULL;

/* GUC variables */
bool		pg_collect_advice_local_collector = false;
int			pg_collect_advice_local_collection_limit = 0;
bool		pg_collect_advice_shared_collector = false;
int			pg_collect_advice_shared_collection_limit = 0;

/* Shadow variables for GUC assign hooks */
static bool pg_collect_advice_local_collector_as_assigned = false;
static bool pg_collect_advice_shared_collector_as_assigned = false;

/* Other file-level globals */
static void (*request_advice_generation_fn) (bool activate) = NULL;
static planner_shutdown_hook_type prev_planner_shutdown = NULL;
static MemoryContext pgca_memory_context = NULL;

/* Function prototypes */
static void pgca_init_shared_state(void *ptr, void *arg);
static void pgca_planner_shutdown(PlannerGlobal *glob, Query *parse,
								  const char *query_string,
								  PlannedStmt *pstmt);
static void pg_collect_advice_local_collector_assign_hook(bool newval,
														  void *extra);
static void pg_collect_advice_shared_collector_assign_hook(bool newval,
														   void *extra);
static DefElem *find_defelem_by_defname(List *deflist, char *defname);

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	/*
	 * Get a pointer so we can call pg_plan_advice_request_advice_generation.
	 *
	 * We need to do this before defining custom GUCs; otherwise, our assign
	 * hook will try to use this function pointer before it's initialized.
	 *
	 * We also need to do this before installing our own hooks, so that if
	 * pg_plan_advice is not yet loaded, it will install its hooks before we
	 * install ours. (See comments in pgca_planner_shutdown.)
	 */
	request_advice_generation_fn =
		load_external_function("pg_plan_advice",
							   "pg_plan_advice_request_advice_generation",
							   true, NULL);

	/* Define our GUCs. */
	DefineCustomBoolVariable("pg_collect_advice.local_collector",
							 "Enable the local advice collector.",
							 NULL,
							 &pg_collect_advice_local_collector,
							 false,
							 PGC_USERSET,
							 0,
							 NULL,
							 pg_collect_advice_local_collector_assign_hook,
							 NULL);

	DefineCustomIntVariable("pg_collect_advice.local_collection_limit",
							"# of advice entries to retain in per-backend memory",
							NULL,
							&pg_collect_advice_local_collection_limit,
							0,
							0, INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomBoolVariable("pg_collect_advice.shared_collector",
							 "Enable the shared advice collector.",
							 NULL,
							 &pg_collect_advice_shared_collector,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 pg_collect_advice_shared_collector_assign_hook,
							 NULL);

	DefineCustomIntVariable("pg_collect_advice.shared_collection_limit",
							"# of advice entries to retain in shared memory",
							NULL,
							&pg_collect_advice_shared_collection_limit,
							0,
							0, INT_MAX,
							PGC_SUSET,
							0,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("pg_collect_advice");

	/* Install hooks */
	prev_planner_shutdown = planner_shutdown_hook;
	planner_shutdown_hook = pgca_planner_shutdown;
}

/*
 * Initialize shared state when first created.
 */
static void
pgca_init_shared_state(void *ptr, void *arg)
{
	pgca_shared_state *state = (pgca_shared_state *) ptr;

	LWLockInitialize(&state->lock,
					 LWLockNewTrancheId("pg_collect_advice_lock"));
	state->dsa_tranche = LWLockNewTrancheId("pg_collect_advice_dsa");
	state->area = DSA_HANDLE_INVALID;
	state->shared_collector = InvalidDsaPointer;
}

/*
 * Return a pointer to a memory context where long-lived data managed by this
 * module can be stored.
 */
MemoryContext
pg_collect_advice_get_mcxt(void)
{
	if (pgca_memory_context == NULL)
		pgca_memory_context = AllocSetContextCreate(TopMemoryContext,
													"pg_collect_advice",
													ALLOCSET_DEFAULT_SIZES);

	return pgca_memory_context;
}

/*
 * Get a pointer to our shared state.
 *
 * If no shared state exists, create and initialize it. If it does exist but
 * this backend has not yet accessed it, attach to it. Otherwise, just return
 * our cached pointer.
 */
pgca_shared_state *
pg_collect_advice_attach(void)
{
	if (pgca_state == NULL)
	{
		bool		found;

		pgca_state =
			GetNamedDSMSegment("pg_collect_advice", sizeof(pgca_shared_state),
							   pgca_init_shared_state, &found, NULL);
	}

	return pgca_state;
}

/*
 * Return a pointer to pg_collect_advice's DSA area, creating it if needed.
 */
dsa_area *
pg_collect_advice_dsa_area(void)
{
	if (pgca_dsa_area == NULL)
	{
		pgca_shared_state *state = pg_collect_advice_attach();
		dsa_handle	area_handle;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(pg_collect_advice_get_mcxt());

		LWLockAcquire(&state->lock, LW_EXCLUSIVE);
		area_handle = state->area;
		if (area_handle == DSA_HANDLE_INVALID)
		{
			pgca_dsa_area = dsa_create(state->dsa_tranche);
			dsa_pin(pgca_dsa_area);
			state->area = dsa_get_handle(pgca_dsa_area);
			LWLockRelease(&state->lock);
		}
		else
		{
			LWLockRelease(&state->lock);
			pgca_dsa_area = dsa_attach(area_handle);
		}

		dsa_pin_mapping(pgca_dsa_area);

		MemoryContextSwitchTo(oldcontext);
	}

	return pgca_dsa_area;
}

/*
 * After planning is complete, retrieve the advice string, if present, and
 * pass it through to the collector.
 */
static void
pgca_planner_shutdown(PlannerGlobal *glob, Query *parse,
					  const char *query_string, PlannedStmt *pstmt)
{
	DefElem    *pgpa_item;
	DefElem    *advice_string_item;
	char	   *advice_string;

	/*
	 * Pass call to previous hook.
	 *
	 * We want to be called after pg_plan_advice's shutdown hook has already
	 * executed. Our _PG_init() makes sure that pg_plan_advice's hooks are
	 * always loaded before ours, and here we pass the hook call down first,
	 * before doing our own work. The combination of those two things should
	 * be good enough to ensure that the advice string is already present when
	 * we go looking for it.
	 */
	if (prev_planner_shutdown)
		(*prev_planner_shutdown) (glob, parse, query_string, pstmt);

	/* Fish out the advice string. If not found, do nothing. */
	pgpa_item = find_defelem_by_defname(pstmt->extension_state,
										"pg_plan_advice");
	if (pgpa_item == NULL)
		return;
	advice_string_item = find_defelem_by_defname((List *) pgpa_item->arg,
												 "advice_string");
	if (advice_string_item == NULL)
		return;
	advice_string = strVal(advice_string_item->arg);

	/*
	 * Pass it through to the actual collector. But, if it's the empty string,
	 * we assume that collecting it is uninteresting.
	 */
	if (advice_string[0] != '\0')
		pg_collect_advice_save(pstmt->queryId, query_string, advice_string);
}

/*
 * pgca_planner_shutdown won't find any advice to collect unless we've
 * requested that it be generated. So, whenever the effective value of
 * pg_collect_advice.local_collector changes, either make or
 * revoke a request for advice generation.
 */
static void
pg_collect_advice_local_collector_assign_hook(bool newval, void *extra)
{
	if (pg_collect_advice_local_collector_as_assigned && !newval)
		(*request_advice_generation_fn) (false);
	if (!pg_collect_advice_local_collector_as_assigned && newval)
		(*request_advice_generation_fn) (true);
	pg_collect_advice_local_collector_as_assigned = newval;
}

/*
 * Same as above, but for pg_collect_advice.shared_collector
 */
static void
pg_collect_advice_shared_collector_assign_hook(bool newval, void *extra)
{
	if (pg_collect_advice_shared_collector_as_assigned && !newval)
		(*request_advice_generation_fn) (false);
	if (!pg_collect_advice_shared_collector_as_assigned && newval)
		(*request_advice_generation_fn) (true);
	pg_collect_advice_shared_collector_as_assigned = newval;
}

/*
 * Search a list of DefElem objects for a given defname.
 */
static DefElem *
find_defelem_by_defname(List *deflist, char *defname)
{
	foreach_node(DefElem, item, deflist)
	{
		if (strcmp(item->defname, defname) == 0)
			return item;
	}

	return NULL;
}
