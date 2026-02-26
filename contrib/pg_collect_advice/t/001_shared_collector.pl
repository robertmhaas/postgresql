# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test the shared advice collector.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Helper function, to avoid depending on exact line-break behavior.
sub smash_whitespace
{
	my $s = shift;
	$s =~ s/^\s+//;
	$s =~ s/\s+$//;
	$s =~ s/\s+/ /g;
	return $s;
}

# Retrieve all collected shared advice as an array of whitespace-normalized
# strings, ordered by id.
sub get_collected_shared_advice
{
	my $psql = shift;
	my $output = $psql->query_safe(
		"SELECT string_agg(advice, '!SEPARATOR!' ORDER BY id) "
		. "FROM pg_get_collected_shared_advice()");
	return () if $output eq '';
	return map { smash_whitespace($_) } split(/!SEPARATOR!/, $output);
}

# Initialize the primary node
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init();

# Load pg_collect_advice and configure a shared collection limit of 5.
$node->append_conf('postgresql.conf', <<EOM);
shared_preload_libraries=pg_collect_advice
pg_collect_advice.shared_collection_limit=5
EOM
$node->start;

# Create the extension so we can access the collector
my $test_db = 'collection_test';
my $test_role = 'collection_role';
$node->safe_psql('postgres', <<EOM);
CREATE DATABASE $test_db;
CREATE USER $test_role;
ALTER ROLE $test_role SET pg_collect_advice.shared_collector = on;
EOM
$node->safe_psql($test_db, 'CREATE EXTENSION pg_collect_advice');

# Set up two connections, one to control the testing process, and the other
# to execute the queries under test.
my $psql_control = $node->background_psql($test_db, on_error_stop => 1);
my $psql_test =
	$node->background_psql($test_db, on_error_stop => 1,
						   extra_params => [ '--username' => $test_role ]);

# Initial setup.
$psql_control->query_safe(<<EOM);
GRANT CREATE ON SCHEMA public TO $test_role;
GRANT SET ON PARAMETER pg_collect_advice.shared_collection_limit TO $test_role;
SET ROLE $test_role;
CREATE TABLE sac_dim (id serial primary key, dim text)
	WITH (autovacuum_enabled = false);
INSERT INTO sac_dim (dim) SELECT random()::text FROM generate_series(1,100) g;
VACUUM ANALYZE sac_dim;

CREATE TABLE sac_fact (
	id int primary key,
	dim_id integer not null references sac_dim (id)
) WITH (autovacuum_enabled = false);
INSERT INTO sac_fact
SELECT g, (g%3)+1 FROM generate_series(1,100000) g;
CREATE INDEX sac_fact_dim_id ON sac_fact (dim_id);
VACUUM ANALYZE sac_fact;
RESET ROLE;
EOM

# Run a few test queries.
$psql_test->query_safe(<<'EOM');
SELECT * FROM sac_fact WHERE id = 42;
SELECT * FROM sac_dim d JOIN sac_fact f ON d.id = f.dim_id;
SELECT * FROM sac_dim d
    WHERE d.id IN (SELECT f.dim_id FROM sac_fact f);
EOM

# Check that we got three advice collections, and the right values for each.
my @advice = get_collected_shared_advice($psql_control);
is(scalar @advice, 3, "three advice entries collected");
is($advice[0], 'INDEX_SCAN(sac_fact public.sac_fact_pkey) NO_GATHER(sac_fact)',
	"correct advice for query 1");
is($advice[1], 'JOIN_ORDER(f d) HASH_JOIN(d) SEQ_SCAN(f d) NO_GATHER(d f)',
	"correct advice for query 2");
is($advice[2], 'JOIN_ORDER(d f) NESTED_LOOP_PLAIN(f) SEQ_SCAN(d) INDEX_ONLY_SCAN(f public.sac_fact_dim_id) SEMIJOIN_NON_UNIQUE(f) NO_GATHER(d f)',
	"correct advice for query 3");

# Run a few more test queries, overrunning the limit. (SET and PREPARE don't
# trigger planning, but EXECUTE does.)
$psql_test->query_safe(<<'EOM');
BEGIN;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;
SELECT count(*) FROM sac_fact;
COMMIT;
EXPLAIN SELECT * FROM sac_dim;
PREPARE test_stmt AS SELECT * FROM sac_fact WHERE id = $1;
EXECUTE test_stmt(42);
EOM

# Check that advice collection was trimmed to the configured limit.
@advice = get_collected_shared_advice($psql_control);
is(scalar @advice, 5, "advice trimmed to collection limit");

# Check the advice for queries 4, 5, and 6.
is($advice[2], 'SEQ_SCAN(sac_fact) GATHER(sac_fact)',
	"correct advice for query 4");
is($advice[3], 'SEQ_SCAN(sac_dim) NO_GATHER(sac_dim)',
	"correct advice for query 5");
is($advice[4],
	'INDEX_SCAN(sac_fact public.sac_fact_pkey) NO_GATHER(sac_fact)',
	"correct advice for query 6");

# Raise the collection limit so that we can collect enough advice to need
# multiple chunks, and then revert back to the old value, so that we try
# to free an entire chunk.
$psql_test->query_safe("SET pg_collect_advice.shared_collection_limit = 1500");
$psql_test->query_safe(<<'EOM');
DO $$
BEGIN
	FOR i IN 1..1500 LOOP
		EXECUTE 'SELECT 1';
	END LOOP;
END $$;
EOM
@advice = get_collected_shared_advice($psql_control);
is(scalar @advice, 1500, "increased collection limit reached");
$psql_test->query_safe("RESET pg_collect_advice.shared_collection_limit");
$psql_test->query_safe("SELECT * FROM sac_dim");
@advice = get_collected_shared_advice($psql_control);
is(scalar @advice, 5, "advice trimmed across chunk boundary");

# Try clearing all the advice.
$psql_control->query_safe("SELECT pg_clear_collected_shared_advice()");
@advice = get_collected_shared_advice($psql_control);
is(scalar @advice, 0, "all shared advice cleared");

# Clean up.
$psql_test->quit;
$psql_control->quit;
done_testing();
