SET input_format_parallel_parsing=0;

DROP TABLE IF EXISTS test.nullt;

CREATE TABLE nullt (c1 Nullable(UInt32), c2 Nullable(String))ENGINE = Log;
INSERT INTO nullt VALUES (1, 'abc'), (2, NULL), (NULL, NULL);

SELECT c2 = ('abc') FROM nullt;
SELECT c2 IN ('abc') FROM nullt;

SELECT c2 IN ('abc', NULL) FROM nullt;

DROP TABLE IF EXISTS nullt;
