/*

MIT License

Copyright (c) 2019 Albert Herd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#ifndef __DS18B20_H
#define __DS18B20_H

#include <stdio.h>

#define ONEWIREDEVICELOCATION "/sys/bus/w1/devices/"
#define ONEWIRESLAVEDEVICE "/w1_slave"
#define DS18B20FAMILYCODE "28"
#define DEFAULTSENSORNAME "Sensor"

typedef struct Sensor {
  char *SensorName;
  FILE *SensorFile;
} Sensor;

typedef struct SensorList {
  Sensor **Sensors;
  int SensorCount;
} SensorList;

  SensorList *GetSensors(char **sensorNames, int sensorNamesCount);
  Sensor *GetSensor(char *sensorId, char *sensorName);

  float ReadTemperature(const Sensor *sensor);

  void FreeSensors(SensorList *sensorList);
  void FreeSensor(Sensor *sensor);

#endif /* __DS18B20_H */
