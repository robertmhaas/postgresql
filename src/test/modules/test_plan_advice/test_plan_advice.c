/*-------------------------------------------------------------------------
 *
 * test_plan_advice.c
 *	  Test pg_plan_advice by planning every query with generated advice.
 *
 * With this module loaded, every time a query is executed, we end up
 * planning it twice. The first time we plan it, we generate plan advice,
 * which we then feed back to pg_plan_advice as the supplied plan advice.
 * It is then planned a second time using that advice. This hopefully
 * allows us to detect cases where the advice is incorrect or causes
 * failures or plan changes for some reason.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  src/test/modules/test_plan_advice/test_plan_advice.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "pg_plan_advice.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

static int	test_plan_advice_max_attempts = 1;
static bool in_recursion = false;
static void (*feedback_warning_fn) (List *feedback);
static planner_hook_type prev_planner_hook = NULL;

static PlannedStmt *test_plan_advice_planner(Query *parse,
											 const char *query_string,
											 int cursorOptions,
											 ParamListInfo boundParams,
											 ExplainState *es);
static char *test_plan_advice_advisor(PlannerGlobal *glob,
									  Query *parse,
									  const char *query_string,
									  int cursorOptions,
									  ExplainState *es);
static PlannedStmt *copy_and_plan_query(Query *parse,
										const char *query_string,
										int cursorOptions,
										ParamListInfo boundParams,
										ExplainState *es,
										bool suppress_messages);
static List *extract_feedback(PlannedStmt *pstmt);
static bool all_feedback_is_ok(List *feedback);
static DefElem *find_defelem_by_defname(List *deflist, char *defname);

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	void		(*add_advisor_fn) (pg_plan_advice_advisor_hook hook);

	DefineCustomIntVariable("test_plan_advice.max_attempts",
							"Maximum number of planning attempts before "
							"reporting feedback warnings.",
							NULL,
							&test_plan_advice_max_attempts,
							1, 1, 10,
							PGC_USERSET,
							0, NULL, NULL, NULL);

	MarkGUCPrefixReserved("test_plan_advice");

	/* Install our planner hook. */
	prev_planner_hook = planner_hook;
	planner_hook = test_plan_advice_planner;

	/*
	 * Ask pg_plan_advice to get advice strings from test_plan_advice_advisor
	 */
	add_advisor_fn =
		load_external_function("pg_plan_advice", "pg_plan_advice_add_advisor",
							   true, NULL);

	(*add_advisor_fn) (test_plan_advice_advisor);

	/*
	 * Get a pointer to pg_plan_advice's function for emitting feedback
	 * warnings.
	 */
	feedback_warning_fn =
		load_external_function("pg_plan_advice",
							   "pgpa_planner_feedback_warning",
							   true, NULL);
}

/*
 * Planner hook that retries planning when feedback indicates a problem.
 *
 * When the catalog changes between the first plan (which generates advice)
 * and the second plan (which uses that advice), the advice can reference
 * objects that no longer exist or reflect stale statistics.  To avoid
 * spurious warnings, we retry planning up to test_plan_advice.max_attempts
 * times.  If the feedback stabilizes (i.e. is the same as the previous
 * attempt), we conclude the problem is genuine and emit warnings.
 */
static PlannedStmt *
test_plan_advice_planner(Query *parse, const char *query_string,
						 int cursorOptions, ParamListInfo boundParams,
						 ExplainState *es)
{
	PlannedStmt *pstmt;
	List	   *feedback;
	List	   *prev_feedback = NIL;

	for (int i = 0; i < test_plan_advice_max_attempts; ++i)
	{
		/*
		 * Try planning the query. On the first iteration, we don't need or
		 * want to suppress any warnings or other chatter that the planner is
		 * going to generate, because our goal here is to get the same output
		 * that would have occurred without this module. But on the second and
		 * later iterations, that output has already been produced, so we
		 * don't want it to appear again.
		 */
		pstmt = copy_and_plan_query(parse, query_string, cursorOptions,
									boundParams, es, (i > 0));

		/* Extract feedback. */
		feedback = extract_feedback(pstmt);

		/* If no problems were detected, stop. */
		if (all_feedback_is_ok(feedback))
			break;

		/*
		 * If the feedback is the same as last time, then apparently there's
		 * a real problem, so emit warnings and stop. If this is the last
		 * iteration, it's less clear that there's a real problem, but if not,
		 * the user hasn't set the maximum number of retries high enough, so
		 * handle that case the same way.
		 */
		if (equal(feedback, prev_feedback) ||
			i == test_plan_advice_max_attempts - 1)
		{
			(*feedback_warning_fn) (feedback);
			break;
		}

		/*
		 * Go around and try it again, with the newly-generated feedback as
		 * the new point of comparison.
		 */
		prev_feedback = feedback;
	}

	return pstmt;
}

/*
 * Re-plan the given query and return the generated advice string as the
 * supplied advice.
 */
static char *
test_plan_advice_advisor(PlannerGlobal *glob, Query *parse,
						 const char *query_string, int cursorOptions,
						 ExplainState *es)
{
	PlannedStmt *pstmt;
	DefElem    *pgpa_item;
	DefElem    *advice_string_item;

	/*
	 * Since this function is called from the planner and triggers planning,
	 * we need a recursion guard.
	 */
	if (in_recursion)
		return NULL;

	/*
	 * Try planning the query, generating advice in the process. We ask
	 * copy_and_plan_query to adjust client_min_messages; otherwise, any
	 * messages that are generated during planning would appear here and again
	 * when the query is replanned with the advice string.
	 */
	PG_TRY();
	{
		in_recursion = true;

		pstmt = copy_and_plan_query(parse, query_string, cursorOptions,
									glob->boundParams, es, true);
	}
	PG_FINALLY();
	{
		in_recursion = false;
	}
	PG_END_TRY();

	/* Extract and return the advice string */
	pgpa_item = find_defelem_by_defname(pstmt->extension_state,
										"pg_plan_advice");
	if (pgpa_item == NULL)
		elog(ERROR, "extension state for pg_plan_advice not found");
	advice_string_item = find_defelem_by_defname((List *) pgpa_item->arg,
												 "advice_string");
	if (advice_string_item == NULL)
		elog(ERROR,
			 "advice string for pg_plan_advice not found in extension state");
	return strVal(advice_string_item->arg);
}

/*
 * Wrapper around the main query planner.
 */
static PlannedStmt *
copy_and_plan_query(Query *parse, const char *query_string, int cursorOptions,
					ParamListInfo boundParams, ExplainState *es,
					bool suppress_messages)
{
	int			save_nestlevel = 0;
	PlannedStmt *pstmt;

	/*
	 * Temporarily set pg_plan_advice.always_store_advice_details. Either
	 * we're being called to generate advice, in which case setting this GUC
	 * is important to make sure that we do, or we're being called to see
	 * whether supplied advice applied properly, in which case this is needed
	 * so that pg_plan_advice will provide feedback.
	 */
	save_nestlevel = NewGUCNestLevel();
	set_config_option("pg_plan_advice.always_store_advice_details",
					  "true",
					  PGC_SUSET, PGC_S_SESSION,
					  GUC_ACTION_SAVE, true, 0, false);

	/*
	 * Planning can trigger expression evaluation, which can result in sending
	 * NOTICE messages or other output to the client. To avoid that, we allow
	 * the caller to request client_min_messages = ERROR in the hopes of
	 * getting the same output with and without this module.
	 */
	if (suppress_messages)
		set_config_option("client_min_messages", "error",
						  PGC_SUSET, PGC_S_SESSION,
						  GUC_ACTION_SAVE, true, 0, false);

	/*
	 * We must copy the Query, because the planner modifies it, and we intend
	 * to plan it multiple times. (As noted elsewhere, that's unfortunate;
	 * perhaps it will be fixed some day.)
	 */
	if (prev_planner_hook)
		pstmt = (*prev_planner_hook) (copyObject(parse), query_string,
									  cursorOptions, boundParams, es);
	else
		pstmt = standard_planner(copyObject(parse), query_string,
								 cursorOptions, boundParams, es);

	/* Roll back any GUC changes */
	AtEOXact_GUC(false, save_nestlevel);

	/* And we're done. */
	return pstmt;
}

/*
 * Extract the feedback list from a PlannedStmt's extension_state.
 * Returns NIL if no feedback is present.
 */
static List *
extract_feedback(PlannedStmt *pstmt)
{
	DefElem    *pgpa_item;
	DefElem    *feedback_item;

	pgpa_item = find_defelem_by_defname(pstmt->extension_state,
										"pg_plan_advice");
	if (pgpa_item == NULL)
		return NIL;
	feedback_item = find_defelem_by_defname((List *) pgpa_item->arg,
											"feedback");
	if (feedback_item == NULL)
		return NIL;
	return (List *) feedback_item->arg;
}

/*
 * Check whether a feedback list indicates that all advice was applied
 * successfully.
 */
static bool
all_feedback_is_ok(List *feedback)
{
	foreach_node(DefElem, item, feedback)
	{
		if (defGetInt32(item) != (PGPA_FB_MATCH_PARTIAL | PGPA_FB_MATCH_FULL))
			return false;
	}
	return true;
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
