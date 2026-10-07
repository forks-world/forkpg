/* contrib/pagestore/pagestore--1.3--1.4.sql */

CREATE FUNCTION pagestore_prepare_branch_from_control(
    target_dir text,
    new_timeline integer,
    parent_timeline integer,
    base_lsn pg_lsn,
    checkpoint_redo pg_lsn,
    fork_lsn pg_lsn,
    incarnation bigint,
    require_materialized boolean)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pagestore_prepare_branch_from_control'
LANGUAGE C STRICT PARALLEL UNSAFE;

COMMENT ON FUNCTION pagestore_prepare_branch_from_control(text, integer, integer, pg_lsn, pg_lsn, pg_lsn, bigint, boolean) IS
'prepare a control-derived branch; the serialized controller requires a durable materializer marker covering the fork';
