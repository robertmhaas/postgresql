/*-------------------------------------------------------------------------
 *
 * parse_utilcmd.h
 *		parse analysis for utility commands
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_utilcmd.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_UTILCMD_H
#define PARSE_UTILCMD_H

#include "parser/parse_node.h"

typedef struct AttrMap AttrMap; /* avoid including attmap.h here */


extern List *transformCreateStmt(CreateStmt *stmt, const char *queryString);
extern AlterTableStmt *transformAlterTableStmt(Oid relid, AlterTableStmt *stmt,
											   const char *queryString,
											   List **beforeStmts,
											   List **afterStmts,
											   Provenances *provenances);
extern IndexStmt *transformIndexStmt(Oid relid, IndexStmt *stmt,
									 const char *queryString,
									 Provenances *provenances);
extern CreateStatsStmt *transformStatsStmt(ParseState *pstate, Oid relid,
										   CreateStatsStmt *stmt);
extern void transformRuleStmt(ParseState *pstate, RuleStmt *stmt,
							  List **actions, Node **whereClause);
extern List *transformCreateSchemaStmtElements(ParseState *pstate,
											   List *schemaElts,
											   const char *schemaName);
extern PartitionBoundSpec *transformPartitionBound(ParseState *pstate, Relation parent,
												   PartitionBoundSpec *spec);
extern List *expandTableLikeClause(RangeVar *heapRel,
								   TableLikeClause *table_like_clause,
								   Provenances *provenances);
extern IndexStmt *generateClonedIndexStmt(RangeVar *heapRel,
										  Relation source_idx,
										  const AttrMap *attmap,
										  Oid *constraintOid);

#endif							/* PARSE_UTILCMD_H */
