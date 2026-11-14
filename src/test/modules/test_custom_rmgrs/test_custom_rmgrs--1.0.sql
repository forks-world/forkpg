/* src/test/modules/test_custom_rmgrs/test_custom_rmgrs--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_custom_rmgrs" to load this file. \quit

--
-- test_custom_rmgrs_insert_wal_record()
--
-- Writes a simple message into WAL with the help of custom WAL
-- resource manager.
--
CREATE FUNCTION test_custom_rmgrs_insert_wal_record(IN payload TEXT,
    OUT lsn pg_lsn
)
AS 'MODULE_PATHNAME', 'test_custom_rmgrs_insert_wal_record'
LANGUAGE C STRICT PARALLEL UNSAFE;

-- Verify a stale segment-switch request does not switch a later WAL segment.
CREATE FUNCTION test_custom_rmgrs_conditional_switch_stale_segment()
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_custom_rmgrs_conditional_switch_stale_segment'
LANGUAGE C STRICT PARALLEL UNSAFE;
