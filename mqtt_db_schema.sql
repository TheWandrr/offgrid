CREATE TABLE topic (
    id INTEGER PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    exponent INTEGER NOT NULL
);

CREATE TABLE message (
    payload INTEGER,
    timestamp INTEGER NOT NULL,
    topic_id INTEGER NOT NULL,
    FOREIGN KEY(topic_id) REFERENCES topic(id)
);
