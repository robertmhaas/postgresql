/*-------------------------------------------------------------------------
 *
 * provenance.c
 *	  Support code (mostly constructors) for provenances.
 *
 * See comments in src/include/nodes/provenance.h for overall goals.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/backend/nodes/provenance.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_attrdef_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint_d.h"
#include "catalog/pg_foreign_server_d.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_policy_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_rewrite_d.h"
#include "catalog/pg_statistic_ext_d.h"
#include "catalog/pg_subscription_d.h"
#include "catalog/pg_trigger_d.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/provenance.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#define		INITIAL_NUM_PROVENANCE_ENTRIES		8

static ProvenanceIndex GetProvenanceInternal(Provenances *provenances,
											 ProvenanceKind kind,
											 Oid object_id, Oid role_id,
											 ProvenanceIndex parent_index);
static char *DescribeRelationProvenance(ProvenanceKind kind, Oid object_id);
static char *DescribeObjectProvenance(ProvenanceKind kind, Oid object_id);
static bool OffsetProvenancesWalker(Node *node, ProvenanceIndex *offset);


/*
 * Initialize a Provenances object for use in bootstrap mode.
 */
Provenances *
InitProvenancesForBootstrap(void)
{
	Provenances *provenances = makeNode(Provenances);
	ProvenanceEntry *pentry;

	Assert(IsBootstrapProcessingMode());

	provenances->length = 1;
	provenances->max_length = INITIAL_NUM_PROVENANCE_ENTRIES;
	provenances->entries = palloc_array(ProvenanceEntry,
										provenances->max_length);

	pentry = &provenances->entries[0];
	pentry->prov_kind = PROVENANCE_SESSION;
	pentry->prov_object_id = InvalidOid;
	pentry->prov_role_id = BOOTSTRAP_SUPERUSERID;
	pentry->prov_parent_index = 0;
	pentry->prov_sole_role_id = pentry->prov_role_id;

	return provenances;
}

/*
 * Initialize a Provenances object with a root entry for the current session.
 *
 * This should ONLY be used for direct user inputs. If a Query (or something
 * else that has Provenance) originates anywhere other than from a direct
 * user input, it should use InitProvenances() to extend an existing Provenance
 * chain rather than creating an entirely new one.
 */
Provenances *
InitProvenancesForSession(void)
{
	Provenances *provenances = makeNode(Provenances);
	ProvenanceEntry *pentry;

	provenances->length = 1;
	provenances->max_length = INITIAL_NUM_PROVENANCE_ENTRIES;
	provenances->entries = palloc_array(ProvenanceEntry,
										provenances->max_length);

	pentry = &provenances->entries[0];
	pentry->prov_kind = PROVENANCE_SESSION;
	pentry->prov_object_id = InvalidOid;
	pentry->prov_role_id = GetSessionUserId();
	pentry->prov_parent_index = 0;
	pentry->prov_sole_role_id = pentry->prov_role_id;
	Assert(OidIsValid(pentry->prov_sole_role_id));

	return provenances;
}

/*
 * Initialize a Provenances object as a child of an existing one.
 *
 * The provenance chain from 'parent' beginning at 'parent_index' is copied
 * to the new object in such a way that it becomes the root provenance (i.e.
 * the chain from the existing object begins at index 0 in the new object).
 * Afterwards, new provenances can be added, extending that history.
 */
Provenances *
InitProvenances(Provenances *parent, ProvenanceIndex parent_index)
{
	Provenances *provenances = makeNode(Provenances);
	ProvenanceIndex index = 0;

	provenances->length = 0;
	provenances->max_length = INITIAL_NUM_PROVENANCE_ENTRIES;
	provenances->entries = palloc_array(ProvenanceEntry,
										provenances->max_length);

	while (1)
	{
		ProvenanceEntry *pentry = &provenances->entries[index];
		ProvenanceEntry *parent_pentry = &parent->entries[parent_index];

		pentry->prov_kind = parent_pentry->prov_kind;
		pentry->prov_object_id = parent_pentry->prov_object_id;
		pentry->prov_role_id = parent_pentry->prov_role_id;
		pentry->prov_sole_role_id = parent_pentry->prov_sole_role_id;

		if (parent_pentry->prov_parent_index == parent_index)
		{
			pentry->prov_parent_index = index;
			break;
		}

		pentry->prov_parent_index = ++index;

		if (index >= provenances->max_length)
		{
			int			new_max_length = provenances->max_length * 2;

			provenances->entries = repalloc_array(provenances->entries,
												  ProvenanceEntry,
												  new_max_length);
			provenances->max_length = new_max_length;
		}

		parent_index = parent_pentry->prov_parent_index;
	}

	provenances->length = index + 1;
	return provenances;
}

/*
 * Initialize a Provenances object for a cache entry.
 *
 * Most of the time, a chain of Provenance should trace back all the way
 * to a user's session, but caches are a special case. For instance, we
 * don't want a relcache entry to have a provenance chain tracing back
 * to whatever query caused it to be created originally, because the cache
 * entry is independent of that original query. Instead, the catalog object
 * that gives rise to the cache becomes the root element of the corresponding
 * Provenances object.
 */
Provenances *
InitProvenancesForCache(ProvenanceKind kind, Oid object_id, Oid role_id)
{
	Provenances *provenances = makeNode(Provenances);
	ProvenanceEntry *pentry;

	provenances->length = 1;
	provenances->max_length = INITIAL_NUM_PROVENANCE_ENTRIES;
	provenances->entries = palloc_array(ProvenanceEntry,
										provenances->max_length);

	pentry = &provenances->entries[0];
	pentry->prov_kind = kind;
	pentry->prov_object_id = object_id;
	pentry->prov_role_id = role_id;
	pentry->prov_parent_index = 0;
	pentry->prov_sole_role_id = pentry->prov_role_id;
	Assert(OidIsValid(pentry->prov_sole_role_id));

	return provenances;
}

/*
 * Initialize a Provenances object for a logical replication subscription.
 *
 * Like InitProvenancesForSession(), but for a logical replication worker
 * (on the subscriber side).
 */
Provenances *
InitProvenancesForSubscription(Oid suboid, Oid subowner)
{
	Provenances *provenances = makeNode(Provenances);
	ProvenanceEntry *pentry;

	provenances->length = 1;
	provenances->max_length = INITIAL_NUM_PROVENANCE_ENTRIES;
	provenances->entries = palloc_array(ProvenanceEntry,
										provenances->max_length);

	pentry = &provenances->entries[0];
	pentry->prov_kind = PROVENANCE_SUBSCRIPTION;
	pentry->prov_object_id = suboid;
	pentry->prov_role_id = subowner;
	pentry->prov_parent_index = 0;
	pentry->prov_sole_role_id = pentry->prov_role_id;
	Assert(OidIsValid(pentry->prov_sole_role_id));

	return provenances;
}

/*
 * Create a new provenance history by extending an existing one with a new
 * entry.
 *
 * This function automatically deduplicates: if an identical entry with the
 * same parent already exists, it will return the entry of the existing index.
 * Otherwise, it will add a new entry pointing to the specified parent and
 * return the new index.
 */
ProvenanceIndex
GetProvenance(Provenances *provenances, ProvenanceKind kind, Oid object_id,
			  Oid role_id, ProvenanceIndex parent_index)
{
	ProvenanceEntry *pentry;
	int			length = provenances->length;
	ProvenanceIndex index;
	Oid			parent_sole_role_id;

	Assert(kind != PROVENANCE_SESSION);

	index = GetProvenanceInternal(provenances, kind, object_id, role_id,
								  parent_index);
	if (provenances->length == length)
		return index;

	pentry = &provenances->entries[index];
	parent_sole_role_id = provenances->entries[parent_index].prov_sole_role_id;

	if (role_id == BOOTSTRAP_SUPERUSERID)
		pentry->prov_sole_role_id = parent_sole_role_id;
	else if (parent_sole_role_id == BOOTSTRAP_SUPERUSERID ||
			 parent_sole_role_id == role_id)
		pentry->prov_sole_role_id = role_id;
	else
		pentry->prov_sole_role_id = InvalidOid;

	return index;
}

/*
 * Internal workhorse for GetProvenance.
 *
 * This function handles deduplication and the initialization of a new entry,
 * if we create one, except that it does not initialize the new entry's
 * prov_sole_role_id. That is left to the caller.
 */
static ProvenanceIndex
GetProvenanceInternal(Provenances *provenances, ProvenanceKind kind,
					  Oid object_id, Oid role_id,
					  ProvenanceIndex parent_index)
{
	ProvenanceEntry *pentry;
	ProvenanceIndex i;

	Assert(parent_index >= 0);
	Assert(parent_index < provenances->length);

	for (i = 0; i < provenances->length; ++i)
	{
		pentry = &provenances->entries[i];

		if (pentry->prov_kind == kind &&
			pentry->prov_object_id == object_id &&
			pentry->prov_role_id == role_id &&
			pentry->prov_parent_index == parent_index)
			return i;
	}

	if (provenances->length >= provenances->max_length)
	{
		int			new_max_length = provenances->max_length * 2;

		provenances->entries = repalloc_array(provenances->entries,
											  ProvenanceEntry,
											  new_max_length);
		provenances->max_length = new_max_length;
	}

	i = provenances->length++;
	pentry = &provenances->entries[i];
	pentry->prov_kind = kind;
	pentry->prov_object_id = object_id;
	pentry->prov_role_id = role_id;
	pentry->prov_parent_index = parent_index;

	return i;
}

/*
 * As GetProvenance, but handling the special case for casts.
 *
 * For a cast, we can't set the prov_role_id of the new entry to anything
 * but InvalidOid, because a cast has no recorded owner.
 *
 * However, we can still set prov_sole_role_id, because in effect the
 * ownership of the cast is shared between the owners of the source and
 * target types.
 */
ProvenanceIndex
ProvenanceForCast(Provenances *provenances, Oid object_id, Oid source_owner,
				  Oid target_owner, ProvenanceIndex parent_index)
{
	int			length = provenances->length;
	ProvenanceIndex index;
	Oid			sole_role_id;

	index = GetProvenanceInternal(provenances, PROVENANCE_CAST, object_id,
								  InvalidOid, parent_index);

	if (provenances->length == length)
		return index;

	sole_role_id = provenances->entries[parent_index].prov_sole_role_id;

	if (source_owner != BOOTSTRAP_SUPERUSERID)
	{
		if (sole_role_id == BOOTSTRAP_SUPERUSERID ||
			sole_role_id == source_owner)
			sole_role_id = source_owner;
		else
			sole_role_id = InvalidOid;
	}

	if (target_owner != BOOTSTRAP_SUPERUSERID)
	{
		if (sole_role_id == BOOTSTRAP_SUPERUSERID ||
			sole_role_id == target_owner)
			sole_role_id = target_owner;
		else
			sole_role_id = InvalidOid;
	}

	provenances->entries[index].prov_sole_role_id = sole_role_id;

	return index;
}

/*
 * Graft one provenance chain onto the end of another.
 *
 * new_provenances is the chain to be grafted, and all of its entries are
 * added into provenances, with the provenances chain rooted at parent_index
 * as the parent of all chains represented in new_provenance.
 *
 * This typically happens when new_provenances was initialized using
 * InitProvenancesForCache(), and the expression tree stored in the cache
 * is being used for something that has a provenance history of its own.
 * In general, the cached expression tree is independent of the calling
 * context, but when used in a certain context, the provenance chain is
 * the sequence of entries that led to its use plus the sequence of entries
 * from the cache's own provenance.
 */
ProvenanceIndex
AppendProvenances(Provenances *provenances, Provenances *new_provenances,
				  ProvenanceIndex parent_index)
{
	int			total_length = provenances->length + new_provenances->length;
	ProvenanceIndex offset = provenances->length;
	Oid			parent_sole_role_id;

	if (total_length > provenances->max_length)
	{
		int			new_max_length = pg_nextpower2_32(total_length);

		provenances->entries = repalloc_array(provenances->entries,
											  ProvenanceEntry,
											  new_max_length);
		provenances->max_length = new_max_length;
	}

	parent_sole_role_id = provenances->entries[parent_index].prov_sole_role_id;

	for (int i = 0; i < new_provenances->length; ++i)
	{
		ProvenanceEntry *new_pentry = &new_provenances->entries[i];
		ProvenanceEntry *pentry = &provenances->entries[i + offset];

		pentry->prov_kind = new_pentry->prov_kind;
		pentry->prov_object_id = new_pentry->prov_object_id;
		pentry->prov_role_id = new_pentry->prov_role_id;

		if (i == 0)
			pentry->prov_parent_index = parent_index;
		else
			pentry->prov_parent_index = new_pentry->prov_parent_index + offset;

		if (parent_sole_role_id == new_pentry->prov_sole_role_id)
			pentry->prov_sole_role_id = new_pentry->prov_sole_role_id;
		else if (parent_sole_role_id == BOOTSTRAP_SUPERUSERID)
			pentry->prov_sole_role_id = new_pentry->prov_sole_role_id;
		else if (new_pentry->prov_sole_role_id == BOOTSTRAP_SUPERUSERID)
			pentry->prov_sole_role_id = parent_sole_role_id;
		else
			pentry->prov_sole_role_id = InvalidOid;
	}

	provenances->length += new_provenances->length;
	return offset;
}

/*
 * Describe a single provenance.
 *
 * If the object no longer exists, we describe it by OID rather than failing;
 * this is because we expect to use this while constructing error messages,
 * and a failure here would would result in the user seeign a much less
 * useful diagnostic.
 */
char *
DescribeProvenance(ProvenanceKind kind, Oid object_id)
{
	switch (kind)
	{
		case PROVENANCE_SESSION:
			return pstrdup(_("session"));
		case PROVENANCE_FILESYSTEM:
			return pstrdup(_("filesystem"));

		case PROVENANCE_COLUMN:
		case PROVENANCE_FOREIGN_TABLE:
		case PROVENANCE_INDEX_DEFINITION:
		case PROVENANCE_INDEX_EXPRESSION:
		case PROVENANCE_INDEX_PREDICATE:
		case PROVENANCE_MATERIALIZED_VIEW:
		case PROVENANCE_PARTITION_KEY:
		case PROVENANCE_PROPERTY_GRAPH:
			return DescribeRelationProvenance(kind, object_id);

		case PROVENANCE_ATTRDEFAULT:
		case PROVENANCE_CAST:
		case PROVENANCE_CONSTRAINT:
		case PROVENANCE_FOREIGN_SERVER:
		case PROVENANCE_FUNCTION:
		case PROVENANCE_OPCLASS:
		case PROVENANCE_OPERATOR:
		case PROVENANCE_OPFAMILY:
		case PROVENANCE_POLICY:
		case PROVENANCE_PUBLICATION:
		case PROVENANCE_RULE:
		case PROVENANCE_STATISTICS:
		case PROVENANCE_SUBSCRIPTION:
		case PROVENANCE_TRIGGER:
		case PROVENANCE_TYPE:
			return DescribeObjectProvenance(kind, object_id);
	}

	elog(ERROR, "unexpected provenance kind: %d", (int) kind);
}

/*
 * Describe a relation provenance.
 *
 * We don't use getObjectDescription in these cases, because we want to
 * substitute our own description strings.
 */
static char *
DescribeRelationProvenance(ProvenanceKind kind, Oid object_id)
{
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *nspname;
	char	   *relname;

	relTup = SearchSysCache1(RELOID, ObjectIdGetDatum(object_id));
	if (!HeapTupleIsValid(relTup))
	{
		switch (kind)
		{
			case PROVENANCE_COLUMN:
				return psprintf(_("column property of relation with OID %u"),
								object_id);
			case PROVENANCE_FOREIGN_TABLE:
				return psprintf(_("foreign table with OID %u"),
								object_id);
			case PROVENANCE_INDEX_DEFINITION:
				return psprintf(_("index with OID %u"),
								object_id);
			case PROVENANCE_INDEX_EXPRESSION:
				return psprintf(_("index expressions on relation with OID %u"),
								object_id);
			case PROVENANCE_INDEX_PREDICATE:
				return psprintf(_("index predicate of relation with OID %u"),
								object_id);
			case PROVENANCE_MATERIALIZED_VIEW:
				return psprintf(_("materialized view with OID %u"),
								object_id);
			case PROVENANCE_PARTITION_KEY:
				return psprintf(_("partition key of relation with OID %u"),
								object_id);
			case PROVENANCE_PROPERTY_GRAPH:
				return psprintf(_("property graph with OID %u"),
								object_id);
			default:
				elog(ERROR, "unexpected provenance kind: %d",
					 (int) kind);
		}
	}

	relForm = (Form_pg_class) GETSTRUCT(relTup);

	/* Qualify the name if not visible in search path. */
	if (RelationIsVisible(object_id))
		nspname = NULL;
	else
		nspname = get_namespace_name(relForm->relnamespace);

	relname = quote_qualified_identifier(nspname,
										 NameStr(relForm->relname));

	ReleaseSysCache(relTup);

	switch (kind)
	{
		case PROVENANCE_COLUMN:
			return psprintf(_("column property of %s"), relname);
		case PROVENANCE_FOREIGN_TABLE:
			return psprintf(_("foreign table %s"), relname);
		case PROVENANCE_INDEX_DEFINITION:
			return psprintf(_("index %s"), relname);
		case PROVENANCE_INDEX_EXPRESSION:
			return psprintf(_("index expressions on %s"), relname);
		case PROVENANCE_INDEX_PREDICATE:
			return psprintf(_("index predicate of %s"), relname);
		case PROVENANCE_MATERIALIZED_VIEW:
			return psprintf(_("materialized view %s"), relname);
		case PROVENANCE_PARTITION_KEY:
			return psprintf(_("partition key of %s"), relname);
		case PROVENANCE_PROPERTY_GRAPH:
			return psprintf(_("property graph %s"), relname);
		default:
			elog(ERROR, "unexpected provenance kind: %d",
				 (int) kind);
	}
}

/*
 * Describe a non-relation catalog-backed provenance entry.
 *
 * getObjectDescription() can do most of the work for us, but we need to
 * handle the case where the object no longer exists.
 */
static char *
DescribeObjectProvenance(ProvenanceKind kind, Oid object_id)
{
	ObjectAddress addr;
	Oid			class_id;
	char	   *result;

	switch (kind)
	{
		case PROVENANCE_ATTRDEFAULT:
			class_id = AttrDefaultRelationId;
			break;
		case PROVENANCE_CAST:
			class_id = CastRelationId;
			break;
		case PROVENANCE_CONSTRAINT:
			class_id = ConstraintRelationId;
			break;
		case PROVENANCE_FOREIGN_SERVER:
			class_id = ForeignServerRelationId;
			break;
		case PROVENANCE_FUNCTION:
			class_id = ProcedureRelationId;
			break;
		case PROVENANCE_OPCLASS:
			class_id = OperatorClassRelationId;
			break;
		case PROVENANCE_OPERATOR:
			class_id = OperatorRelationId;
			break;
		case PROVENANCE_OPFAMILY:
			class_id = OperatorFamilyRelationId;
			break;
		case PROVENANCE_POLICY:
			class_id = PolicyRelationId;
			break;
		case PROVENANCE_RULE:
			class_id = RewriteRelationId;
			break;
		case PROVENANCE_STATISTICS:
			class_id = StatisticExtRelationId;
			break;
		case PROVENANCE_SUBSCRIPTION:
			class_id = SubscriptionRelationId;
			break;
		case PROVENANCE_TRIGGER:
			class_id = TriggerRelationId;
			break;
		case PROVENANCE_TYPE:
			class_id = TypeRelationId;
			break;
		default:
			elog(ERROR, "unexpected provenance kind: %d",
				 (int) kind);
	}

	ObjectAddressSet(addr, class_id, object_id);
	result = getObjectDescription(&addr, true);
	if (result != NULL)
		return result;

	switch (kind)
	{
		case PROVENANCE_ATTRDEFAULT:
			return psprintf(_("attribute default with OID %u"),
							object_id);
		case PROVENANCE_CAST:
			return psprintf(_("cast with OID %u"), object_id);
		case PROVENANCE_CONSTRAINT:
			return psprintf(_("constraint with OID %u"),
							object_id);
		case PROVENANCE_FOREIGN_SERVER:
			return psprintf(_("foreign server with OID %u"),
							object_id);
		case PROVENANCE_FUNCTION:
			return psprintf(_("function with OID %u"),
							object_id);
		case PROVENANCE_OPCLASS:
			return psprintf(_("operator class with OID %u"),
							object_id);
		case PROVENANCE_OPERATOR:
			return psprintf(_("operator with OID %u"),
							object_id);
		case PROVENANCE_OPFAMILY:
			return psprintf(_("operator family with OID %u"),
							object_id);
		case PROVENANCE_POLICY:
			return psprintf(_("policy with OID %u"), object_id);
		case PROVENANCE_RULE:
			return psprintf(_("rule with OID %u"), object_id);
		case PROVENANCE_STATISTICS:
			return psprintf(_("statistics object with OID %u"),
							object_id);
		case PROVENANCE_SUBSCRIPTION:
			return psprintf(_("subscription with OID %u"),
							object_id);
		case PROVENANCE_PUBLICATION:
			return psprintf(_("publication with OID %u"),
							object_id);
		case PROVENANCE_TRIGGER:
			return psprintf(_("trigger with OID %u"),
							object_id);
		case PROVENANCE_TYPE:
			return psprintf(_("type with OID %u"), object_id);
		default:
			elog(ERROR, "unexpected provenance kind: %d", (int) kind);
	}
}

/*
 * Offset all ProvenanceIndex fields in a node tree in place.
 */
void
OffsetProvenances(Node *node, ProvenanceIndex offset)
{
	Assert(offset >= 0);

	if (offset == 0)
		return;
	(void) query_or_expression_tree_walker(node,
										   OffsetProvenancesWalker,
										   &offset,
										   0);
}

/*
 * Walker for OffsetProvenances.
 */
static bool
OffsetProvenancesWalker(Node *node, ProvenanceIndex *offset)
{
	if (node == NULL)
		return false;

	if (IsA(node, Query))
		return query_tree_walker((Query *) node,
								 OffsetProvenancesWalker,
								 offset,
								 0);

	if (IsA(node, Aggref))
		((Aggref *) node)->pidx += *offset;
	else if (IsA(node, WindowFunc))
		((WindowFunc *) node)->pidx += *offset;
	else if (IsA(node, FuncExpr))
		((FuncExpr *) node)->pidx += *offset;
	else if (IsA(node, OpExpr) || IsA(node, DistinctExpr) || IsA(node, NullIfExpr))
		((OpExpr *) node)->pidx += *offset;
	else if (IsA(node, ScalarArrayOpExpr))
		((ScalarArrayOpExpr *) node)->pidx += *offset;
	else if (IsA(node, CoerceViaIO))
		((CoerceViaIO *) node)->pidx += *offset;
	else if (IsA(node, RowCompareExpr))
	{
		RowCompareExpr *rc = (RowCompareExpr *) node;
		int			nops = list_length(rc->opnos);

		for (int i = 0; i < nops; i++)
			rc->pidxarr[i] += *offset;
	}
	else if (IsA(node, MinMaxExpr))
		((MinMaxExpr *) node)->pidx += *offset;
	else if (IsA(node, JsonExpr))
		((JsonExpr *) node)->pidx += *offset;
	else if (IsA(node, CommonTableExpr))
		((CommonTableExpr *) node)->pidx += *offset;

	return expression_tree_walker(node,
								  OffsetProvenancesWalker,
								  offset);
}
