/* contrib/pagestore/pagestore--1.5--1.6.sql */

CREATE FUNCTION pagestore_branch_snapshot_is_safe(timeline integer, incarnation bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pagestore_branch_snapshot_is_safe'
LANGUAGE C STRICT PARALLEL RESTRICTED;

COMMENT ON FUNCTION pagestore_branch_snapshot_is_safe(integer, bigint) IS
'check that this branch incarnation has a durable finite snapshot cap on a capable daemon; legacy branches are not recertified';
