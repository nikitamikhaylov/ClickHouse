SET input_format_parallel_parsing=0;

DROP TABLE IF EXISTS alter;
CREATE TABLE alter (`boolean_false` Nullable(String)) ENGINE = MergeTree ORDER BY tuple();

INSERT INTO alter_00665 (`boolean_false`) VALUES (NULL), (''), ('123');
SELECT * FROM alter_00665;
SELECT * FROM alter_00665 ORDER BY boolean_false NULLS LAST;

ALTER TABLE alter_00665 MODIFY COLUMN `boolean_false` Nullable(UInt8);
SELECT * FROM alter_00665;
SELECT * FROM alter_00665 ORDER BY boolean_false NULLS LAST;

DROP TABLE alter_00665;
