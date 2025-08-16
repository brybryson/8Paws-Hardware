#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// WiFi credentials array for fallback connections
struct WiFiCredentials {
  const char* ssid;
  const char* password;
};

WiFiCredentials wifiList[] = {
  {"PLDTHOMEFIBRc11f8", "PLDTWIFIg9y9y"},
  {"WiFi_Network_2", "password2"},
  {"WiFi_Network_3", "password3"}
  // Add more WiFi credentials as needed
};
const int numWiFiNetworks = sizeof(wifiList) / sizeof(wifiList[0]);

// Firebase configuration - Using REST API for Firestore
#define API_KEY "AIzaSyAng8ReVdMslHIpKLRNx1qi3bhU_idFr_c"
#define PROJECT_ID "pawsproject-1379a"
#define FIRESTORE_URL "https://firestore.googleapis.com/v1/projects/" PROJECT_ID "/databases/(default)/documents"

// Pin definitions for ESP32
#define SS_PIN    21    // SDA/SS pin for RFID
#define RST_PIN   22    // Reset pin for RFID
#define BUZZER_PIN 17   // Buzzer pin

// Create MFRC522 instance
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Time configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 28800; // GMT+8 for Philippines (8 * 3600)
const int daylightOffset_sec = 0;

// Variables for better detection
String lastCardID = "";
unsigned long lastDetectionTime = 0;
unsigned long cardTimeout = 2000; // Consider card "gone" after 2 seconds of no detection
bool cardWasPresent = false;

// Variables for 3-second validation and 5-second disable
unsigned long sameCardStartTime = 0;
bool sameCardDetected = false;
bool validationSuccessful = false;
unsigned long disableStartTime = 0;
bool rfidDisabled = false;
const unsigned long VALIDATION_TIME = 3000; // 3 seconds
const unsigned long DISABLE_TIME = 5000; // 5 seconds

// Function to generate 8-character alphanumeric Custom UID (completely random)
String generateCustomUID() {
  const char alphaNum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const int alphaNumLength = sizeof(alphaNum) - 1;
  String customUID = "";
  
  // Seed random number generator with current time and hardware values
  randomSeed(millis() + ESP.getCycleCount());
  
  // Generate 8 random alphanumeric characters
  for (int i = 0; i < 8; i++) {
    int randomIndex = random(0, alphaNumLength);
    customUID += alphaNum[randomIndex];
  }
  
  return customUID;
}

// Function to generate Custom UID based on RFID UID (consistent for same card)
String generateCustomUIDFromRFID(String rfidUID) {
  // Remove colons and convert to uppercase
  String cleanUID = rfidUID;
  cleanUID.replace(":", "");
  cleanUID.toUpperCase();
  
  // Use the RFID UID as seed for consistent generation
  unsigned long seed = 0;
  for (int i = 0; i < cleanUID.length(); i++) {
    seed += cleanUID.charAt(i) * (i + 1);
  }
  
  const char alphaNum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const int alphaNumLength = sizeof(alphaNum) - 1;
  String customUID = "";
  
  // Use seed for consistent random generation
  randomSeed(seed);
  
  // Generate 8 characters
  for (int i = 0; i < 8; i++) {
    int randomIndex = random(0, alphaNumLength);
    customUID += alphaNum[randomIndex];
  }
  
  return customUID;
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  
  // Initialize buzzer pin (simple on/off)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Try to connect to WiFi networks
  connectToWiFi();
  
  // Initialize time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Getting time from NTP server...");
  delay(2000);
  
  Serial.println("Firebase Firestore (REST API) initialized!");
  
  // Initialize SPI bus
  SPI.begin();
  
  // Initialize MFRC522
  mfrc522.PCD_Init();
  
  // Check RFID scanner status
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if ((version == 0x00) || (version == 0xFF)) {
    Serial.println("WARNING: RFID Scanner communication failed!");
  } else {
    Serial.println("RFID Scanner connected successfully!");
  }
  
  // Show details of PCD - MFRC522 Card Reader
  mfrc522.PCD_DumpVersionToSerial();
  
  Serial.println("RFID Reader Ready!");
  Serial.println("Tap an RFID card/tag to read its ID...");
  Serial.println("Hold card for 3 seconds for successful validation!");
  Serial.println("----------------------------------------");
}

void connectToWiFi() {
  Serial.println("Trying to connect to WiFi networks...");
  
  for (int i = 0; i < numWiFiNetworks; i++) {
    Serial.printf("Attempting to connect to: %s\n", wifiList[i].ssid);
    WiFi.begin(wifiList[i].ssid, wifiList[i].password);
    
    // Wait up to 15 seconds for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.printf("Successfully connected to: %s\n", wifiList[i].ssid);
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.print("Signal strength (RSSI): ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      return; // Exit function on successful connection
    } else {
      Serial.println();
      Serial.printf("Failed to connect to: %s\n", wifiList[i].ssid);
      WiFi.disconnect();
      delay(1000);
    }
  }
  
  // If we reach here, no WiFi networks worked
  Serial.println("ERROR: Could not connect to any WiFi network!");
  Serial.println("Please check your credentials and try again.");
  // You might want to restart the ESP32 or enter a retry loop here
  while(true) {
    Serial.println("No WiFi connection. Retrying in 30 seconds...");
    delay(30000);
    connectToWiFi(); // Retry the entire process
  }
}

void loop() {
  // Check WiFi connection and reconnect if necessary
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost. Attempting to reconnect...");
    connectToWiFi();
  }
  
  // Check if RFID is disabled (5-second timeout after successful validation)
  if (rfidDisabled) {
    if (millis() - disableStartTime >= DISABLE_TIME) {
      // Re-enable RFID after 5 seconds
      rfidDisabled = false;
      validationSuccessful = false;
      sameCardDetected = false;
      lastCardID = "";
      Serial.println("RFID Reader Re-enabled!");
      Serial.println("Ready for next card...");
      Serial.println("----------------------------------------");
    } else {
      // Show countdown
      unsigned long timeLeft = (DISABLE_TIME - (millis() - disableStartTime)) / 1000 + 1;
      Serial.println("RFID DISABLED - Reactivating in " + String(timeLeft) + " seconds...");
      delay(1000);
      return;
    }
  }

  bool cardDetected = false;
  String currentCardID = "";
  
  // Try to detect card
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    cardDetected = true;
    lastDetectionTime = millis();
    
    // Build card ID string
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) {
        currentCardID += "0";
      }
      currentCardID += String(mfrc522.uid.uidByte[i], HEX);
      if (i < mfrc522.uid.size - 1) {
        currentCardID += ":";
      }
    }
    
    // Check if same card as before
    if (currentCardID == lastCardID) {
      if (!sameCardDetected) {
        // First time detecting this specific card
        sameCardStartTime = millis();
        sameCardDetected = true;
        Serial.println("New card detected - Starting 3-second validation...");
        
        // Single beep for new card
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
      }
      
      // Same card continues - check if 3 seconds have passed
      if (millis() - sameCardStartTime >= VALIDATION_TIME && !validationSuccessful) {
        // SUCCESS! 3 seconds completed
        Serial.println("*** SUCCESS! CARD VALIDATED! ***");
        Serial.println("Card UID: " + currentCardID + " - APPROVED");
        Serial.println("Validation completed successfully!");
        Serial.println("----------------------------------------");
        
        // Send data to Firestore immediately after successful validation
        sendToFirestore(currentCardID);
        
        // Buzzer success pattern (3 short beeps)
        for(int i = 0; i < 3; i++) {
          digitalWrite(BUZZER_PIN, HIGH);
          delay(200);
          digitalWrite(BUZZER_PIN, LOW);
          delay(200);
        }
        
        validationSuccessful = true;
        rfidDisabled = true;
        disableStartTime = millis();
        return;
      }
    } else {
      // Different card detected - reset everything
      sameCardStartTime = millis();
      sameCardDetected = true;
      Serial.println("Different card detected - Starting 3-second validation...");
      
      // Single beep for new card
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
    }
    
    // Show detection with countdown
    unsigned long timeHeld = millis() - sameCardStartTime;
    unsigned long timeLeft = (VALIDATION_TIME - timeHeld) / 1000 + 1;
    
    Serial.print("SCANNING - Card UID: ");
    Serial.println(currentCardID);
    Serial.println("Card ID (String): " + currentCardID);
    if (timeLeft > 0 && !validationSuccessful) {
      Serial.println("Status: Hold for " + String(timeLeft) + " more seconds for validation");
    } else {
      Serial.println("Status: Card Present & Active");
    }
    Serial.println("----------------------------------------");
    
    lastCardID = currentCardID;
    cardWasPresent = true;
  }
  
  // Check if card has been gone for too long
  if (cardWasPresent && (millis() - lastDetectionTime > cardTimeout)) {
    Serial.println("Card removed - Validation cancelled");
    Serial.println("Waiting for RFID card...");
    Serial.println("----------------------------------------");
    cardWasPresent = false;
    sameCardDetected = false;
    lastCardID = "";
    sameCardStartTime = 0;
  }
  
  // Shorter delay for more responsive detection
  delay(200);
}

// Function to get current date and time
String getCurrentDateTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "Time Error";
  }
  
  char dateTime[64];
  strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(dateTime);
}

// Function to get timestamp in milliseconds (for Firestore timestamp)
String getCurrentTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "1970-01-01T00:00:00Z";
  }
  
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(timestamp);
}

// Function to check RFID scanner status
String getRFIDStatus() {
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if ((version == 0x00) || (version == 0xFF)) {
    return "ERROR";
  } else {
    return "OK";
  }
}

// Function to send data to Firestore using REST API (NOW WITH CUSTOM UID)
void sendToFirestore(String cardUID) {
  Serial.println("========================================");
  Serial.println("🔄 PREPARING TO SEND DATA TO FIRESTORE");
  Serial.println("========================================");
  
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ No WiFi connection. Cannot send data to Firestore.");
    return;
  }
  
  Serial.println("Checking connection status...");
  Serial.println("WiFi status: ✅ CONNECTED");
  
  // Generate Custom UID based on RFID UID (consistent for same card)
  String customUID = generateCustomUIDFromRFID(cardUID);
  
  String currentDateTime = getCurrentDateTime();
  String currentTimestamp = getCurrentTimestamp();
  String rfidStatus = getRFIDStatus();
  
  // Generate organized document ID with date and time
  struct tm timeinfo;
  String documentID = "card_ERROR_" + String(millis()); // fallback
  
  if(getLocalTime(&timeinfo)){
  char dateStr[16]; // MMDDYYYY format
  char timeStr[20]; // HHMMSSAM/PM format (increased size for seconds)
  
  // Format date as MMDDYYYY (e.g., 08162025)
  strftime(dateStr, sizeof(dateStr), "%m%d%Y", &timeinfo);
  
  // Format time as HHMMSSAM/PM (e.g., 044637PM) - now includes seconds
  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;
  int second = timeinfo.tm_sec;
  String ampm = (hour >= 12) ? "PM" : "AM";
  
  // Convert to 12-hour format
  if (hour > 12) hour -= 12;
  if (hour == 0) hour = 12;
  
  sprintf(timeStr, "%02d%02d%02d%s", hour, minute, second, ampm.c_str());
  
  documentID = "card_" + String(dateStr) + "_" + String(timeStr);
}
  
  Serial.println("📁 Collection: rfid_validations");
  Serial.println("📄 Document ID: " + documentID);
  Serial.println("🏷️ Card UID: " + cardUID);
  Serial.println("🆔 Custom UID: " + customUID);
  Serial.println("⏰ Timestamp: " + currentDateTime);
  
  // Create JSON document for Firestore
  DynamicJsonDocument doc(1024);
  
  // Create fields object with Firestore format - NOW INCLUDING CustomUID
  doc["fields"]["cardUID"]["stringValue"] = cardUID;
  doc["fields"]["customUID"]["stringValue"] = customUID; // NEW FIELD
  doc["fields"]["status"]["stringValue"] = "APPROVED";
  doc["fields"]["message"]["stringValue"] = "CARD VALIDATED";
  doc["fields"]["timestamp"]["timestampValue"] = currentTimestamp;
  doc["fields"]["readableTime"]["stringValue"] = currentDateTime;
  doc["fields"]["rfidScannerStatus"]["stringValue"] = rfidStatus;
  doc["fields"]["deviceInfo"]["stringValue"] = "ESP32-RFID-Scanner";
  doc["fields"]["wifiNetwork"]["stringValue"] = WiFi.SSID();
  doc["fields"]["signalStrength"]["integerValue"] = WiFi.RSSI();
  doc["fields"]["validationTime"]["integerValue"] = VALIDATION_TIME;
  doc["fields"]["projectId"]["stringValue"] = PROJECT_ID;
  
  // Convert JSON to string
  String jsonString;
  serializeJson(doc, jsonString);
  
  Serial.println("📄 JSON Data prepared:");
  Serial.println(jsonString);
  
  // Prepare HTTP client
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); // For testing purposes
  
  // Build Firestore REST API URL
  String url = String(FIRESTORE_URL) + "/rfid_validations/" + documentID + "?key=" + API_KEY;
  
  Serial.println("🔗 URL: " + url);
  Serial.println("🔄 Sending HTTP PATCH request to Firestore...");
  
  // Send HTTP PATCH request to create/update document
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  
  int httpResponseCode = http.PATCH(jsonString);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    
    if (httpResponseCode == 200) {
      Serial.println("========================================");
      Serial.println("🎉 SUCCESS! DOCUMENT SENT TO FIRESTORE!");
      Serial.println("📁 Collection: rfid_validations");
      Serial.println("📄 Document ID: " + documentID);
      Serial.println("🏷️ Card UID: " + cardUID);
      Serial.println("🆔 Custom UID: " + customUID);
      Serial.println("⏰ Time: " + currentDateTime);
      Serial.println("📊 Response Code: " + String(httpResponseCode));
      Serial.println("🌐 Check your Firebase Console -> Firestore Database!");
      Serial.println("========================================");
    } else {
      Serial.println("========================================");
      Serial.println("⚠️ FIRESTORE RESPONSE (Non-200):");
      Serial.println("📊 Response Code: " + String(httpResponseCode));
      Serial.println("📄 Response: " + response);
      Serial.println("========================================");
    }
  } else {
    Serial.println("========================================");
    Serial.println("❌ FAILED to send document to Firestore");
    Serial.println("📊 HTTP Error Code: " + String(httpResponseCode));
    Serial.println("❌ Check your internet connection and API key");
    Serial.println("========================================");
  }
  
  http.end();
}