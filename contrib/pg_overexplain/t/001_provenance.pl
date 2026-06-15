use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', <<EOM);
session_preload_libraries = 'pg_overexplain'
EOM
$node->start;

# Create one superuser and one normal user.
$node->safe_psql('postgres', <<EOM);
CREATE USER p_superuser SUPERUSER;
CREATE USER p_normaluser;
GRANT CREATE ON SCHEMA public TO p_normaluser;
EOM

# Determine the identity of the bootstrap superuser.
my $bootstrap_superuser = $node->safe_psql('postgres', <<EOM);
SELECT rolname FROM pg_authid WHERE oid = 10
EOM

# Run psql as a particular user.
sub run_psql
{
	my ($connuser, $sql) = @_;
	$node->safe_psql('postgres', $sql, extra_params => ['-U', $connuser]);
}

# Strip leading whitespace from each line in a list.
sub trim_lines
{
	my @lines = @_;
	foreach my $line (@lines)
	{
		$line =~ s/^\s+//;
	}
	return @lines;
}

# Run a SQL query and check that we get the expected provenance lines back.
sub check_explain
{
	my ($tag, $connuser, $sql, $expected) = @_;
	chomp $sql;
	chomp $expected;

	my $stdout = run_psql($connuser, $sql);
	my @got = trim_lines(grep { /^\s*Provenance \d+:/ } split(/\n/, $stdout));
	my @expected = map { s/BOOTSTRAP_SUPERUSER/$bootstrap_superuser/gr; }
		trim_lines(grep { /\S/ } split(/\n/, $expected));
	my $n_expected = 0+@expected;

	is(0+@got, $n_expected, "$tag: provenance count");
	for my $i (0 .. $n_expected - 1)
	{
		is($got[$i], $expected[$i], "$tag: provenance $i");
	}
}

# Simple SELECT — session provenance only.
check_explain('simple SELECT', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT 1
EOQ
Provenance 0: session (role: p_superuser)
EOP

# View owned by p_superuser — session + rewrite rule.
run_psql('p_superuser', <<EOM);
CREATE TABLE t (a int);
CREATE VIEW v AS SELECT * FROM t;
EOM
check_explain('same-role view', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM v
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: rule _RETURN on view v (role: p_superuser)
EOP

# View owned by someone else, queried by p_superuser.
run_psql('p_normaluser', <<EOM);
CREATE TABLE t2 (b int);
CREATE VIEW v2 AS SELECT * FROM t2;
GRANT SELECT ON v2 TO p_superuser;
EOM
check_explain('cross-role view', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM v2
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: rule _RETURN on view v2 (role: p_normaluser)
EOP

# Column default — provenance for the pg_attrdef entry.
run_psql('p_superuser', <<EOM);
CREATE TABLE t_coldefault (a int DEFAULT 42, b text);
EOM
check_explain('column default', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) INSERT INTO t_coldefault (b) VALUES ('x')
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: default value for column a of table t_coldefault (role: p_superuser)
EOP

# Domain default — provenance for the pg_type entry.
run_psql('p_superuser', <<EOM);
CREATE DOMAIN d_int AS int DEFAULT 99;
CREATE TABLE t_typedefault (a d_int, b text);
EOM
check_explain('domain default', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) INSERT INTO t_typedefault (b) VALUES ('x')
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: type d_int (role: p_superuser)
EOP

# Identity column — provenance attributed to the table.
run_psql('p_superuser', <<EOM);
CREATE TABLE t_identity (a int GENERATED ALWAYS AS IDENTITY, b text);
EOM
check_explain('identity column', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) INSERT INTO t_identity (b) VALUES ('x')
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: column property of t_identity (role: p_superuser)
EOP

# RLS policy — provenance for the pg_policy entry.
# Must use a non-superuser: superusers have BYPASSRLS which overrides
# FORCE ROW LEVEL SECURITY.
run_psql('p_normaluser', <<EOM);
CREATE TABLE t_rls (a int);
ALTER TABLE t_rls ENABLE ROW LEVEL SECURITY;
ALTER TABLE t_rls FORCE ROW LEVEL SECURITY;
CREATE POLICY p_rls ON t_rls USING (a > 0);
EOM
check_explain('rls policy', 'p_normaluser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM t_rls
EOQ
Provenance 0: session (role: p_normaluser)
Provenance 1: policy p_rls on table t_rls (role: p_normaluser)
EOP

# SQL scalar function inlining — provenance for the pg_proc entry.
run_psql('p_superuser', <<'EOM');
CREATE FUNCTION f_add_one(int) RETURNS int AS 'SELECT $1 + 1' LANGUAGE SQL;
EOM
check_explain('sql function inlining', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT f_add_one(42)
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: function f_add_one(integer) (role: p_superuser)
EOP

# Cross-role SQL function inlining — function owner appears in provenance.
run_psql('p_normaluser', <<'EOM');
CREATE FUNCTION f_normal(int) RETURNS int AS 'SELECT $1 + 2' LANGUAGE SQL;
GRANT EXECUTE ON FUNCTION f_normal(int) TO p_superuser;
EOM
check_explain('cross-role sql function', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT f_normal(42)
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: function f_normal(integer) (role: p_normaluser)
EOP

# Conditional INSTEAD rule — exercises CopyAndAddInvertedQual.
# Both the qual_product (original INSERT with NOT(qual)) and the rule action
# appear in EXPLAIN output, each with their own provenance list.
run_psql('p_superuser', <<EOM);
CREATE TABLE t_rule_src (a int);
CREATE TABLE t_rule_dst (a int);
CREATE RULE r_cond AS ON INSERT TO t_rule_src
    WHERE (NEW.a > 0) DO INSTEAD INSERT INTO t_rule_dst VALUES (NEW.a);
EOM
check_explain('conditional rule', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) INSERT INTO t_rule_src VALUES (1)
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: rule r_cond on table t_rule_src (role: p_superuser)
Provenance 0: session (role: p_superuser)
Provenance 1: rule r_cond on table t_rule_src (role: p_superuser)
EOP

# SQL set-returning function inlining — exercises inline_sql_function_in_from.
run_psql('p_superuser', <<EOM);
CREATE FUNCTION f_srf() RETURNS TABLE (x int) AS
    'SELECT g FROM generate_series(1, 3) g' LANGUAGE SQL STABLE;
EOM
check_explain('sql srf inlining', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM f_srf()
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: function f_srf() (role: p_superuser)
EOP

# PL/pgSQL function running a query via SPI — the inner query's provenance
# should be the function, not the session.
run_psql('p_superuser', <<'EOM');
CREATE FUNCTION f_plpgsql_explain() RETURNS SETOF text AS
$$BEGIN RETURN QUERY EXECUTE 'EXPLAIN (PROVENANCE, COSTS OFF) SELECT 1'; END$$
LANGUAGE plpgsql;
EOM
check_explain('plpgsql spi', 'p_superuser', <<EOQ, <<EOP);
SELECT * FROM f_plpgsql_explain()
EOQ
Provenance 0: function f_plpgsql_explain() (role: p_superuser)
EOP

# Cross-role PL/pgSQL function — provenance shows the function owner.
run_psql('p_normaluser', <<'EOM');
CREATE FUNCTION f_plpgsql_normal() RETURNS SETOF text AS
$$BEGIN RETURN QUERY EXECUTE 'EXPLAIN (PROVENANCE, COSTS OFF) SELECT 1'; END$$
LANGUAGE plpgsql;
GRANT EXECUTE ON FUNCTION f_plpgsql_normal() TO p_superuser;
EOM
check_explain('cross-role plpgsql spi', 'p_superuser', <<EOQ, <<EOP);
SELECT * FROM f_plpgsql_normal()
EOQ
Provenance 0: function f_plpgsql_normal() (role: p_normaluser)
EOP

# DO block — provenance should pass through from calling context (session).
run_psql('p_superuser', <<'EOM');
CREATE TABLE do_explain (id serial, line text);
DO $$
  DECLARE r text;
BEGIN
  FOR r IN
    EXECUTE 'EXPLAIN (PROVENANCE, COSTS OFF) SELECT 1'
  LOOP
    INSERT INTO do_explain (line) VALUES (r);
  END LOOP;
END$$;
EOM
my $do_result = run_psql('p_superuser',
	"SELECT line FROM do_explain WHERE line LIKE '%Provenance%' ORDER BY id");
my @do_lines = trim_lines(split(/\n/, $do_result));
is(0+@do_lines, 1, "DO block: provenance count");
is($do_lines[0], "Provenance 0: session (role: p_superuser)",
   "DO block: session provenance pass-through");

# Property graph — graph owner appears in provenance.
run_psql('p_normaluser', <<EOM);
CREATE TABLE gt_vertices2 (id int PRIMARY KEY, name text);
CREATE TABLE gt_edges2 (id int PRIMARY KEY, src int, dest int);
CREATE PROPERTY GRAPH gt_graph2
    VERTEX TABLES (
        gt_vertices2 KEY (id) LABEL v PROPERTIES (name)
    )
    EDGE TABLES (
        gt_edges2 KEY (id)
            SOURCE KEY (src) REFERENCES gt_vertices2 (id)
            DESTINATION KEY (dest) REFERENCES gt_vertices2 (id)
            LABEL e
    );
EOM
check_explain('cross-role property graph', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM GRAPH_TABLE (gt_graph2 MATCH (a IS v)-[IS e]->(b IS v) COLUMNS (a.name AS n1, b.name AS n2))
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: property graph gt_graph2 (role: p_normaluser)
Provenance 2: operator =(integer,integer) (role: BOOTSTRAP_SUPERUSER)
EOP

# Expression index — indexprs provenance appears because expression indexes
# are always processed by create_index_paths().
run_psql('p_normaluser', <<EOM);
CREATE TABLE t_indexprs (a text);
CREATE INDEX i_indexprs ON t_indexprs (lower(a));
GRANT SELECT ON t_indexprs TO p_superuser;
EOM
check_explain('expression index', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM t_indexprs WHERE lower(a) = 'foo'
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: index expressions on i_indexprs (role: p_normaluser)
EOP

# Partial index predicate — indpred provenance appears when the query
# implies the index predicate (predOK is true).
run_psql('p_normaluser', <<EOM);
CREATE TABLE t_indpred (a int);
CREATE INDEX i_indpred ON t_indpred (a) WHERE a > 0;
GRANT SELECT ON t_indpred TO p_superuser;
EOM
check_explain('partial index predicate', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM t_indpred WHERE a > 0
EOQ
Provenance 0: session (role: p_superuser)
Provenance 1: index predicate of i_indpred (role: p_normaluser)
EOP

# Unused partial index — when the query does not imply the index predicate,
# predOK is false and create_index_paths() skips the index, so its
# provenance should NOT appear.
check_explain('unused partial index', 'p_superuser', <<EOQ, <<EOP);
EXPLAIN (PROVENANCE, COSTS OFF) SELECT * FROM t_indpred WHERE a < 0
EOQ
Provenance 0: session (role: p_superuser)
EOP

done_testing();
