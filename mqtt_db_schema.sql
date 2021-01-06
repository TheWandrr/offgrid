CREATE TABLE topic (
    id INTEGER PRIMARY KEY NOT NULL,
    name TEXT NOT NULL
);

CREATE TABLE message (
    payload INTEGER,
    timestamp INTEGER NOT NULL,
    topic_id INTEGER NOT NULL,
    FOREIGN KEY(topic_id) REFERENCES topic(id)
);

-- TODO: Change this to runtime initialization by querying the device's interface
-- TODO: Add other encode/decode parameters (fixed point placement, transmission bytes, etc)
INSERT INTO topic (name) VALUES
    ( 'og/setting/broadcast_period_ms' ),
    ( 'og/batmon/bank0/volts' ),
    ( 'og/batmon/bank0/amps' ),
    ( 'og/batmon/bank0/ah' ),
    ( 'og/batmon/bank0/soc' ),
    ( 'og/batmon/bank0/amps_multiplier' ),
    ( 'og/batmon/bank0/amphours_capacity' ),
    ( 'og/batmon/bank0/volts_charged' ),
    ( 'og/batmon/bank0/minutes_charged_detection_time' ),
    ( 'og/batmon/bank0/current_threshold' ),
    ( 'og/batmon/bank0/tail_current_factor' ),
    ( 'og/batmon/bank0/peukert_factor' ),
    ( 'og/batmon/bank0/charge_efficiency_factor' ),
    ( 'og/house/light/ceiling' ),
    ( 'og/house/light/ceiling_encoder' ),
    ( 'og/house/battery/amps' ),
    ( 'og/house/fuse_panel/amps' ),
    ( 'og/house/vehicle_in/amps' ),
    ( 'og/house/inverter/amps' ) 
;
