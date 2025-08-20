#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <map>
#include <Preferences.h>

Preferences preferences;
std::map<String, int> cardTapCount;
std::map<String, String> cardCustomUID;

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

#define SERVER_URL "http://192.168.1.34/8paws/api/rfid_endpoint.php"

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
bool timeInitialized = false;

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
  delay(2000);  // Give Serial Monitor time to connect
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("🚀 ESP32 RFID SCANNER STARTING UP");
  Serial.println("========================================");
  
  // Initialize buzzer pin (simple on/off)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Try to connect to WiFi networks
  connectToWiFi();
  
  // IMPROVED TIME INITIALIZATION - Quick setup with fallback
  quickTimeSetup();
  
  Serial.println("🔧 Initializing RFID scanner...");
  
  // Initialize SPI bus
  SPI.begin();
  
  // Initialize MFRC522
  mfrc522.PCD_Init();
  
  // Check RFID scanner status
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  if ((version == 0x00) || (version == 0xFF)) {
    Serial.println("⚠️ WARNING: RFID Scanner communication failed!");
  } else {
    Serial.println("✅ RFID Scanner connected successfully!");
  }
  
  // Show details of PCD - MFRC522 Card Reader
  mfrc522.PCD_DumpVersionToSerial();
  
  // Initialize preferences and load card data
  preferences.begin("rfid_data", false);
  loadCardData();
  
  Serial.println("========================================");
  Serial.println("🎯 RFID READER READY!");
  Serial.println("📱 Tap an RFID card/tag to read its ID");
  Serial.println("⏱️ Hold card for 3 seconds for validation");
  Serial.println("========================================");
}

void connectToWiFi() {
  Serial.println("🌐 Connecting to WiFi networks...");
  
  for (int i = 0; i < numWiFiNetworks; i++) {
    Serial.printf("📡 Attempting: %s\n", wifiList[i].ssid);
    WiFi.begin(wifiList[i].ssid, wifiList[i].password);
    
    // Wait up to 10 seconds for connection (reduced from 15)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.printf("✅ Connected to: %s\n", wifiList[i].ssid);
      Serial.printf("🌐 IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("📶 Signal: %d dBm\n", WiFi.RSSI());
      return; // Exit function on successful connection
    } else {
      Serial.println();
      Serial.printf("❌ Failed: %s\n", wifiList[i].ssid);
      WiFi.disconnect();
      delay(1000);
    }
  }
  
  // If we reach here, no WiFi networks worked
  Serial.println("💥 ERROR: Could not connect to any WiFi network!");
  Serial.println("🔍 Please check your credentials and try again.");
  while(true) {
    Serial.println("⏰ No WiFi connection. Retrying in 30 seconds...");
    delay(30000);
    connectToWiFi(); // Retry the entire process
  }
}

// QUICK TIME SETUP - No hanging!
void quickTimeSetup() {
  Serial.println("⏰ Configuring time with NTP server (5 second max)...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int attempts = 0;
  
  while (!getLocalTime(&timeinfo) && attempts < 5) {
    Serial.print(".");
    delay(1000);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println();
    Serial.println("✅ Time synchronized successfully!");
    char timeString[64];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.println("📅 Current time: " + String(timeString));
    timeInitialized = true;
  } else {
    Serial.println();
    Serial.println("⚠️ NTP sync timeout - using system timestamps");
    Serial.println("✅ Ready to continue with fallback timing");
    timeInitialized = false;
  }
}

void loop() {
  // Check WiFi connection and reconnect if necessary
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("📶 WiFi connection lost. Attempting to reconnect...");
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
      Serial.println("🔄 RFID Reader Re-enabled!");
      Serial.println("🎯 Ready for next card...");
      Serial.println("----------------------------------------");
    } else {
      // Show countdown
      unsigned long timeLeft = (DISABLE_TIME - (millis() - disableStartTime)) / 1000 + 1;
      Serial.println("⏸️ RFID DISABLED - Reactivating in " + String(timeLeft) + " seconds...");
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
        Serial.println("🆕 New card detected - Starting 3-second validation...");
        
        // Single beep for new card
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
      }
      
      // Same card continues - check if 3 seconds have passed
      if (millis() - sameCardStartTime >= VALIDATION_TIME && !validationSuccessful) {
        // SUCCESS! 3 seconds completed
        Serial.println("🎉 *** SUCCESS! CARD VALIDATED! ***");
        Serial.println("✅ Card UID: " + currentCardID + " - APPROVED");
        Serial.println("🏁 Validation completed successfully!");
        Serial.println("----------------------------------------");
        
        // Send data to mysql immediately after successful validation
        sendToMySQL(currentCardID);
        
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
      Serial.println("🔄 Different card detected - Starting 3-second validation...");
      
      // Single beep for new card
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
    }
    
    // Show detection with countdown
    unsigned long timeHeld = millis() - sameCardStartTime;
    unsigned long timeLeft = (VALIDATION_TIME - timeHeld) / 1000 + 1;
    
    Serial.print("🔍 SCANNING - Card UID: ");
    Serial.println(currentCardID);
    Serial.println("🏷️ Card ID (String): " + currentCardID);
    if (timeLeft > 0 && !validationSuccessful) {
      Serial.println("⏳ Status: Hold for " + String(timeLeft) + " more seconds for validation");
    } else {
      Serial.println("🟢 Status: Card Present & Active");
    }
    Serial.println("----------------------------------------");
    
    lastCardID = currentCardID;
    cardWasPresent = true;
  }
  
  // Check if card has been gone for too long
  if (cardWasPresent && (millis() - lastDetectionTime > cardTimeout)) {
    Serial.println("📤 Card removed - Validation cancelled");
    Serial.println("⏳ Waiting for RFID card...");
    Serial.println("----------------------------------------");
    cardWasPresent = false;
    sameCardDetected = false;
    lastCardID = "";
    sameCardStartTime = 0;
  }
  
  // Shorter delay for more responsive detection
  delay(200);
}

// UPDATED TIME FUNCTIONS - MySQL Compatible Format
String getFastDateTime() {
  // Try to get real time quickly
  struct tm timeinfo;
  if (timeInitialized && getLocalTime(&timeinfo)) {
    char dateTime[64];
    // Use MySQL format: YYYY-MM-DD HH:MM:SS
    strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(dateTime);
  }
  
  // Fallback to system uptime with proper format
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds %= 60;
  minutes %= 60;
  hours %= 24;
  
  // Generate a fake but valid MySQL timestamp
  char sysTime[32];
  sprintf(sysTime, "2025-08-17 %02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(sysTime);
}

String getFastTimestamp() {
  // Return MySQL compatible timestamp format
  struct tm timeinfo;
  if (timeInitialized && getLocalTime(&timeinfo)) {
    char timestamp[32];
    // Use MySQL format instead of ISO 8601
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timestamp);
  }
  
  // Fallback timestamp in MySQL format
  unsigned long ms = millis();
  unsigned long seconds = ms / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  
  seconds %= 60;
  minutes %= 60;
  hours %= 24;
  
  char fallbackTime[32];
  sprintf(fallbackTime, "2025-08-17 %02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(fallbackTime);
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

void loadCardData() {
  Serial.println("📂 Loading saved card data...");
  
  // Load tap counts and custom UIDs from preferences
  size_t keyCount = preferences.getBytesLength("cardKeys");
  if (keyCount > 0) {
    char* keys = (char*)malloc(keyCount);
    preferences.getBytes("cardKeys", keys, keyCount);
    
    String keyString = String(keys);
    free(keys);
    
    // Parse stored card UIDs
    int startIndex = 0;
    int endIndex = keyString.indexOf(',');
    
    while (endIndex != -1) {
      String cardUID = keyString.substring(startIndex, endIndex);
      if (cardUID.length() > 0) {
        int tapCount = preferences.getInt(("tap_" + cardUID).c_str(), 0);
        String customUID = preferences.getString(("uid_" + cardUID).c_str(), "");
        
        if (tapCount > 0 && customUID.length() > 0) {
          cardTapCount[cardUID] = tapCount;
          cardCustomUID[cardUID] = customUID;
          Serial.println("📋 Loaded: " + cardUID + " - Taps: " + String(tapCount) + " - CustomUID: " + customUID);
        }
      }
      
      startIndex = endIndex + 1;
      endIndex = keyString.indexOf(',', startIndex);
    }
  }
  Serial.println("✅ Card data loading complete");
}

void saveCardData() {
  // Save all card UIDs as a comma-separated string
  String allKeys = "";
  
  for (auto& pair : cardTapCount) {
    allKeys += pair.first + ",";
    
    // Save individual tap count and custom UID
    preferences.putInt(("tap_" + pair.first).c_str(), pair.second);
    preferences.putString(("uid_" + pair.first).c_str(), cardCustomUID[pair.first]);
  }
  
  preferences.putBytes("cardKeys", allKeys.c_str(), allKeys.length());
  Serial.println("💾 Card data saved to preferences");
}

String getOrCreateCustomUID(String cardUID) {
  // Check if card exists in our tracking
  if (cardTapCount.find(cardUID) == cardTapCount.end()) {
    // New card - create first entry
    cardTapCount[cardUID] = 0;
    cardCustomUID[cardUID] = generateCustomUIDFromRFID(cardUID);
    Serial.println("🆕 New card registered: " + cardUID + " -> " + cardCustomUID[cardUID]);
  }
  
  // Increment tap count
  cardTapCount[cardUID]++;
  
  Serial.println("📊 Card: " + cardUID + " - Tap #" + String(cardTapCount[cardUID]) + "/5");
  
  // Check if we need to generate new customUID (after 5 taps - reset for reuse)
  if (cardTapCount[cardUID] > 5) {
    // Reset and create new customUID for reuse
    cardTapCount[cardUID] = 1;
    cardCustomUID[cardUID] = generateCustomUIDFromRFID(cardUID + String(millis())); // Add timestamp for uniqueness
    Serial.println("🔄 Card completed cycle! New CustomUID generated: " + cardCustomUID[cardUID]);
    Serial.println("🎫 Card ready for new customer assignment");
  }
  
  // Save data to persistent storage
  saveCardData();
  
  return cardCustomUID[cardUID];
}

// Update the JSON data structure in sendToMySQL function
void sendToMySQL(String cardUID) {
    Serial.println("========================================");
    Serial.println("🔄 PREPARING TO SEND DATA TO DATABASE");
    Serial.println("========================================");
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ No WiFi connection. Cannot send data.");
        return;
    }
    
    Serial.println("📝 Processing card data...");
    
    // Keep existing Custom UID logic
    String customUID = getOrCreateCustomUID(cardUID);
    int currentTapCount = cardTapCount[cardUID];
    
    // Use FAST time functions (no hanging!)
    String currentDateTime = getFastDateTime();
    String currentTimestamp = getFastTimestamp();
    String rfidStatus = getRFIDStatus();
    
    Serial.println("🕒 Time: " + currentDateTime);
    Serial.println("🆔 Custom UID: " + customUID);
    Serial.println("🔢 Tap count: " + String(currentTapCount));
    
    // Create JSON matching your database structure - UPDATED max_taps to 5
    DynamicJsonDocument doc(1024);
    doc["card_uid"] = cardUID;
    doc["custom_uid"] = customUID;
    doc["tap_count"] = currentTapCount;
    doc["max_taps"] = 5;  // CHANGED from 4 to 5
    doc["tap_number"] = currentTapCount;
    doc["device_info"] = "ESP32-RFID-Scanner";
    doc["wifi_network"] = WiFi.SSID();
    doc["signal_strength"] = WiFi.RSSI();
    doc["validation_status"] = "approved";
    doc["readable_time"] = currentDateTime;
    doc["timestamp_value"] = currentTimestamp;
    doc["rfid_scanner_status"] = rfidStatus;
    doc["validation_time_ms"] = VALIDATION_TIME;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    Serial.println("📄 JSON Data: " + jsonString);
    Serial.println("🌐 Sending to: " + String(SERVER_URL));
    
    // Send HTTP POST
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    
    Serial.println("📡 Sending HTTP request...");
    int httpResponseCode = http.POST(jsonString);
    
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("📥 Server Response Code: " + String(httpResponseCode));
        
        if (httpResponseCode == 200) {
            Serial.println("🎉 SUCCESS! Data sent to MySQL database");
            Serial.println("🏷️ Card UID: " + cardUID);
            Serial.println("🆔 Custom UID: " + customUID);
            Serial.println("📊 Tap Count: " + String(currentTapCount) + "/5");
        } else {
            Serial.println("⚠️ Server responded with non-200 code: " + String(httpResponseCode));
        }
        Serial.println("📝 Response: " + response);
    } else {
        Serial.println("❌ Failed to send to database");
        Serial.println("💥 Error Code: " + String(httpResponseCode));
        Serial.println("🔍 Possible issues:");
        Serial.println("   • Server not running");
        Serial.println("   • Wrong URL");
        Serial.println("   • Network connectivity");
    }
    
    http.end();
    Serial.println("🔚 HTTP connection closed");
    Serial.println("========================================");
}