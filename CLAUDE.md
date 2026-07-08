On this branch, we're trying to implement a provenance system. The basics
are explained in src/include/nodes/provenance.h, which you should read
immediately to make sure you understand the overall goals. See also
src/backend/nodes/provenance.c. Known gaps in the implementation are
marked with PROVENANCE-TODO comments.

A lot of work has been done to try to make sure that provenances reach
eval_const_expressions() and the functions that call it, which now seems
to be largely complete. From there, we need to thread a Provenances * and
a ProvenanceIndex through to ExecInitExprRec so that these values can be
passed through to ExecInitFunc and then to fmgr_info. For this to be
possible, nodes like FuncExpr and OpExpr need to carry a ProvenanceIndex.
As of this writing, the field has been added, but it's not yet properly
populated everywhere, which is something we need to try to figure out how
to fix, but unfortunately there are a bunch of complicated cases.

Eventually, I'd like to get to a point where fmgr_info() stores a
Provenances pointer and the relative ProvenanceIndex in every FmgrInfo,
so that the whole chain of events that led to the function being called
is traced out right there. This would allow elimination of a bunch of
nasty hacks where we write InitProvenancesForCache(PROVENANCE_FUNCTION,
fcinfo->flinfo->fn_oid, fcinfo->flinfo->fn_owner), only capturing
provenance back to the point where that function was called, instead
of all the way back to the user's session.

Any catalog lookups that influence decisions about which functions to call
or with what arguments should become part of the provenance chain. Such
catalog lookups are frequently lookups of operators, operator classes, or
operator families. When it's a lookup of an operator, we might be traversing
a link via oprcom or oprneg to another operator, in which case both should
become part of the provenance chain. We're trying to trace the causality
links all the way out to whatever the user entered originally.

When a FuncExpr, OpExpr, etc. is created during parse analysis, its
provenance is the provenance of the query, which should be at the root
index (i.e. zero) of the available Provenances object. But any later
transformations should result in non-zero indexes. For instance, if the
user wrote 5 < a and we turn that into a > 5, the call to the greater-than
operator had better not have a provenance index of zero, because the user
never said anything about the > operator, only the < operator. The provenance
of the function we ultimately call might be something like function int4gt
=> operator >(int4,int4) => operator <(int4,int4) => provenance of the query
itself. This assumes we just looked up the negator of <(int4, int4). If we
did a more complicated set of lookups involving opfamilies or opclasses, those
should be incorporated into the chain as well. But note that if something
is completely hard-coded, it does not extend the provenances chain. For
example, the FuncExpr for F_SATISFIES_HASH_PARTITION in partbounds.c traces
its provenance directly back to the partition key, because there's no
subsequent catalog lookup to blame for the fact that we're calling that
function; the selection of hash partitioning as a strategy is the whole
story.

Previously, it seemed as though operator_predicate_proof() didn't need
Provenances passed down, but it builds and evaluates an OpExpr, so it does.

process_implied_equality() build equality clauses that don't exist in the
original query. I am not sure how to assign provenance in a case like this.
The process by which the EC was build is complex, and likely involves a
bunch of catalog lookups that aren't chained together in a strictly
cause-and-effect fashion.

plpgsql's get_cast_hashentry() needs more study. It seems to be maintaining
some sort of cache, but it looks like that cache is for a particular
PLpgSQL_execstate, so maybe we ought to be chaining off of the execstate's
provenances.

The long-term goal is to make use of the Provenance data to guard against
attempts by lower-privileged or differently-privileged accounts to
abuse the privileges of other users via indirect calls that the
victim may not have anticipated. Therefore, it's critical that no entries
be omited from the provenances chain. If the user wrote a query which
referenced a view owned by Alice which contained an operator owned by
Bob which was commuted to an operator owned by Charlie, all of those
steps can affect whether the user is OK the code executing on their
behalf.  The details of how this should work in detail are not sorted out
yet, but in general, if the superuser executes a trigger owned by some
other and that trigger wants to call int4pl, the superuser is probably fine
with that. If that same trigger wants to do ALTER ROLE alice SUPERUSER,
the superuser would probably prefer for that to be blocked. Provenance will
help by telling us whether a given query, function call, utility
statement, or whatever came from the session user (in which case it's
not our job to rescue them from themselves) or from somebody else (in
which case we may want to block the operation, depending on the
circumstances).  This is why it's critical that provenance information
is always properly fed through every part of the system: we can only
stop Alice from doing bad things if we know that the code that we're
executing came from Alice. If we erroneously believe it came from the
superuser themselves, we won't understand that there's a possible issue.
A major failure mode here is passing a provenance index of 0 when we should
be doing something else.

Coding Style Notes
==================

The developing pattern for top-level command entry points (ExecVacuum,
ExecRepack, ExecReindex, ExecuteTruncate, DefineRelation, etc.) is:

    /* Separate parse-time provenances from execution-time provenances. */
    provenances = InitProvenances(pstate->p_provenances, 0);

This creates a child Provenances that inherits the parse-time chain
but provides a separate object for execution-time entries. The result
is then threaded downward through the call stack. Callers further
down that need provenances receive the pointer as a parameter; they
generally should not copy that object again (either via InitProvenances
or copyObject) except where it makes sense to avoid polluting the
caller's provenances.

Use helper macros as effectively as possible e.g. don't
directly call InitProvenancesForCache when you could instead use
InitProvenancesForIndexExpressionCache, InitProvenancesForPartitionKeyCache,
etc.; don't directly call GetProvenance when you could use
ProvenanceForFunction, ProvenanceForConstraint, etc.

Some of the locutions pertaining to provenance construction are quite
lengthy. Overly long lines can easily result. Introduce intermediate
variables to avoid that where necessary. But also avoid introducing
line breaks or temporary variables without a good reason when we'd
fit in 78 characters or less without those things.
