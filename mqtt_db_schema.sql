CREATE TABLE interface (
    id INTEGER PRIMARY KEY NOT NULL,
    name TEXT NOT NULL UNIQUE,
    unit TEXT NOT NULL,
    exponent INTEGER NOT NULL
);

CREATE TABLE message (
    payload INTEGER,
    timestamp INTEGER NOT NULL,
    interface_id INTEGER NOT NULL,
    FOREIGN KEY(interface_id) REFERENCES interface(id)
);
