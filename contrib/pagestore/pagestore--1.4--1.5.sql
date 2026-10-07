/* contrib/pagestore/pagestore--1.4--1.5.sql */

CREATE FUNCTION pagestore_branch_window_open(
    OUT checkpoint_redo_lsn pg_lsn,
    OUT next_xid xid8,
    OUT prepared_count integer)
RETURNS record
AS 'MODULE_PATHNAME', 'pagestore_branch_window_open'
LANGUAGE C PARALLEL UNSAFE;

CREATE FUNCTION pagestore_prepare_branch_from_window(
    target_dir text, new_timeline integer, parent_timeline integer,
    base_lsn pg_lsn, checkpoint_redo pg_lsn, fork_lsn pg_lsn,
    incarnation bigint, expected_next_xid xid8, expected_prepared_count integer)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pagestore_prepare_branch_from_window'
LANGUAGE C STRICT PARALLEL UNSAFE;

COMMENT ON FUNCTION pagestore_branch_window_open() IS
'open the restricted writer transaction window against its shutdown checkpoint; records the full next XID and requires zero prepared transactions';
COMMENT ON FUNCTION pagestore_prepare_branch_from_window(text, integer, integer, pg_lsn, pg_lsn, pg_lsn, bigint, xid8, integer) IS
'prepare a materialized branch after checking the journaled transaction window; the post-create WAL scan remains authoritative';
