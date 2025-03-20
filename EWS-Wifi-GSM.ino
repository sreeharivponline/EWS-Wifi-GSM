#include <ESP8266WiFi.h>
#include <ModbusMaster.h>
#include <SoftwareSerial.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <NewPing.h>

// Pin Assignments
#define TRIG_PIN D3        
#define ECHO_PIN D4        
#define ANEMOMETER_PIN A0  
#define RAIN_SENSOR_PIN D5 
#define RO_PIN D1
#define DI_PIN D2
#define RELAY_PIN D6

float Rainfall;
float WindSpeed;
int RainStatus;
String updateDate;

#define MAX_DISTANCE 400
NewPing sonar(TRIG_PIN, ECHO_PIN, MAX_DISTANCE);

// WiFi and Firebase credentials
#define API_KEY "YOUR_FIREBASE_API_KEY"
#define PROJECT_ID "YOUR_FIREBASE_PROJECT_ID"
#define USER_EMAIL "YOUR_EMAIL"
#define USER_PASSWORD "YOUR_PASSWORD"
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

// EC200U CN GSM Module Pins
#define EC200U_TX D7  
#define EC200U_RX D8  
SoftwareSerial ec200u(EC200U_RX, EC200U_TX);  

// Modbus & Firebase
SoftwareSerial RS485Serial(RO_PIN, DI_PIN);
ModbusMaster node;
FirebaseData firebaseData;
FirebaseAuth firebaseAuth;
FirebaseConfig firebaseConfig;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

bool wifiConnected = false;

void connectWiFi() {
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    Serial.println("\nWiFi connected");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi");
  }
}

void sendATCommand(String command, int waitTime = 2000) {
  Serial.println("Sending: " + command);
  ec200u.println(command);
  delay(waitTime);
  String response = "";
  while (ec200u.available()) {
    response += (char)ec200u.read();
  }
  Serial.println("Response: " + response);
}

void sendDataToFirebase(int waterLevel, float windSpeed, float rainfall, int rainStatus, String updateDate, String updateTimeString) {
  String jsonData = "{\"fields\": {\"waterLevel\": {\"integerValue\": " + String(waterLevel) +
                    "}, \"WindSpeed\": {\"doubleValue\": " + String(windSpeed) +
                    "}, \"Rainfall\": {\"doubleValue\": " + String(rainfall) +
                    "}, \"RainStatus\": {\"integerValue\": " + String(rainStatus) +
                    "}, \"updateDate\": {\"stringValue\": \"" + updateDate +
                    "\"}, \"updateTimeString\": {\"stringValue\": \"" + updateTimeString + "\"}}}";

  if (wifiConnected) {
    Serial.println("Sending data via WiFi...");
    FirebaseJson content;
    content.setJsonData(jsonData);
    String documentPath = "Flood-data/EWS-Main";
    if (Firebase.Firestore.patchDocument(&firebaseData, PROJECT_ID, "", documentPath.c_str(), content.raw(), "")) {
      Serial.println("Data sent successfully via WiFi");
    } else {
      Serial.printf("Failed to send data via WiFi: %s\n", firebaseData.errorReason().c_str());
    }
  } else {
    Serial.println("Sending data via EC200U...");
    sendATCommand("AT+CREG?", 1000);
    sendATCommand("AT+CGATT?", 1000);
    sendATCommand("AT+QIACT?", 2000);
    sendATCommand("AT+QHTTPCFG=\"sslctxid\",1", 1000);
    sendATCommand("AT+QSSLCFG=\"seclevel\",1,0", 1000);
    sendATCommand("AT+QHTTPCFG=\"requestheader\",1", 1000);
    sendATCommand("AT+QHTTPHEADER=29", 2000);
    ec200u.println("Content-Type: application/json");
    delay(500);
    ec200u.println();
    sendATCommand("AT+QHTTPURL=" + String(jsonData.length()) + ",80", 2000);
    ec200u.print(jsonData);
    delay(2000);
    sendATCommand("AT+QHTTPPOST=" + String(jsonData.length()) + ",80,80", 5000);
    sendATCommand("AT+QHTTPREAD=4000", 5000);
  }
}

void setup() {
  Serial.begin(115200);
  RS485Serial.begin(4800);
  ec200u.begin(115200);
  node.begin(1, RS485Serial);
  connectWiFi();

  firebaseConfig.api_key = API_KEY;
  firebaseAuth.user.email = USER_EMAIL;
  firebaseAuth.user.password = USER_PASSWORD;
  firebaseConfig.token_status_callback = tokenStatusCallback;
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Firebase.reconnectWiFi(true);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RAIN_SENSOR_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  timeClient.begin();
}

float getUltrasonicDistance() {
  int distance_cm = sonar.ping_cm();
  return (distance_cm == 0) ? MAX_DISTANCE : distance_cm;
}

float getWindSpeed() {
  int analog_value = analogRead(ANEMOMETER_PIN);
  float voltage = (analog_value / 1023.0) * 3.3;
  return (voltage - 0.4) * (32.4 / 2.0);
}

void loop() {
  // Get Rainfall from Modbus
  uint8_t result = node.readHoldingRegisters(0x0000, 1);
  if (result == node.ku8MBSuccess) {
    Rainfall = node.getResponseBuffer(0) / 10.0;
  }

  // Read Sensor Data
  float waterLevel = getUltrasonicDistance();
  int sensorValue = analogRead(ANEMOMETER_PIN);
  WindSpeed = map(sensorValue, 0, 1023, 0, 30);
  RainStatus = digitalRead(RAIN_SENSOR_PIN);

  // Get Date & Time
  timeClient.update();
  String updateTimeString = timeClient.getFormattedTime();
  time_t rawTime = timeClient.getEpochTime();
  if (rawTime > 0) {
    rawTime += 19800;
    struct tm timeInfo;
    localtime_r(&rawTime, &timeInfo);
    char dateBuffer[20];
    sprintf(dateBuffer, "%04d-%02d-%02d", timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday);
    updateDate = String(dateBuffer);
  }

  // Print Data
  Serial.println("=================================");
  Serial.print("RainStatus: "); Serial.println(RainStatus);
  Serial.print("Rainfall: "); Serial.println(Rainfall);
  Serial.print("WindSpeed: "); Serial.println(WindSpeed);
  Serial.print("waterLevel: "); Serial.println(waterLevel);
  Serial.print("updateDate: "); Serial.println(updateDate);
  Serial.print("updateTimeString: "); Serial.println(updateTimeString);
  Serial.println("=================================\n");

  waterLevel = 400 - waterLevel;
  sendDataToFirebase(waterLevel, WindSpeed, Rainfall, RainStatus, updateDate, updateTimeString);

  delay(500);
}
