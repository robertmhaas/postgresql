/*-------------------------------------------------------------------------
 *
 * provenance.h
 *	  Provenance declarations.
 *
 * Provenance refers to the origin or history of ownership of an object;
 * in the case of PostgreSQL, our interest is in tracking the causal
 * links that have caused us to access a particular object or consider
 * performing a particular operation.
 *
 * For example, if a user enters the query SELECT * FROM some_rls_table
 * WHERE a = 1 and a row-level security policy introduces an additional
 * qual is_publicly_visible(status), the provenance of the equality operator
 * in a = 1 is the user's session, but the provenance of the call to
 * is_publicly_visible(id) is the row-level security policy.
 *
 * Provenance forms a chain that ultimately links back to the user's
 * session. In the example above, the equality operator in the qual
 * a = 1 is ultimately implemented by a function such as int4eq. So,
 * int4eq() is called because of the presence of the equality operator,
 * and the equality operator is called because of the user's session.
 * If the is_publicly_visible(status) call were inlined to produce a new
 * qual such as status > 0, ultimately calling the int4gt operator,
 * then the full provenance of that operator would be:
 *
 * (1) function int4gt, which was called because of
 * (2) operator >, which was called because of
 * (3) function is_publicly_visible, which was called because of
 * (4) the row-level security policy, which was called because of
 * (5) the user's session
 *
 * At each step of the chain, we track not only the object of origin but
 * the role OID responsible for it. Note that this role OID is *never*
 * the current user OID. That would tell us whose privileges are being
 * used to execute it, but provenance is meant to tell us who was in
 * control of the decision to execute it in the first place. In the
 * above example, the role OID for steps (1) and (2) will be the
 * bootstrap superuser, which owns those objects; the role OID for
 * step (3) is the owner of the function; the role OID for step (4) is
 * the owner of the relation, who controls the policies on it; and
 * the role OID for step 5 is GetSessionUserId().
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/nodes/provenance.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROVENANCE_H
#define PROVENANCE_H

#include "catalog/pg_authid_d.h"
#include "nodes/nodes.h"

/*
 * Each ProvenanceEntry is identified by a ProvenanceKind, which identifies
 * the type of object from which the relevant reference originated.
 *
 * Many types of catalog objects contain exactly one provenance-relevant
 * reference; for example, a cast points to a single cast function and nothing
 * else, so a single enum value suffices.
 *
 * Some types of catalog objects, particularly relations, can refer to other
 * objects in multiple ways. For example, an index can have both index
 * predicates and index expressions, and it costs us nothing to be clear about
 * which is at issue.
 *
 * A few of these provenance kinds do not correspond to catalog objects at all.
 * In particular, PROVENANCE_SESSION is for top-level user inputs, and
 * PROVENANCE_FILESYSTEM is for queries read from disk.
 */
typedef enum ProvenanceKind
{
	PROVENANCE_ATTRDEFAULT,		/* pg_attrdef */
	PROVENANCE_CAST,			/* pg_cast */
	PROVENANCE_COLUMN,			/* pg_class, but per-column property */
	PROVENANCE_CONSTRAINT,		/* pg_constraint */
	PROVENANCE_FILESYSTEM,		/* server filesystem */
	PROVENANCE_FOREIGN_SERVER,	/* pg_foreign_server */
	PROVENANCE_FOREIGN_TABLE,	/* pg_class with RELKIND_FOREIGN_TABLE */
	PROVENANCE_FUNCTION,		/* pg_proc */
	PROVENANCE_INDEX_DEFINITION,	/* for ALTER TABLE, recreating an index */
	PROVENANCE_INDEX_EXPRESSION,	/* pg_index.indxexprs */
	PROVENANCE_INDEX_PREDICATE, /* pg_index.indpred */
	PROVENANCE_MATERIALIZED_VIEW,	/* pg_class with RELKIND_MATVIEW */
	PROVENANCE_OPCLASS,			/* pg_opclass */
	PROVENANCE_OPERATOR,		/* pg_operator */
	PROVENANCE_OPFAMILY,		/* pg_opfamily */
	PROVENANCE_PARTITION_KEY,	/* pg_partitioned_table.partexprs */
	PROVENANCE_POLICY,			/* pg_policy */
	PROVENANCE_PROPERTY_GRAPH,	/* pg_class with RELKIND_PROPGRAPH */
	PROVENANCE_PUBLICATION,		/* pg_publication */
	PROVENANCE_RULE,			/* pg_rewrite */
	PROVENANCE_SESSION,			/* direct user input */
	PROVENANCE_STATISTICS,		/* pg_statistic_ext */
	PROVENANCE_SUBSCRIPTION,	/* pg_subscription */
	PROVENANCE_TRIGGER,			/* pg_trigger */
	PROVENANCE_TYPE,			/* pg_type */
} ProvenanceKind;

/*
 * An index into a Provenances object.
 */
typedef int ProvenanceIndex;

/*
 * Special provenance index that can be used whenever it's believed that
 * the expression tree being constructed will never be executed.
 */
#define PI_NEVER_EXECUTED	(-100)

/*
 * A ProvenanceEntry represents one link in a chain of provenance.
 *
 * prov_kind identifies what type of object this entry refers to.
 *
 * prov_object_id is the OID of the relevant catalog entry (e.g. a pg_proc
 * OID for PROVENANCE_FUNCTION, a pg_class OID for relation-based kinds).
 * For PROVENANCE_SESSION and PROVENANCE_FILESYSTEM, it is InvalidOid.
 *
 * prov_role_id is the OID of the PostgreSQL role that we judge to be in
 * control of that origin.
 *
 * prov_parent_index is the index within a single Provenances object of the
 * next link in the provenance chain. Every Provenances object should have
 * exactly one entry where this link is self-referential, which should be
 * the root of every provenance chain represented by that object.
 *
 * prov_sole_role_id is InvalidOid if there are two or more role OIDs other
 * than BOOTSTRAP_SUPERUSERID in the provenance chain that terminates at
 * this entry. If there is exactly one such role OID, then this is that role
 * OID. If there are no such role OIDs, then this is BOOTSTRAP_SUPERUSERID.
 * Intuitively, whenever this is not InvalidOid, it's the only role OID
 * that matters for purposes of checking trust: everyone must trust the
 * bootstrap superuser by necessity.
 *
 * Exception: Casts have no owner, and control is shared between the roles
 * that own the source and target type. prov_role_id is always recorded as
 * InvalidOid, but prov_sole_role_id is computed as if both the source
 * and target type owners were included in the chain. In the common case
 * where users execute only their own code and built-in objects, this
 * preserves the invariant that prov_sole_role_id gives us the only role
 * OID with which we need to be concerned.
 */
typedef struct ProvenanceEntry
{
	ProvenanceKind prov_kind;
	Oid			prov_object_id;
	Oid			prov_role_id;
	ProvenanceIndex prov_parent_index;
	Oid			prov_sole_role_id;
} ProvenanceEntry;

/*
 * A Provenances object represents a set of related provenances. To identify
 * the provenance of some function call or other entity in particular, you
 * need a Provenances object and an index into the "entries" array.
 *
 * The initial provenance chain created when initializing a Provenances object
 * always begins at index 0. Subsequently-added provenances will point back,
 * directly or indirectly, to this root entry.
 */
typedef struct Provenances
{
	pg_node_attr(custom_copy_equal, custom_read_write, no_query_jumble)
	NodeTag		type;
	int			length;			/* number of entries currently present */
	int			max_length;		/* allocated length of entries[] */
	ProvenanceEntry *entries;	/* re-allocatable array of entries */
} Provenances;

/*
 * Constructor functions.
 */
extern Provenances *InitProvenancesForBootstrap(void);
extern Provenances *InitProvenancesForSession(void);
extern Provenances *InitProvenances(Provenances *parent,
									ProvenanceIndex parent_index);
extern Provenances *InitProvenancesForCache(ProvenanceKind kind,
											Oid object_id,
											Oid role_id);
extern Provenances *InitProvenancesForSubscription(Oid suboid,
												   Oid subowner);
extern ProvenanceIndex GetProvenance(Provenances *provenances,
									 ProvenanceKind kind, Oid object_id,
									 Oid role_id,
									 ProvenanceIndex parent_index);
extern ProvenanceIndex AppendProvenances(Provenances *provenances,
										 Provenances *new_provenances,
										 ProvenanceIndex parent_index);
extern ProvenanceIndex ProvenanceForCast(Provenances *provenances,
										 Oid object_id, Oid source_owner,
										 Oid target_owner,
										 ProvenanceIndex parent_index);

/* Formatting function. */
extern char *DescribeProvenance(ProvenanceKind kind, Oid object_id);

/* Adjust provenance indexes in place. */
extern void OffsetProvenances(Node *node, ProvenanceIndex offset);

/*
 * Constructor macros for provenances objects.
 */
#define InitProvenancesForForeignTableCache(rel) \
	InitProvenancesForCache(PROVENANCE_FOREIGN_TABLE, \
							RelationGetRelid((rel)), \
							(rel)->rd_rel->relowner)
#define InitProvenancesForIndexExpressionCache(rel) \
	InitProvenancesForCache(PROVENANCE_INDEX_EXPRESSION, \
							RelationGetRelid((rel)), \
							(rel)->rd_rel->relowner)
#define InitProvenancesForIndexPredicateCache(rel) \
	InitProvenancesForCache(PROVENANCE_INDEX_PREDICATE, \
							RelationGetRelid((rel)), \
							(rel)->rd_rel->relowner)
#define InitProvenancesForPartitionKeyCache(rel) \
	InitProvenancesForCache(PROVENANCE_PARTITION_KEY, \
							RelationGetRelid((rel)), \
							(rel)->rd_rel->relowner)

/*
 * Constructor macros for provenance entries.
 */
#define ProvenanceForAttrDefault(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_ATTRDEFAULT, \
				  (oid), (owner), (pidx))
#define ProvenanceForColumn(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_COLUMN, \
				  (oid), (owner), (pidx))
#define ProvenanceForConstraint(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_CONSTRAINT, \
				  (oid), (owner), (pidx))
#define ProvenanceForFileSystem(provenances, pidx) \
	GetProvenance((provenances), PROVENANCE_FILESYSTEM, \
				  InvalidOid, BOOTSTRAP_SUPERUSERID, pidx)
#define ProvenanceForForeignServer(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_FOREIGN_SERVER, \
				  (oid), (owner), (pidx))
#define ProvenanceForFunction(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_FUNCTION, \
				  (oid), (owner), (pidx))
#define ProvenanceForFmgrInfo(provenances, fcinfo, pidx) \
	ProvenanceForFunction((provenances), (fcinfo)->flinfo->fn_oid, \
						  (fcinfo)->flinfo->fn_owner, (pidx))
#define ProvenanceForIndexDefinition(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_INDEX_DEFINITION, \
				  (oid), (owner), (pidx))
#define ProvenanceForIndexExpression(provenances, rel, pidx) \
	GetProvenance((provenances), PROVENANCE_INDEX_EXPRESSION, \
				  RelationGetRelid((rel)), (rel)->rd_rel->relowner, (pidx))
#define ProvenanceForIndexPredicate(provenances, rel, pidx) \
	GetProvenance((provenances), PROVENANCE_INDEX_PREDICATE, \
				  RelationGetRelid((rel)), (rel)->rd_rel->relowner, (pidx))
#define ProvenanceForOpclass(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_OPCLASS, \
				  (oid), (owner), (pidx))
#define ProvenanceForOperator(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_OPERATOR, \
				  (oid), (owner), (pidx))
#define ProvenanceForOpfamily(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_OPFAMILY, \
				  (oid), (owner), (pidx))
#define ProvenanceForPartitionKey(provenances, rel, pidx) \
	GetProvenance((provenances), PROVENANCE_PARTITION_KEY, \
				  RelationGetRelid((rel)), (rel)->rd_rel->relowner, (pidx))
#define ProvenanceForPolicy(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_POLICY, \
				  (oid), (owner), (pidx))
#define ProvenanceForPropertyGraph(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_PROPERTY_GRAPH, \
				  (oid), (owner), (pidx))
#define ProvenanceForPublication(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_PUBLICATION, \
				  (oid), (owner), (pidx))
#define ProvenanceForRule(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_RULE, \
				  (oid), (owner), (pidx))
#define ProvenanceForStatistics(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_STATISTICS, \
				  (oid), (owner), (pidx))
#define ProvenanceForTrigger(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_TRIGGER, \
				  (oid), (owner), (pidx))
#define ProvenanceForType(provenances, oid, owner, pidx) \
	GetProvenance((provenances), PROVENANCE_TYPE, \
				  (oid), (owner), (pidx))

#endif
