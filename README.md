# Offgrid

It pains me to release this project into the wild in its current state. In doing so, I hope that others can learn by example, contribute improvements, and create documentation.

This project has been developed using a Raspberry Pi 3B+, "Raspberry Pi Meet Arduino" hat, and a few external components. Reading through the code should indicate these components, but I can try to answer questions on what has been used where it isn't clear. I will not be able to answer troubleshooting questions regarding how to get hardware components connected and operational.

Code for the Arduino board may be found here: https://github.com/TheWandrr/offgrid_arduino

# Purpose

The Raspberry Pi is connected via serial link to the Arduino. The functions of the Raspberry Pi are as follows:
  - Receive and decode periodically broadcast metrics/telemetry from serially connected Arduino
  - Rebroadcast metrics (topics) via MQTT
  - Store select (tagged) metrics into a SQLITE database.
  - Manage some local sensors as though they were attached to the Arduino directly

# Future Direction

Most of the code in this project has been running in my own campervan conversion for at least a year or two. Small changes have been made along the way when I've added new hardware (eg. solar) or have become annoyed with the way something was working (or not). It has arrived at a point where I now see better ways to design the system and thus will probably only use portions of the code herein but with a different system topology.

TheWandrr
2022-05-15
