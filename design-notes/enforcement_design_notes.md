# Provenance Enforcement Design Notes

## The Problem

The provenance system tracks where expressions come from (session,
RLS policy, rewrite rule, inlined function, etc.). But provenance
alone is not sufficient for security enforcement, because an
untrusted party can influence what code runs through indirect
mechanisms — particularly operator resolution.

## Operator Trust Gap

An operator is a catalog entry that maps a name + input types to
a function. Anyone can create an operator. Cross-type matching in
operator resolution means a query can pick up an operator the
author didn't specifically intend. The operator owner controls
which function gets called, but their involvement is invisible
to the provenance system.

Attack scenario: Claude (untrusted) creates an operator that
points to a function owned by the current user. The function is
trusted, the provenance is the session — both checks pass. But
Claude controlled which function gets called and with what
argument routing, potentially causing trusted code to be invoked
in unintended ways.

This is not unique to operators: function inlining can produce
the same effect (untrusted function inlines to a call to a
trusted function with attacker-chosen arguments). But operators
are especially concerning because anyone can create them and
cross-type matching makes accidental use plausible.

Opclasses and opfamilies are less concerning: they can only be
created by superusers, and there's an argument that the concept
of ownership should be removed from those object types altogether.

## Proposed Solution: oprowner on OpExpr

Add an `oprowner` Oid field to OpExpr, populated at operator
resolution time (wherever opfuncid is set). This gives the
enforcement layer direct access to who created the operator
without any catalog lookup at enforcement time.

Related node types (DistinctExpr, NullIfExpr, ScalarArrayOpExpr)
would need the same treatment.

## Function Purity Labeling

A function purity concept (distinct from volatility) would help
make enforcement less oppressive. A "pure" function only computes
on its inputs — no side effects, no reading of database state,
session state, or external data. Calling a pure function with
unexpected arguments cannot leak data, modify state, or escalate
privileges; the worst outcome is a wrong answer.

Purity allows the enforcement layer to set a lower trust bar for
pure functions: if the function can't do damage regardless of
inputs, it matters less who introduced the call or who chose the
function via operator resolution.

## Enforcement Point: ExecInitExpr

ExecInitExpr already checks ACL_EXECUTE on funcid. The proposed
enforcement adds two new checks:

1. **Provenance trust**: Does GetUserId() trust ptRoleId (from
   the provenance) enough to call this function?
2. **Operator owner trust** (OpExpr only): Does GetUserId()
   trust oprowner enough to call this function?

Both checks can be modulated by function purity: pure functions
need a lower trust bar.

All three inputs — ptRoleId, oprowner, and the function's
purity/identity — are available on the node at ExecInitExpr
time. No catalog lookups needed during enforcement.

## Other Enforcement Points (Not Yet Designed)

- **ProcessUtility()**: Untrusted provenance attempting DDL is
  likely a red flag in most cases.
- **Table access**: If a query mentions table T and the
  provenance source doesn't have access to T, the current user
  probably doesn't want to provide access on their behalf.
