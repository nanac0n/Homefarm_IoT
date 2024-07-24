#!/usr/bin/env python3

import Adafruit_DHT
import sys

# DHT 센서 유형과 GPIO 핀 번호 설정
DHT_SENSOR = Adafruit_DHT.DHT11
DHT_PIN = 27

def read_dht():
    humidity, temperature = Adafruit_DHT.read_retry(DHT_SENSOR, DHT_PIN)
    if humidity is not None and temperature is not None:
        print(f"{temperature:.1f},{humidity:.1f}")
    else:
        print("Failed to retrieve data from humidity sensor")

if __name__ == "__main__":
    read_dht()
