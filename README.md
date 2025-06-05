# AgroPRO – ESP8266 IoT Datalogger for Agriculture

AgroPRO is an NTP-synchronized ESP8266-based IoT system designed to monitor temperature and humidity using DS18B20 and DHT sensors. It streams real-time readings to Arduino IoT Cloud while logging time-aligned data every 10 minutes for hourly average reporting to Google Sheets.

## ✨ Features

- 📡 Continuous sensor updates to Arduino Cloud (every ~2 seconds)
- 🕒 Precise NTP-aligned 10-minute sampling (e.g., 07:10, 07:20, ...)
- 📊 Hourly average upload to Google Sheets via HTTPS Web App
- 🌡️ Supports up to 4x DS18B20 + 1x DHT11/DHT22
- 📅 Accurate time sync using NTP (GMT+8 default)
- 🔁 Robust error handling for sensor failures
- 🌱 Ideal for compost, greenhouse, barn, or aquaponic environments

## 📦 Hardware Requirements

- NodeMCU / ESP8266 board
- DS18B20 temperature sensors (1–4 units)
- DHT11 or DHT22 humidity sensor
- 4.7kΩ pull-up resistor for DS18B20 data line
- WiFi access (with internet for NTP + Cloud)

## 🛠️ Arduino Libraries

- `ArduinoIoTCloud`
- `Arduino_ConnectionHandler`
- `DHT sensor library`
- `DallasTemperature`
- `OneWire`
- `ESP8266HTTPClient`
- `WiFiClientSecure`

Install all via **Library Manager** in the Arduino IDE.

## 📈 Data Flow

| Function             | Platform         | Frequency     |
|----------------------|------------------|---------------|
| Sensor reading       | Local MCU        | Every 2 sec   |
| Cloud update         | Arduino Cloud    | Every 10 sec  |
| Sampling (for GSheet)| Local buffer     | Every 10 min  |
| Report to GSheet     | Google Web App   | Hourly (hh:00)|

## 🔐 Setup Notes

- Configure your **Arduino Cloud Thing** with variables:  
  `temp1`, `temp2`, `temp3`, `temp4`, `dhtTemp`, `dhtHumi`  
  Set them to `READ` with `ON_UPDATE` mode (10,000 ms interval recommended).

- Deploy your **Google Apps Script Web App** and allow anonymous POST access.

- Set your timezone offset in `GMT_OFFSET_SECONDS` (e.g., GMT+8 → `8 * 3600`)

## 📜 License

MIT License. Feel free to remix and adapt for your farm, lab, or research use.

---

Made with 💚 by Aman & Anna.
