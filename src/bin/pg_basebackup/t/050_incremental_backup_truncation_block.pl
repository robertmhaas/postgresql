# Test case for a scenario where truncation_block is miscalculated during an incremental backup

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
# Enable WAL summarization to support incremental backup
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
$primary->start;

# Backup locations
my $backup_path = $primary->backup_dir;
my $full_backup = "$backup_path/full";

# NB: CI tests run on one platform with the segment size set to six blocks,
#  so we can't rely on any defaults (e.g., segment size = 131072 blocks).
# Calculate how many rows we need to insert into the table to make it span more than one physical storage segment,
# sized so that at most one tuple fits on a page.
my $block_size = $primary->safe_psql('postgres', "SELECT current_setting('block_size')::int;");
my $segment_size_blocks = $primary->safe_psql('postgres',
    "SELECT (pg_size_bytes(current_setting('segment_size')) / $block_size)::int;"
);
# Two blocks will be located in the second segment, and one block will still stay there after truncation.
my $target_rows = int($segment_size_blocks + 2);
my $rows_after_truncation = int($target_rows - 1);

# Create a test table.
# STORAGE PLAIN prevents compression and TOASTing of repetitive data, ensuring predictable row sizes.
$primary->safe_psql('postgres', q{
    CREATE TABLE t (
        id int,
        data text STORAGE PLAIN
    );
});

# The tuple size should be enough to prevent two tuples from being on the same page.
# Since the template string has a length of 32 bytes, it's enough to repeat it (block_size / (2*32)) times.
$primary->safe_psql('postgres',
    "INSERT INTO t
        SELECT i, repeat('0123456789ABCDEF0123456789ABCDEF', ($block_size / (2*32)))
    FROM generate_series(1, $target_rows) i;"
);

# This step is required because at this moment, tuples do not have hint bits set.
# Later (for example, soon after the base backup is created), a background process may set hint bits
# on many tuples and change many heap pages.
# Because of this, the WAL summary may show that too many pages were changed and create a full file copy
# instead of an incremental one, which makes the issue non-reproducible.
$primary->safe_psql('postgres', 'VACUUM t;');

# Verify that relation spans more than one physical storage segment
my $t_blocks = $primary->safe_psql('postgres', "SELECT pg_relation_size('t') / current_setting('block_size')::int;");
cmp_ok($t_blocks, '>', $segment_size_blocks, 'Relation spans more than one physical segment.');

# Take a full base backup
$primary->backup('full');

# Delete rows at the logical end of the table. This creates removable empty pages at the tail
$primary->safe_psql('postgres', "DELETE FROM t WHERE id > ($rows_after_truncation);");

# Although TRUNCATE is enabled by default, here it emphasizes the expected behavior of the operation.
$primary->safe_psql('postgres', 'VACUUM (TRUNCATE) t;');

# Verify that after VACUUM relation is truncated but still spans more than one physical storage segment.
$t_blocks = $primary->safe_psql('postgres',
    "SELECT pg_relation_size('t') / current_setting('block_size')::int;"
);
is($t_blocks, $rows_after_truncation, 'Relation has expected size.');
cmp_ok($t_blocks, '>', $segment_size_blocks, 'Relation spans more than one physical segment.');

# Take an incremental backup based on the full backup manifest
$primary->backup('incr', backup_options => [ '--incremental', "$full_backup/backup_manifest" ]);

# Combine full and incremental backups.
# This step must correctly handle truncated relation segments.
# Before the fix, this failed because the INCREMENTAL file header contained an incorrect truncation_block value.
my $restored = PostgreSQL::Test::Cluster->new('node2');
$restored->init_from_backup($primary, 'incr', combine_with_prior => [ 'full' ]);
$restored->start();

# Check that the restored table contains the correct number of rows
my $restored_count = $restored->safe_psql('postgres', "SELECT count(*) FROM t;");
is($restored_count, $rows_after_truncation, 'Restored backup has correct row count');

$primary->stop;
$restored->stop;

done_testing();
