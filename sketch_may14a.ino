#include <Wire.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> // REQUIRED for https:// connections

// ------------------- CONFIGURATION ------------------- //
const char* ssid = "Samsung Galaxy A51";
const char* password = "11111111"; // Make sure this matches your router exactly!

// CLOUD URL (Render)
const char* serverUrl = "https://pathols.onrender.com/api/pothole"; 

// Hardware Pins (Standard ESP32 Defaults)
const int MPU_ADDR = 0x68;
const int SDA_PIN = 21;  // GPIO 21
const int SCL_PIN = 22;  // GPIO 22
const int GPS_RX_PIN = 16; // RX2
const int GPS_TX_PIN = 17; // TX2

// Tuned Thresholds
const float SHOCK_THRESHOLD = 3.0;      
const float GYRO_THRESHOLD = 30.0;      
const int DEBOUNCE_DELAY = 1500; 

// Variables
int16_t accel_z, gyro_y;
float Az, Gy;
float baselineZ = 9.8; 
unsigned long lastTriggerTime = 0;

// Objects
TinyGPSPlus gps;
HardwareSerial gpsSerial(2); // Use UART2

// Forward Declarations
void sendData(String type, float z_val, float y_val);
void readSensorRaw();

void setup() {
  Serial.begin(115200);
  
  // 1. Start I2C (Force Pins & Low Speed for stability)
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000); 

  // 2. Start GPS
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  Serial.println("\n--------------------------------");
  Serial.println("ESP32 Powering up...");
  delay(1000); 

  // 3. Wake Up MPU6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0); 
  byte error = Wire.endTransmission();

  if (error == 0) {
    Serial.println("✅ MPU6050 Found & Woken Up!");
  } else {
    Serial.print("❌ MPU Error: "); Serial.println(error);
  }

  // 4. Connect WiFi
  WiFi.mode(WIFI_STA); // Explicitly set Station Mode
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }
  
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
  } else {
    Serial.println("\n❌ WiFi Failed! Will try again in loop.");
  }

  // 5. Calibration
  Serial.println("⚖️ Calibrating... Keep still!");
  float sumZ = 0;
  for (int i = 0; i < 50; i++) {
    readSensorRaw();
    sumZ += (accel_z / 16384.0 * 9.8); 
    delay(20);
  }
  baselineZ = sumZ / 50.0;
  Serial.print("✅ Baseline Z: "); Serial.println(baselineZ);
  Serial.println("System Ready! Monitoring Road...");
}

void loop() {
  // 1. Read GPS (Always running)
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // 2. Read Sensor
  readSensorRaw();

  // Convert (2G Range defaults)
  Az = accel_z / 16384.0 * 9.8; 
  Gy = gyro_y / 131.0;

  // Calculate Shock
  float verticalShock = abs(Az - baselineZ);

  // --- LOGIC ---
  if (verticalShock > SHOCK_THRESHOLD) {
    if (millis() - lastTriggerTime > DEBOUNCE_DELAY) {
      
      // LOGIC: Gyro > 30 is POTHOLE, Gyro < -30 is SPEED BREAKER
      if (Gy > GYRO_THRESHOLD) { 
         Serial.println("\n--------------------------------");
         Serial.println("⚠️ POTHOLE DETECTED");
         printEventDetails(verticalShock);
         sendData("POTHOLE", Az, Gy);
         lastTriggerTime = millis();
      } 
      else if (Gy < -GYRO_THRESHOLD) {
         Serial.println("\n--------------------------------");
         Serial.println("⚠️ SPEED BREAKER DETECTED");
         printEventDetails(verticalShock);
         sendData("SPEED_BREAKER", Az, Gy);
         lastTriggerTime = millis();
      }
    }
  }
}

void printEventDetails(float shock) {
   Serial.print("📉 Shock Level: "); Serial.println(shock);
   Serial.print("🚗 Speed:       "); Serial.print(gps.speed.kmph()); Serial.println(" km/h");
   Serial.print("📍 Location:    "); 
   Serial.print(gps.location.lat(), 6);
   Serial.print(", ");
   Serial.println(gps.location.lng(), 6);
}

void readSensorRaw() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3F); 
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)2, true);
  if (Wire.available()) accel_z = Wire.read() << 8 | Wire.read();

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x45);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (size_t)2, true);
  if (Wire.available()) gyro_y = Wire.read() << 8 | Wire.read();
}

// ---------------------------------------------------------
//  OPTIMIZED SEND FUNCTION (FIXES TIMEOUT & SSL DELAY)
// ---------------------------------------------------------
void sendData(String type, float z_val, float y_val) {
  
  // 1. Force Reconnect if WiFi dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi dropped! Reconnecting...");
    WiFi.disconnect();
    WiFi.reconnect();
    // Block for up to 5 seconds to get connection back
    for(int i=0; i<50; i++) {
        if(WiFi.status() == WL_CONNECTED) break;
        delay(100);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    // 2. Use WiFiClientSecure for HTTPS
    WiFiClientSecure client;
    client.setInsecure(); // <--- CRITICAL: Skips slow certificate check!
    
    HTTPClient http;
    http.begin(client, serverUrl);
    
    // 3. INCREASE TIMEOUT for Render Free Tier (Prevents Error -11)
    http.setTimeout(15000); // Wait 15 Seconds before giving up
    
    http.addHeader("Content-Type", "application/json");

    float speed = gps.speed.kmph();
    double lat = gps.location.lat();
    double lng = gps.location.lng();

    String jsonPayload = "{";
    jsonPayload += "\"device_id\": \"ESP32_Standard\","; 
    jsonPayload += "\"event_type\": \"" + type + "\",";
    jsonPayload += "\"accel_z\": " + String(z_val) + ",";
    jsonPayload += "\"gyro_y\": " + String(y_val) + ",";
    jsonPayload += "\"speed_kmph\": " + String(speed) + ",";
    jsonPayload += "\"latitude\": " + String(lat, 6) + ",";
    jsonPayload += "\"longitude\": " + String(lng, 6) + ",";
    
    if (gps.time.isValid()) {
       char timeBuffer[10];
       sprintf(timeBuffer, "%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
       jsonPayload += "\"gps_time\": \"" + String(timeBuffer) + "\"";
    } else {
       jsonPayload += "\"gps_time\": \"waiting_for_fix\"";
    }
    jsonPayload += "}";

    // 4. SEND (This may still take 2-3 seconds on Render)
    int httpResponseCode = http.POST(jsonPayload);
    
    if (httpResponseCode > 0) {
      Serial.print("✅ Data Sent! Resp: "); 
      Serial.println(httpResponseCode); 
    } else {
      Serial.print("❌ Send Error: "); 
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("❌ WiFi Dead. Skipping send.");
  }
}
