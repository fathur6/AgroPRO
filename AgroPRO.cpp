// Aman & Anna – NTP-Aligned 10-min Datalogging & Hourly Reporting for ESP8266
// Data is sampled at NTP-aligned 10-minute marks (hh:00, hh:10, ...),
// and an hourly report of averages is sent at hh:00 (approximately).

#include "thingProperties.h"      // For Arduino Cloud variables and connection
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <ESP8266HTTPClient.h>    // For making HTTP/HTTPS requests
#include <WiFiClientSecure.h>     // For HTTPS
#include <time.h>                 // For time functions

// --- Configuration Constants ---
// Network & Web Service
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwgPGSPbvbY2sWSYUBstWece1FNbq5NLLHkBIBBhaRspdGKvDbgaiw0vC6cfDgHKdIMlQ/exec";

// Hardware Pins
const int ONE_WIRE_BUS_PIN = 12;  // DS18B20 data pin
const int DHT_SENSOR_PIN   = 14;  // DHT sensor data pin
const int DHT_SENSOR_TYPE  = DHT11; // Change to DHT22 or DHT21 if using those

// NTP Time
const long        GMT_OFFSET_SECONDS      = 8 * 3600; // GMT+8
const int         DAYLIGHT_OFFSET_SECONDS = 0;
const char* NTP_SERVER_PRIMARY      = "pool.ntp.org";
const char* NTP_SERVER_SECONDARY    = "time.nist.gov"; // Fallback NTP
unsigned long     lastNtpSyncMillis       = 0;
const unsigned long NTP_RESYNC_INTERVAL_MS  = 12UL * 3600UL * 1000UL; // Resync every 12 hours
const int         NTP_SYNC_MAX_TRIES      = 20;
const int         NTP_SYNC_RETRY_DELAY_MS = 500;
const long        MIN_EPOCH_TIME_SEC      = 946684800L; // Min valid time (Jan 1, 2000, 00:00:00 UTC)

// Data Sampling & Reporting
const int SAMPLES_PER_HOUR         = 6;    // e.g., one sample every 10 minutes
const int SAMPLING_INTERVAL_MIN    = 10;   // Sample every 10 minutes
const int REPORTING_TRIGGER_SECOND = 5;    // Second of minute 00 to trigger report (e.g., hh:00:05)

// DS18B20 Sensor Addresses (ensure these are correct for your sensors)
const int NUM_DS18B20_SENSORS = 4;
DeviceAddress ds18b20_addresses[NUM_DS18B20_SENSORS] = {
  {0x28,0x88,0x95,0x57,0x04,0xE1,0x3D,0x02}, // Sensor 1
  {0x28,0x8A,0x64,0x57,0x04,0xE1,0x3D,0x07}, // Sensor 2
  {0x28,0xD5,0xDA,0x57,0x04,0xE1,0x3D,0xE0}, // Sensor 3
  {0x28,0x8D,0x17,0x57,0x04,0xE1,0x3D,0xA1}  // Sensor 4
};

// --- Global Objects ---
OneWire oneWire(ONE_WIRE_BUS_PIN);
DallasTemperature ds18b20_sensors(&oneWire);
DHT dht(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);

// --- Data Storage for Averaging ---
float ds18b20_temp_samples[NUM_DS18B20_SENSORS][SAMPLES_PER_HOUR];
float dht_humidity_samples[SAMPLES_PER_HOUR];
float dht_temp_samples[SAMPLES_PER_HOUR];
int   current_sample_index = 0; // Index for the circular buffer of samples
int   samples_taken_this_hour = 0; // Count of valid samples in the current hour

// --- State Variables for Timing ---
int last_sample_minute_taken = -1; // To prevent double sampling in the same minute
int last_report_hour_sent  = -1; // To prevent double reporting in the same hour

// =======================================================================================
//                                   SETUP FUNCTION
// =======================================================================================
void setup() {
  Serial.begin(9600); // Or 115200 for faster serial
  delay(1500); // Wait for serial monitor to connect
  Serial.println("\nESP8266 Datalogger Initializing...");

  // Initialize Arduino Cloud (this also handles WiFi connection)
  initProperties(); // Links variables to Arduino Cloud
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2); // 0 (errors), 1 (info), 2 (debug)
  ArduinoCloud.printDebugInfo();
  Serial.println("Waiting for Arduino Cloud connection...");

  // Initialize Sensors
  ds18b20_sensors.begin();
  dht.begin();
  Serial.println("Sensors initialized.");

  // Configure and Synchronize NTP Time
  // Ensure WiFi is connected before configuring time (ArduinoCloud handles this)
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
    synchronizeNtpTime();
  } else {
    Serial.println("Error: WiFi not connected, cannot synchronize NTP time at setup.");
    // Potentially loop here or set a flag to retry NTP sync later
  }
  
  clearSampleArrays(); // Prepare arrays for first hour of sampling
  Serial.println("Setup complete. Starting main loop.");
}

// =======================================================================================
//                                    MAIN LOOP
// =======================================================================================
void loop() {
  ArduinoCloud.update(); // Essential for Arduino Cloud functionality

  unsigned long current_millis = millis();

  // --- Periodic NTP Time Re-synchronization ---
  if (current_millis - lastNtpSyncMillis > NTP_RESYNC_INTERVAL_MS || lastNtpSyncMillis == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      synchronizeNtpTime();
    }
    lastNtpSyncMillis = current_millis; // Update even if sync failed to avoid rapid retries
  }

  time_t now_epoch = time(nullptr);
  if (now_epoch < MIN_EPOCH_TIME_SEC) { // Check if time is valid before proceeding
    Serial.println("Time not yet synchronized or invalid. Skipping sampling/reporting cycle.");
    delay(1000); // Wait a bit before retrying
    return;
  }

  struct tm time_info;
  localtime_r(&now_epoch, &time_info);

  // --- NTP-aligned Datalogging (e.g., every 10 minutes at hh:00:00, hh:10:00, ...) ---
  if (time_info.tm_min % SAMPLING_INTERVAL_MIN == 0 &&
      time_info.tm_sec == 0 && // Sample exactly at the start of the second
      last_sample_minute_taken != time_info.tm_min) {
    
    Serial.printf("Taking sample at %02d:%02d:%02d\n", time_info.tm_hour, time_info.tm_min, time_info.tm_sec);
    takeSample(current_sample_index);
    
    current_sample_index = (current_sample_index + 1) % SAMPLES_PER_HOUR; // Advance circular buffer index
    samples_taken_this_hour = min(samples_taken_this_hour + 1, SAMPLES_PER_HOUR); // Increment sample count for the hour
    last_sample_minute_taken = time_info.tm_min; // Mark this minute as sampled
  }

  // Reset 'last_sample_minute_taken' if we've moved past the sampling minute.
  // This allows the next SAMPLING_INTERVAL_MIN mark to trigger a new sample.
  if (time_info.tm_min % SAMPLING_INTERVAL_MIN != 0) {
    last_sample_minute_taken = -1;
  }

  // --- NTP-aligned Hourly Reporting (e.g., at hh:00:05) ---
  if (time_info.tm_min == 0 &&                         // Top of the hour
      time_info.tm_sec == REPORTING_TRIGGER_SECOND && // Specific second to trigger report
      last_report_hour_sent != time_info.tm_hour &&   // Ensure only one report per hour
      samples_taken_this_hour > 0) {                  // Only report if samples were taken
        
    Serial.printf("Initiating hourly report for hour: %d\n", time_info.tm_hour);
    reportDataToGoogleSheet();
    
    // Reset for the next hour
    clearSampleArrays();
    current_sample_index = 0;
    samples_taken_this_hour = 0;
    last_report_hour_sent = time_info.tm_hour;
  }
  delay(200); // Small delay to yield to other processes, adjust as needed
}

// =======================================================================================
//                                 HELPER FUNCTIONS
// =======================================================================================

/**
 * @brief Synchronizes the ESP8266's internal clock with an NTP server.
 */
void synchronizeNtpTime() {
  Serial.print("Synchronizing time with NTP server... ");
  time_t now = time(nullptr);
  int retries = NTP_SYNC_MAX_TRIES;
  
  // Wait until time is valid (e.g., after year 2000)
  while (now < MIN_EPOCH_TIME_SEC && retries-- > 0) {
    delay(NTP_SYNC_RETRY_DELAY_MS);
    now = time(nullptr);
    Serial.print(".");
  }
  Serial.println();

  if (now < MIN_EPOCH_TIME_SEC) {
    Serial.println("NTP Time synchronization failed!");
  } else {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo); // Use localtime_r for thread safety
    Serial.print("NTP Time synchronized: ");
    Serial.print(asctime(&timeinfo)); // asctime() adds a newline
  }
}

/**
 * @brief Takes sensor readings and stores them in the sample arrays at the given index.
 * @param sample_idx The index in the sample arrays to store the new readings.
 */
void takeSample(int sample_idx) {
  ds18b20_sensors.requestTemperatures(); // Request all DS18B20 sensors to convert temperature

  // Read DS18B20 sensors
  for (int i = 0; i < NUM_DS18B20_SENSORS; i++) {
    float temp_c = ds18b20_sensors.getTempC(ds18b20_addresses[i]);
    if (temp_c == DEVICE_DISCONNECTED_C || temp_c == 85.0 || temp_c == (-127.0) ) { // 85C can be a power-on reset value, -127 is error
      ds18b20_temp_samples[i][sample_idx] = NAN; // Store NAN for invalid readings
      Serial.printf("Error reading DS18B20 Sensor %d.\n", i + 1);
    } else {
      ds18b20_temp_samples[i][sample_idx] = temp_c;
    }
  }

  // Read DHT sensor
  float dht_temp = dht.readTemperature(); // Celsius
  float dht_hum  = dht.readHumidity();    // Percent

  // Validate DHT readings (DHT library often returns NAN on failure)
  dht_temp_samples[sample_idx]   = (!isnan(dht_temp)) ? dht_temp : NAN;
  dht_humidity_samples[sample_idx] = (!isnan(dht_hum))  ? dht_hum  : NAN;

  Serial.printf("Sample [%d]:", sample_idx);
  for (int i = 0; i < NUM_DS18B20_SENSORS; i++) {
    Serial.printf(" DS%d:%.2fC", i + 1, ds18b20_temp_samples[i][sample_idx]);
  }
  Serial.printf(" DHT-T:%.2fC DHT-H:%.1f%%\n", dht_temp_samples[sample_idx], dht_humidity_samples[sample_idx]);

  // Update Arduino Cloud "live" variables with the latest sample
  // Ensure these variable names (sensor1, dhtTemp etc.) match those in your thingProperties.h
  if (NUM_DS18B20_SENSORS > 0) sensor1 = ds18b20_temp_samples[0][sample_idx];
  if (NUM_DS18B20_SENSORS > 1) sensor2 = ds18b20_temp_samples[1][sample_idx];
  if (NUM_DS18B20_SENSORS > 2) sensor3 = ds18b20_temp_samples[2][sample_idx];
  if (NUM_DS18B20_SENSORS > 3) sensor4 = ds18b20_temp_samples[3][sample_idx];
  dhtTemp = dht_temp_samples[sample_idx];
  dhtHumi = dht_humidity_samples[sample_idx];
}

/**
 * @brief Calculates averages and sends the hourly report to Google Sheets.
 */
void reportDataToGoogleSheet() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot send report to Google Sheet.");
    return;
  }

  Serial.println("[Reporting hourly averages to Google Sheets]");
  
  // Calculate averages
  float avg_ds18b20_temps[NUM_DS18B20_SENSORS];
  for (int i = 0; i < NUM_DS18B20_SENSORS; i++) {
    avg_ds18b20_temps[i] = calculateAverage(ds18b20_temp_samples[i], samples_taken_this_hour);
  }
  float avg_dht_temp = calculateAverage(dht_temp_samples, samples_taken_this_hour);
  float avg_dht_hum  = calculateAverage(dht_humidity_samples, samples_taken_this_hour);

  // Prepare JSON payload
  // Adjust buffer size if more data fields are added.
  // Using snprintf for safer string formatting.
  char json_payload[256]; 
  int written_chars = snprintf(json_payload, sizeof(json_payload),
             "{\"sensor1\":%.2f,\"sensor2\":%.2f,\"sensor3\":%.2f,\"sensor4\":%.2f,\"dhttemp\":%.2f,\"dhthumidity\":%.2f}",
             (NUM_DS18B20_SENSORS > 0) ? avg_ds18b20_temps[0] : NAN,
             (NUM_DS18B20_SENSORS > 1) ? avg_ds18b20_temps[1] : NAN,
             (NUM_DS18B20_SENSORS > 2) ? avg_ds18b20_temps[2] : NAN,
             (NUM_DS18B20_SENSORS > 3) ? avg_ds18b20_temps[3] : NAN,
             avg_dht_temp,
             avg_dht_hum);

  if (written_chars < 0 || written_chars >= sizeof(json_payload)) {
    Serial.println("Error: JSON payload encoding failed or buffer too small.");
    return;
  }

  Serial.print("Sending JSON: "); Serial.println(json_payload);

  WiFiClientSecure https_client;
  // For ESP8266, `setFingerprint()` or `setTrustAnchors()` is more secure if you have the server's fingerprint/CA.
  // `setInsecure()` skips server certificate validation (less secure, MITM risk).
  https_client.setInsecure(); 
  // https_client.setBufferSizes(1024, 512); // Optional: if needed for larger responses

  HTTPClient http_client;
  http_client.setTimeout(10000); // Increased timeout for potentially slow Google Scripts

  if (http_client.begin(https_client, GOOGLE_SCRIPT_URL)) {
    http_client.addHeader("Content-Type", "application/json; charset=utf-8");
    
    int http_response_code = http_client.POST(json_payload); // Send the payload as String or uint8_t*
    
    if (http_response_code > 0) {
      Serial.printf("HTTP POST successful, Response Code: %d\n", http_response_code);
      String response_body = http_client.getString();
      Serial.println("Response body: " + response_body);
    } else {
      Serial.printf("HTTP POST failed, Error: %s (Code: %d)\n", http_client.errorToString(http_response_code).c_str(), http_response_code);
    }
    http_client.end();
  } else {
    Serial.println("Error: Unable to connect to Google Script URL.");
  }
}

/**
 * @brief Clears all sample arrays by filling them with NAN.
 */
void clearSampleArrays() {
  for (int i = 0; i < SAMPLES_PER_HOUR; i++) {
    for (int j = 0; j < NUM_DS18B20_SENSORS; j++) {
      ds18b20_temp_samples[j][i] = NAN;
    }
    dht_temp_samples[i]   = NAN;
    dht_humidity_samples[i] = NAN;
  }
  Serial.println("Sample arrays cleared.");
}

/**
 * @brief Calculates the average of valid (non-NAN) float values in an array.
 * @param arr Pointer to the float array.
 * @param num_samples The number of samples to consider for averaging.
 * @return The average value, or NAN if no valid samples.
 */
float calculateAverage(const float arr[], int num_samples) {
  if (num_samples == 0) return NAN;
  
  float sum = 0.0f;
  int valid_sample_count = 0;
  for (int i = 0; i < num_samples; i++) {
    if (!isnan(arr[i])) { // Only consider valid, non-NAN numbers
      sum += arr[i];
      valid_sample_count++;
    }
  }
  
  return (valid_sample_count > 0) ? (sum / valid_sample_count) : NAN;
}

// Ensure that cloud variables (sensor1, sensor2, etc.) are declared in "thingProperties.h"
// Example (in thingProperties.h):
// CloudTemperatureSensor sensor1;
// CloudTemperatureSensor sensor2;
// CloudTemperatureSensor sensor3;
// CloudTemperatureSensor sensor4;
// CloudTemperatureSensor dhtTemp;
// CloudRelativeHumidity dhtHumi;