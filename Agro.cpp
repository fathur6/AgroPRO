// Aman & Anna – ESP8266 NTP‑Aligned Logger v3
// ────────────────────────────────────────────────────────────────
// • Continuous updates to Arduino IoT Cloud every ~2 s  (temp1‑4, dhtTemp, dhtHumi)
// • Precise 10‑min sampling for Google‑Sheet hourly average
// ────────────────────────────────────────────────────────────────

#include "thingProperties.h"          // defines: temp1‑4, dhtTemp, dhtHumi
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ───── Google Sheet Webhook ─────
const char* GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/AKfycbwgPGSPbvbY2sWSYUBstWece1FNbq5NLLHkBIBBhaRspdGKvDbgaiw0vC6cfDgHKdIMlQ/exec";

// ───── Pins ─────
constexpr uint8_t ONE_WIRE_PIN = 12; // DS18B20 bus
constexpr uint8_t DHT_PIN      = 14;
constexpr uint8_t DHT_TYPE     = DHT11; // DHT22 if you use it

// DS18B20 ROM codes (little‑endian)
DeviceAddress DS_ADDR[] = {
  {0x28,0x88,0x95,0x57,0x04,0xE1,0x3D,0x02},
  {0x28,0x8A,0x64,0x57,0x04,0xE1,0x3D,0x07},
  {0x28,0xD5,0xDA,0x57,0x04,0xE1,0x3D,0xE0},
  {0x28,0x8D,0x17,0x57,0x04,0xE1,0x3D,0xA1}
};
const uint8_t NUM_DS = sizeof(DS_ADDR)/sizeof(DS_ADDR[0]);

// ───── Time ─────
constexpr long  GMT_OFFSET = 8*3600;     // GMT+8
constexpr char* NTP1       = "pool.ntp.org";
constexpr char* NTP2       = "time.nist.gov";
constexpr long  MIN_EPOCH  = 946684800L;  // 2000‑01‑01

// ───── Datalogging cadence ─────
constexpr uint8_t SAMPLES_HR   = 6;   // 10‑min = 6 per hr
constexpr uint8_t SAMPLE_STEP  = 10;  // every 10 min
constexpr uint8_t REPORT_SEC   = 5;   // hh:00:05

// ───── Globals ─────
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds(&oneWire);
DHT dht(DHT_PIN, DHT_TYPE);

float dsBuf[4][SAMPLES_HR];
float dhtTBuf[SAMPLES_HR], dhtHBuf[SAMPLES_HR];
uint8_t bufIdx = 0, validSamples = 0;
int lastSampleMin = -1, lastReportHr = -1;

// timers for continuous Cloud push
unsigned long lastFastRead = 0;
constexpr unsigned long FAST_READ_MS = 5000; // read sensors every 5 s

// ---- prototypes ----
void clearBuffers();
float avg(const float*, uint8_t);
void postSheet();
void syncNTP();
void readSensorsFast();

// ───────────── setup ─────────────
void setup(){
  Serial.begin(9600); delay(1500);
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  ds.begin(); dht.begin();
  configTime(GMT_OFFSET,0,NTP1,NTP2); syncNTP();
  clearBuffers();
  Serial.println(F("Init OK"));
}

// ───────────── loop ─────────────
void loop(){
  ArduinoCloud.update();

  // 1. FAST sensor read for Cloud every 5 s -------------
  if(millis()-lastFastRead>=FAST_READ_MS){
    lastFastRead = millis();
    readSensorsFast();
  }

  // 2. Time‑aligned sampling / reporting ---------------
  time_t now=time(nullptr); if(now<MIN_EPOCH) return; tm t; localtime_r(&now,&t);

  // 10‑min sample into buffer
  if(t.tm_min%SAMPLE_STEP==0 && t.tm_sec==0 && lastSampleMin!=t.tm_min){
    ds.requestTemperatures();
    for(uint8_t i=0;i<NUM_DS;i++){
      float tc = ds.getTempC(DS_ADDR[i]);
      dsBuf[i][bufIdx] = (tc==DEVICE_DISCONNECTED_C||tc==85||tc==-127)?NAN:tc;
    }
    float tt=dht.readTemperature(), hh=dht.readHumidity();
    dhtTBuf[bufIdx]=isnan(tt)?NAN:tt;
    dhtHBuf[bufIdx]=isnan(hh)?NAN:hh;
    validSamples = min<uint8_t>(validSamples+1,SAMPLES_HR);
    bufIdx = (bufIdx+1)%SAMPLES_HR;
    lastSampleMin = t.tm_min;
  }
  if(t.tm_min%SAMPLE_STEP!=0) lastSampleMin=-1;

  // hourly post
  if(t.tm_min==0 && t.tm_sec==REPORT_SEC && lastReportHr!=t.tm_hour && validSamples){
    postSheet(); clearBuffers(); bufIdx=0; validSamples=0; lastReportHr=t.tm_hour;
  }
}

// ───────────── functions ─────────────
void readSensorsFast(){
  // DS18B20 bulk read
  ds.requestTemperatures();
  float dsT[4];
  for(uint8_t i=0;i<NUM_DS;i++){
    float tc = ds.getTempC(DS_ADDR[i]);
    dsT[i] = (tc==DEVICE_DISCONNECTED_C||tc==85||tc==-127)?NAN:tc;
  }
  float dhtT=dht.readTemperature(), dhtH=dht.readHumidity();
  // push to Cloud (ON_UPDATE 10 s recommended in thingProperties.h)
  temp1 = dsT[0]; temp2 = dsT[1]; temp3 = dsT[2]; temp4 = dsT[3];
  dhtTemp = isnan(dhtT)?dhtTemp:dhtT;
  dhtHumi = isnan(dhtH)?dhtHumi:dhtH;
}

void postSheet(){
  Serial.println(F("[POST] hourly avg"));
  float a1=avg(dsBuf[0],validSamples), a2=avg(dsBuf[1],validSamples);
  float a3=avg(dsBuf[2],validSamples), a4=avg(dsBuf[3],validSamples);
  float at=avg(dhtTBuf,validSamples), ah=avg(dhtHBuf,validSamples);

  static WiFiClientSecure cli; cli.setBufferSizes(1024,512); cli.setInsecure();
  HTTPClient http; http.setTimeout(8000);
  char js[256]; snprintf(js,sizeof(js),
    "{\"sensor1\":%.2f,\"sensor2\":%.2f,\"sensor3\":%.2f,\"sensor4\":%.2f,\"dhttemp\":%.2f,\"dhthumidity\":%.2f}",
    a1,a2,a3,a4,at,ah);
  http.begin(cli,GOOGLE_SCRIPT_URL); http.addHeader("Content-Type","application/json");
  int code=http.POST(String(js)); Serial.printf("HTTP %d\n",code); http.end();
}

void clearBuffers(){ for(uint8_t i=0;i<SAMPLES_HR;i++){ for(uint8_t j=0;j<4;j++) dsBuf[j][i]=NAN; dhtTBuf[i]=dhtHBuf[i]=NAN;} }
float avg(const float* a,uint8_t n){ float s=0;uint8_t c=0; for(uint8_t i=0;i<n;i++) if(!isnan(a[i])){s+=a[i];c++;} return c? s/c : NAN; }
void syncNTP(){ Serial.print(F("NTP sync")); int t=20; time_t n=time(nullptr); while(n<MIN_EPOCH && t--){Serial.print('.'); delay(500); n=time(nullptr);} Serial.println(); }
