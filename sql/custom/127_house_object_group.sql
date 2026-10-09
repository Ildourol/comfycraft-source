-- ============================================================================
--  Housing: furniture groups  (tw_char)
-- ============================================================================
--  A group is a set of pieces in one house that move, turn and resize as one.
--  This column is the whole of the persistent state.
--
--    0  not grouped.
--    n  group n of this house. Numbered per house, like `slot`: a new group
--       takes the highest number in the house plus one.
--
--  A group is live only while two or more loaded pieces of one house share the
--  number. A piece left alone after the rest were picked up keeps its number
--  and acts ungrouped, so nothing has to clean it up.
--
--  APPLY THIS BEFORE SWAPPING THE BINARY. The loader selects the column, and a
--  failed query is a hard crash here.
--
--  Re-runnable (ADD COLUMN IF NOT EXISTS). Apply with:
--    Get-Content sql\custom\127_house_object_group.sql | DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
-- ============================================================================

ALTER TABLE house_object
    ADD COLUMN IF NOT EXISTS group_no INT UNSIGNED NOT NULL DEFAULT 0
    COMMENT 'furniture group within the house; 0 = not grouped' AFTER slot;
