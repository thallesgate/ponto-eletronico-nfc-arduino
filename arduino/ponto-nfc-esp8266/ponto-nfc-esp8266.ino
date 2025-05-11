#include <MFRC522.h>
#include <MFRC522Extended.h>
#include <require_cpp11.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>            // Added for mDNS
#include <WiFiUdp.h>                // Already present, but used by ArduinoOTA and NTP
#include <ArduinoOTA.h>             // Added for ArduinoOTA
#include <ESP8266HTTPUpdateServer.h> // Added for Web Updater
#include <EEPROM.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <TimeLib.h>

#include "credentials.h" // Include the new credentials file

// Pin definitions for NodeMCU (ESP8266)
#define RST_PIN D3
#define SS_PIN D4
#define LCD_BACKLIGHT_PIN D0  // Choose an appropriate PWM-capable pin

// Constants
#define MAX_USERS 50
#define MAX_RECORDS 200
#define NAME_LENGTH 16
#define USER_DATA_START 0
#define RECORDS_DATA_START 2000
#define RECORDS_COUNT_ADDR 3900
#define USERS_COUNT_ADDR 3950
#define WIFI_MODE_ADDR 3960  // New EEPROM address for WiFi mode setting
#define WIFI_CHECK_INTERVAL 21600000 // Check WiFi connectivity every 6 hours (6 * 60 * 60 * 1000 ms)
#define WIFI_CONNECTION_TIMEOUT 20000 // 20 seconds timeout for WiFi connection
#define BACKLIGHT_DEFAULT 200   // Default brightness (0-255)
#define BACKLIGHT_LOW 20        // Low brightness for power saving
#define BACKLIGHT_TIMEOUT 30000 // Time in ms to dim the display after inactivity
#define REGISTRATION_TIMEOUT 60000  // 60 segundos para timeout do registro

// WiFi settings
const char* hostname = "redbone-ponto";

// NTP settings
const long utcOffsetInSeconds = -3 * 3600;  // Brazil timezone (UTC-3)

// Objects
MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
ESP8266HTTPUpdateServer httpUpdater; // Added for Web Updater

// Data structures
struct User {
  byte uid[4];
  char name[NAME_LENGTH + 1];
  bool lastState;  // true = in, false = out
};

struct Record {
  byte uid[4];
  byte timestamp[4];  // Em vez de unsigned long timestamp
  bool isIn;  // true = punch in, false = punch out
};

// Global variables
User users[MAX_USERS];
int numUsers = 0;
int numRecords = 0;
unsigned long lastCardRead = 0;
const unsigned long cardReadDelay = 1000;  // 2 seconds between reads
String serialCommand = "";
bool commandComplete = false;
Record records[MAX_RECORDS];
bool waitingForName = false;
int registrationIndex = -1;
unsigned long lastWifiConnectAttempt = 0;
bool isInAPMode = true;
byte backlightLevel = BACKLIGHT_DEFAULT;
unsigned long lastActivityTime = 0;
bool backlightDimmed = false;
unsigned long registrationStartTime = 0;
bool ntpSynced = false;
bool stayConnectedToWiFi = false;  // Flag to determine if we should maintain WiFi connection

// Login credentials and status
bool isLoggedIn = false;

// Adicione esta variável global para rastrear o último minuto exibido
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 20000; // Atualiza a cada 10 segundos

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  
  // Initialize dedicated backlight pin
  pinMode(LCD_BACKLIGHT_PIN, OUTPUT);
  setBacklight(backlightLevel);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Redbone Ponto");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");

  // Initialize SPI
  SPI.begin();
  
  // Initialize MFRC522
  mfrc522.PCD_Init();
  delay(4);
  
  // Initialize EEPROM
  EEPROM.begin(4096);
  
  // Read stored data
  loadUserData();
  loadRecordCount();
  loadWiFiModeSetting(); // Load WiFi mode setting from EEPROM

  // Setup WiFi
  setupWiFi(); // Ensures WiFi is up before OTA/MDNS setup

  // Initialize NTP
  timeClient.begin();
  
  // Immediately try to sync time after connecting to WiFi
  if (WiFi.status() == WL_CONNECTED || !isInAPMode) {
    syncTimeNTP();
  } else if (isInAPMode) {
    // In AP mode, attempt immediate temporary connection for time sync
    connectToWiFiForTimeUpdate();
  }
  
  // Setup web server
  setupWebServer(); // server.begin() is called here

  // Setup MDNS responder
  if (MDNS.begin(hostname)) {
    Serial.println("MDNS responder started");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error setting up MDNS responder!");
  }

  // Setup OTA (Over-The-Air) Updates from Arduino IDE
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.setPassword(otaPassword);  // Use password from credentials.h

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Atualizacao OTA");
    lcd.setCursor(0, 1);
    lcd.print("Iniciando...");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    lcd.setCursor(0, 1);
    lcd.print("Concluido!");
    delay(1000);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percentage = (progress / (total / 100));
    Serial.printf("Progress: %u%%\r", percentage);
    lcd.setCursor(0, 1);
    lcd.print("Progresso: " + String(percentage) + "%");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Erro OTA!");
    lcd.setCursor(0, 1);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    lcd.print("Erro: " + String(error));
    delay(2000);
  });
  ArduinoOTA.begin();
  Serial.println("ArduinoOTA Ready");

  // Setup HTTP Update Server (Web Updater)
  // This will use the same adminUser and adminPass for authentication on the /update path
  httpUpdater.setup(&server, adminUser, adminPass); 
  Serial.println("HTTPUpdateServer ready. Access at /update");
  
  // Display ready message
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Redbone Ponto");
  lcd.setCursor(0, 1);
  lcd.print("Pronto!");
  delay(2000);
  
  updateIdleDisplay();
}

void loop() {
  // Handle web server clients (also handles httpUpdater)
  server.handleClient();
  
  // Handle Arduino OTA updates
  ArduinoOTA.handle();
  
  // Update time from NTP server
  timeClient.update();
  
  // Atualizar display ocioso a cada intervalo definido
  unsigned long currentMillis = millis();
  if (currentMillis - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = currentMillis;
    
    // Somente atualiza se não estiver no meio de alguma operação
    if (!waitingForName) {
      updateIdleDisplay();
    }
  }
  
  // Check for Serial commands
  checkSerialCommands();
  
  // NFC card detection
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    if (currentMillis - lastCardRead > cardReadDelay) {
      lastCardRead = currentMillis;
      handleCardRead();
    }
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  
  // Check for registration timeout
  if (waitingForName && (millis() - registrationStartTime > REGISTRATION_TIMEOUT)) {
    waitingForName = false;
    registrationIndex = -1;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Tempo esgotado!");
    lcd.setCursor(0, 1);
    lcd.print("Registro cancel.");
    Serial.println("Registration timeout - canceled");
    delay(2000);
    
    updateIdleDisplay();
  }
  
  // Check if it's time to attempt WiFi connection for time update
  if (currentMillis - lastWifiConnectAttempt > WIFI_CHECK_INTERVAL) {
    lastWifiConnectAttempt = currentMillis;
    
    // Only attempt to connect to WiFi if we're not in the middle of card registration
    if (!waitingForName) {
      connectToWiFiForTimeUpdate();
    }
  }

  // Check for backlight timing
  manageBacklight();
}

// Adicione esta nova função para atualizar o display quando ocioso
void updateIdleDisplay() {
  // Obter hora atual
  time_t currentTime = timeClient.getEpochTime();
  int hours = hour(currentTime);
  int minutes = minute(currentTime);
  
  // Formatar hora como HH:MM
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", hours, minutes);
  
  // Atualizar display
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("REDBONE ");
  lcd.print(timeStr);
  lcd.setCursor(0, 1);
  lcd.print("Aproxime a tag.");
}

void handleCardRead() {
  // Update activity time and ensure backlight is on
  if (backlightDimmed) {
    setBacklight(BACKLIGHT_DEFAULT);
  } else {
    lastActivityTime = millis();
  }
  
  // Get card UID
  byte cardUID[4];
  for (byte i = 0; i < 4; i++) {
    cardUID[i] = mfrc522.uid.uidByte[i];
  }
  
  // Verificar se o cartão já existe
  int userIndex = findUserByUID(cardUID);
  
  if (userIndex == -1) {
    // Cartão não encontrado - iniciar automaticamente o registro
    startRegistration(cardUID);
  } else {
    // Cartão encontrado - processar entrada/saída normalmente
    processPunchInOut(cardUID);
  }
}

void startRegistration(byte cardUID[4]) {
  // Check if we have space for new user
  if (numUsers >= MAX_USERS) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Memoria cheia!");
    lcd.setCursor(0, 1);
    lcd.print("Limite alcancado");
    Serial.println("User memory full!");
    delay(2000);
    
    updateIdleDisplay();
    return;
  }
  
  // Display instructions for name input
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Nova tag!");
  lcd.setCursor(0, 1);
  lcd.print("Registre via Web");
  
  // Wait for name input via Serial or web
  Serial.print("Enter name for new card (UID: ");
  for (byte i = 0; i < 4; i++) {
    Serial.print(cardUID[i] < 0x10 ? " 0" : " ");
    Serial.print(cardUID[i], HEX);
  }
  Serial.println("):");
  
  // Store temporary UID for registration
  memcpy(users[numUsers].uid, cardUID, 4);
  
  // Registration will be completed when name is received via serial or web
  waitingForName = true;
  registrationIndex = numUsers;
  registrationStartTime = millis();  // Inicia o timer de timeout
}

void processPunchInOut(byte cardUID[4]) {
  int userIndex = findUserByUID(cardUID);
  
  if (userIndex == -1) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Cartao nao");
    lcd.setCursor(0, 1);
    lcd.print("registrado!");
    delay(2000);
    
    updateIdleDisplay();
    return;
  }
  
  // Get current time
  time_t currentTime = timeClient.getEpochTime();
  
  // Update user's state (in/out)
  users[userIndex].lastState = !users[userIndex].lastState;
  saveUserData();
  
  // Add record
  if (numRecords < MAX_RECORDS) {
    memcpy(records[numRecords].uid, cardUID, 4);
    
    // Converter o timestamp em bytes individuais para evitar problemas de endianness
    unsigned long epochTime = currentTime;
    records[numRecords].timestamp[0] = epochTime & 0xFF;
    records[numRecords].timestamp[1] = (epochTime >> 8) & 0xFF;
    records[numRecords].timestamp[2] = (epochTime >> 16) & 0xFF;
    records[numRecords].timestamp[3] = (epochTime >> 24) & 0xFF;
    
    records[numRecords].isIn = users[userIndex].lastState;
    numRecords++;
    saveRecordCount();
    saveRecord(numRecords - 1);
  }
  
  // Format time for display
  int hours = hour(currentTime);
  int minutes = minute(currentTime);
  char timeStr[6];
  sprintf(timeStr, "%02d:%02d", hours, minutes);
  
  // Display user info and punch status
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(users[userIndex].name);
  lcd.setCursor(0, 1);
  if (users[userIndex].lastState) {
    lcd.print("Entrada: ");
  } else {
    lcd.print("Saida: ");
  }
  lcd.print(timeStr);
  
  // Log to serial
  Serial.print("User: ");
  Serial.print(users[userIndex].name);
  Serial.print(" - ");
  Serial.print(users[userIndex].lastState ? "Punch IN" : "Punch OUT");
  Serial.print(" at ");
  Serial.println(timeStr);
  
  delay(1500); // Reduced delay from 3000ms to make the system ready for the next scan sooner
  
  updateIdleDisplay();
}

unsigned long getTimestampFromRecord(int recordIndex) {
  if (recordIndex < 0 || recordIndex >= numRecords) return 0;
  
  unsigned long result = 0;
  result |= (unsigned long)(records[recordIndex].timestamp[0] & 0xFF);
  result |= (unsigned long)(records[recordIndex].timestamp[1] & 0xFF) << 8;
  result |= (unsigned long)(records[recordIndex].timestamp[2] & 0xFF) << 16;
  result |= (unsigned long)(records[recordIndex].timestamp[3] & 0xFF) << 24;
  
  return result;
}

int findUserByUID(byte cardUID[4]) {
  for (int i = 0; i < numUsers; i++) {
    bool match = true;
    for (byte j = 0; j < 4; j++) {
      if (users[i].uid[j] != cardUID[j]) {
        match = false;
        break;
      }
    }
    if (match) {
      return i;
    }
  }
  return -1;
}

void loadUserData() {
  numUsers = EEPROM.read(USERS_COUNT_ADDR);
  if (numUsers > MAX_USERS) {
    numUsers = 0;  // Reset if corrupted
  }
  
  for (int i = 0; i < numUsers; i++) {
    int addr = USER_DATA_START + (i * sizeof(User));
    for (int j = 0; j < sizeof(User); j++) {
      *((byte*)&users[i] + j) = EEPROM.read(addr + j);
    }
  }
  
  Serial.print("Loaded ");
  Serial.print(numUsers);
  Serial.println(" users from EEPROM");
}

void saveUserData() {
  EEPROM.write(USERS_COUNT_ADDR, numUsers);
  
  for (int i = 0; i < numUsers; i++) {
    int addr = USER_DATA_START + (i * sizeof(User));
    for (int j = 0; j < sizeof(User); j++) {
      EEPROM.write(addr + j, *((byte*)&users[i] + j));
    }
  }
  
  EEPROM.commit();
  Serial.println("User data saved to EEPROM");
}

void loadRecordCount() {
  numRecords = EEPROM.read(RECORDS_COUNT_ADDR);
  if (numRecords > MAX_RECORDS) {
    numRecords = 0;  // Reset if corrupted
  }
  
  // Load records
  for (int i = 0; i < numRecords; i++) {
    loadRecord(i);
  }
  
  Serial.print("Loaded ");
  Serial.print(numRecords);
  Serial.println(" records from EEPROM");
}

void loadRecord(int index) {
  if (index < 0 || index >= MAX_RECORDS) return;
  
  int addr = RECORDS_DATA_START + (index * sizeof(Record));
  for (int j = 0; j < sizeof(Record); j++) {
    *((byte*)&records[index] + j) = EEPROM.read(addr + j);
  }
}

void saveRecord(int index) {
  if (index < 0 || index >= MAX_RECORDS) return;
  
  int addr = RECORDS_DATA_START + (index * sizeof(Record));
  for (int j = 0; j < sizeof(Record); j++) {
    EEPROM.write(addr + j, *((byte*)&records[index] + j));
  }
  
  EEPROM.commit();
}

void saveRecordCount() {
  EEPROM.write(RECORDS_COUNT_ADDR, numRecords);
  EEPROM.commit();
}

// Add after other EEPROM functions

void loadWiFiModeSetting() {
  byte value = EEPROM.read(WIFI_MODE_ADDR);
  stayConnectedToWiFi = (value == 1);
  Serial.print("Loaded WiFi mode setting: ");
  Serial.println(stayConnectedToWiFi ? "Stay Connected" : "AP Mode");
}

void saveWiFiModeSetting() {
  EEPROM.write(WIFI_MODE_ADDR, stayConnectedToWiFi ? 1 : 0);
  EEPROM.commit();
  Serial.print("Saved WiFi mode setting: ");
  Serial.println(stayConnectedToWiFi ? "Stay Connected" : "AP Mode");
}

void setupWiFi() {
  // First, load the WiFi mode setting
  loadWiFiModeSetting();
  
  if (stayConnectedToWiFi) {
    // Try to connect to WiFi networks in credentials list
    bool connected = false;
    
    WiFi.mode(WIFI_STA);
    WiFi.hostname(hostname);
    
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Conectando WiFi");
      lcd.setCursor(0, 1);
      lcd.print(availableNetworks[i].ssid);
      
      Serial.print("Trying to connect to: ");
      Serial.println(availableNetworks[i].ssid);
      
      WiFi.begin(availableNetworks[i].ssid, availableNetworks[i].password);
      
      // Wait for connection with timeout
      unsigned long startAttemptTime = millis();
      while (WiFi.status() != WL_CONNECTED && 
             millis() - startAttemptTime < WIFI_CONNECTION_TIMEOUT) {
        delay(500);
        Serial.print(".");
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        Serial.println();
        Serial.print("Connected to ");
        Serial.println(availableNetworks[i].ssid);
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Conectado a");
        lcd.setCursor(0, 1);
        lcd.print(availableNetworks[i].ssid);
        delay(1000);
        
        // Display IP address
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("IP:");
        lcd.setCursor(0, 1);
        lcd.print(WiFi.localIP().toString());
        delay(2000);
        
        break;
      } else {
        Serial.println(" Failed to connect.");
      }
    }
    
    if (!connected) {
      Serial.println("Couldn't connect to any network, switching to AP mode");
      // Fall back to AP mode
      stayConnectedToWiFi = false;
      saveWiFiModeSetting();
      setupWiFiAP();
    }
  } else {
    // Use AP mode
    setupWiFiAP();
  }
}

// New function to setup AP mode specifically
void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password); // Use ap_ssid and ap_password from credentials.h
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.hostname(hostname);
  
  Serial.println("Wi-Fi AP setup complete");
  Serial.print("SSID: ");
  Serial.println(ap_ssid);
  Serial.print("Password: ");
  Serial.println(ap_password);
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());
  
  isInAPMode = true;
}

void setupWebServer() {
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", HTTP_GET, handleLogout);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/users", HTTP_GET, handleUsers);
  server.on("/records", HTTP_GET, handleRecords);
  server.on("/register", HTTP_GET, handleRegisterForm);
  server.on("/register", HTTP_POST, handleRegisterPost);
  server.on("/export", HTTP_GET, handleExportOptions);
  server.on("/export/records", HTTP_GET, handleExportRecords);
  server.on("/export/users", HTTP_GET, handleExportUsers);
  server.on("/export/all", HTTP_GET, handleExportAll);
  server.on("/backlight", HTTP_GET, handleBacklightForm);
  server.on("/backlight", HTTP_POST, handleBacklightPost);
  server.on("/register/manual", HTTP_GET, handleManualRegisterForm);
  server.on("/register/manual", HTTP_POST, handleManualRegisterPost);
  server.on("/edit-user", HTTP_GET, handleEditUserForm);
  server.on("/edit-user", HTTP_POST, handleEditUserPost);
  server.on("/wifi-mode", HTTP_GET, handleWiFiModeForm);    // Add this line
  server.on("/wifi-mode", HTTP_POST, handleWiFiModePost);   // Add this line
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("HTTP server started");
}

void connectToWiFiForTimeUpdate() {
  // If we're already connected to WiFi and in stay-connected mode, just update time
  if (!isInAPMode && stayConnectedToWiFi && WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Atualizando");
    lcd.setCursor(0, 1);
    lcd.print("Hora NTP...");
    
    // Try to update time
    bool updated = false;
    for (int attempt = 0; attempt < 5; attempt++) {
      if (timeClient.forceUpdate()) {
        updated = true;
        ntpSynced = true;
        break;
      }
      delay(500);
    }
    
    if (updated) {
      // Display updated time
      time_t currentTime = timeClient.getEpochTime();
      int hours = hour(currentTime);
      int minutes = minute(currentTime);
      char timeStr[6];
      sprintf(timeStr, "%02d:%02d", hours, minutes);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hora Atual:");
      lcd.setCursor(0, 1);
      lcd.print(timeStr);
      
      Serial.print("Time updated: ");
      Serial.println(timeStr);
      
      delay(2000);
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Falha ao");
      lcd.setCursor(0, 1);
      lcd.print("atualizar hora!");
      Serial.println("Failed to update NTP time");
      delay(2000);
    }
    
    updateIdleDisplay();
    return;
  }

  // Original method for AP mode or when connection is lost in stay-connected mode
  String previousLcdLine1 = "Aproxime o";
  String previousLcdLine2 = "cartao...";
  
  // Show status on LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Atualizando");
  lcd.setCursor(0, 1);
  lcd.print("hora...");
  
  Serial.println("Attempting to connect to WiFi for time update");
  
  // Switch to station mode
  WiFi.mode(WIFI_STA);
  isInAPMode = false;
  WiFi.disconnect();
  
  bool connected = false;
  
  // Try all available networks
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    lcd.setCursor(0, 1);
    lcd.print("Conectando: ");
    lcd.print(i + 1);
    
    Serial.print("Trying to connect to: ");
    Serial.println(availableNetworks[i].ssid);
    
    WiFi.begin(availableNetworks[i].ssid, availableNetworks[i].password);
    
    // Wait for connection with timeout
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && 
           millis() - startAttemptTime < WIFI_CONNECTION_TIMEOUT) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.println();
      Serial.print("Connected to ");
      Serial.println(availableNetworks[i].ssid);
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Conectado a");
      lcd.setCursor(0, 1);
      lcd.print(availableNetworks[i].ssid);
      delay(1000);
      
      // Display IP address
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("IP:");
      lcd.setCursor(0, 1);
      lcd.print(WiFi.localIP().toString());
      delay(2000);
      
      break;
    } else {
      Serial.println(" Failed to connect.");
    }
  }
  
  if (connected) {
    // Update NTP time
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Atualizando");
    lcd.setCursor(0, 1);
    lcd.print("Hora NTP...");
    
    // Try multiple times to ensure an update
    bool updated = false;
    for (int attempt = 0; attempt < 5; attempt++) {
      if (timeClient.forceUpdate()) {
        updated = true;
        ntpSynced = true;
        break;
      }
      delay(500);
    }
    
    if (updated) {
      // Display updated time
      time_t currentTime = timeClient.getEpochTime();
      int hours = hour(currentTime);
      int minutes = minute(currentTime);
      char timeStr[6];
      sprintf(timeStr, "%02d:%02d", hours, minutes);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hora Atual:");
      lcd.setCursor(0, 1);
      lcd.print(timeStr);
      
      Serial.print("Time updated: ");
      Serial.println(timeStr);
      
      delay(2000);
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Falha ao");
      lcd.setCursor(0, 1);
      lcd.print("atualizar hora!");
      Serial.println("Failed to update NTP time");
      delay(2000);
    }
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Falha na");
    lcd.setCursor(0, 1);
    lcd.print("conexao WiFi");
    Serial.println("Failed to connect to any WiFi network");
    delay(2000);
  }
  
  // If we're supposed to stay connected but lost connection, try to reconnect later
  if (stayConnectedToWiFi) {
    // Don't switch to AP mode, leave in STA mode and it will try to reconnect later
    isInAPMode = false;
  } else {
    // Return to AP mode for normal operation
    setupWiFiAP(); 
  }
  
  // Setup web server again
  setupWebServer();
  
  // Restore previous LCD content
  updateIdleDisplay();
}

// Add this new function - best placed near connectToWiFiForTimeUpdate()
void syncTimeNTP() {
  // Only try to sync if we're connected to WiFi
  if (WiFi.status() == WL_CONNECTED) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sincronizando");
    lcd.setCursor(0, 1);
    lcd.print("Hora NTP...");
    
    // Try to update time
    bool updated = false;
    for (int attempt = 0; attempt < 5; attempt++) {
      if (timeClient.forceUpdate()) {
        updated = true;
        ntpSynced = true;
        break;
      }
      delay(500);
    }
    
    if (updated) {
      // Display updated time
      time_t currentTime = timeClient.getEpochTime();
      int hours = hour(currentTime);
      int minutes = minute(currentTime);
      char timeStr[6];
      sprintf(timeStr, "%02d:%02d", hours, minutes);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Hora Atual:");
      lcd.setCursor(0, 1);
      lcd.print(timeStr);
      
      Serial.print("Time synced: ");
      Serial.println(timeStr);
      
      delay(2000);
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Falha ao");
      lcd.setCursor(0, 1);
      lcd.print("sincronizar hora!");
      Serial.println("Failed to sync NTP time");
      delay(2000);
    }
    
    // Reset lastWifiConnectAttempt to maintain regular sync schedule
    lastWifiConnectAttempt = millis();
  }
}

void handleRoot() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Redbone Ponto</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "a { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                ".info { background-color: #f8f8f8; border: 1px solid #ddd; padding: 15px; margin: 20px 0; border-radius: 5px; }"
                ".info h2 { margin-top: 0; color: #333; font-size: 18px; }"
                ".progress { width: 100%; background-color: #e0e0e0; height: 20px; border-radius: 5px; margin: 10px 0; }"
                ".progress-bar { height: 100%; background-color: #4CAF50; border-radius: 5px; text-align: center; color: white; line-height: 20px; }"
                "</style></head><body>"
                "<h1>Redbone Ponto - Sistema de Controle</h1>"
                "<p>Sistema de registro de ponto via NFC</p>"
                "<a href='/users'>Ver Usuários</a><br>"
                "<a href='/records'>Ver Registros</a><br>"
                "<a href='/register'>Registrar Usuário (via Tag)</a><br>"
                "<a href='/register/manual'>Registrar Manualmente</a><br>"
                "<a href='/export'>Exportar Dados</a><br>"
                "<a href='/backlight'>Controle de Iluminação</a><br>"
                "<a href='/wifi-mode'>Configurar WiFi</a><br>"  // Add this line
                "<a href='/update' style='background-color: #ff9800;'>Atualizar Firmware (Web)</a><br>"
                "<a href='/logout' style='background-color: #f44336;'>Sair</a>"
                
                "<div class='info'>"
                "<h2>Status da Memória</h2>"
                "<p>Usuários: " + String(numUsers) + " de " + String(MAX_USERS) + " (" + String(MAX_USERS - numUsers) + " disponíveis)</p>"
                "<div class='progress'>"
                "<div class='progress-bar' style='width: " + String((numUsers * 100) / MAX_USERS) + "%;'>"
                + String((numUsers * 100) / MAX_USERS) + "%</div>"
                "</div>"
                "<p>Registros: " + String(numRecords) + " de " + String(MAX_RECORDS) + " (" + String(MAX_RECORDS - numRecords) + " disponíveis)</p>"
                "<div class='progress'>"
                "<div class='progress-bar' style='width: " + String((numRecords * 100) / MAX_RECORDS) + "%;'>"
                + String((numRecords * 100) / MAX_RECORDS) + "%</div>"
                "</div>"
                "</div>"
                "<p style='font-size:0.8em; color:#555;'>Para atualizar via IDE, procure pela porta de rede: " + String(hostname) + "</p>" // Note for IDE OTA
                
                "</body></html>";
                
  server.send(200, "text/html; charset=UTF-8", html);
}

// Manual registration form
void handleManualRegisterForm() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Registro Manual</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "form { max-width: 400px; margin: 0 auto; }"
                "label, input { display: block; margin: 10px 0; }"
                "input[type='text'] { width: 100%; padding: 8px; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; padding: 10px 15px; "
                "border: none; border-radius: 5px; cursor: pointer; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                ".help { font-size: 0.9em; color: #666; margin-bottom: 15px; }"
                "</style></head><body>"
                "<h1>Registro Manual de Usuário</h1>"
                
                "<form action='/register/manual' method='post'>"
                "<label for='uid'>UID da Tag (formato: 00:11:22:33):</label>"
                "<input type='text' id='uid' name='uid' required pattern='[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}'>"
                "<p class='help'>Digite o UID no formato hexadecimal, separado por dois pontos (xx:xx:xx:xx)</p>"
                
                "<label for='name'>Nome:</label>"
                "<input type='text' id='name' name='name' maxlength='16' required>"
                
                "<input type='submit' value='Registrar'>"
                "</form><br>"
                
                "<a class='back' href='/'>Voltar</a></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

// Process manual registration form submission
void handleManualRegisterPost() {
  if (!isLoggedIn) { // Added check here as well for POST actions
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  // Update activity time when user interacts
  lastActivityTime = millis();

  if (numUsers >= MAX_USERS) {
    server.send(400, "text/html; charset=UTF-8", "Memória cheia! Não é possível registrar mais usuários.");
    return;
  }

  String uidStr = server.arg("uid");
  String name = server.arg("name");
  
  if (name.length() == 0 || name.length() > NAME_LENGTH || uidStr.length() != 11) {
    server.send(400, "text/html; charset=UTF-8", "Dados inválidos. O UID deve ter o formato xx:xx:xx:xx e o nome deve ter entre 1 e 16 caracteres.");
    return;
  }
  
  // Parse the UID string
  byte cardUID[4];
  int pos = 0;
  for (int i = 0; i < 4; i++) {
    String byteStr = uidStr.substring(pos, pos + 2);
    cardUID[i] = strtol(byteStr.c_str(), NULL, 16);
    pos += 3;  // Skip over the 2 hex chars and the colon
  }
  
  // Check if this UID already exists
  int userIndex = findUserByUID(cardUID);
  if (userIndex != -1) {
    server.send(400, "text/html; charset=UTF-8", "Este UID já está registrado para o usuário: " + String(users[userIndex].name));
    return;
  }
  
  // Register the new user
  memcpy(users[numUsers].uid, cardUID, 4);
  name.toCharArray(users[numUsers].name, NAME_LENGTH + 1);
  users[numUsers].lastState = false;
  numUsers++;
  saveUserData();
  
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Registro Concluído</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                "h1 { color: #4CAF50; }"
                "a { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "</style></head><body>"
                "<h1>Usuário Registrado com Sucesso!</h1>"
                "<p>Nome: " + name + "</p>"
                "<p>UID: " + uidStr + "</p>"
                "<a href='/users'>Ver Usuários</a><br>"
                "<a href='/register/manual'>Registrar Outro</a><br>"
                "<a href='/'>Voltar</a></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
  
  Serial.print("User registered manually: ");
  Serial.print(name);
  Serial.print(" with UID: ");
  Serial.println(uidStr);
}

// Update the users list to include edit buttons
void handleUsers() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Usuários</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "table { border-collapse: collapse; width: 100%; }"
                "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }"
                "tr:nth-child(even) { background-color: #f2f2f2; }"
                "th { background-color: #4CAF50; color: white; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "a.edit { background-color: #2196F3; color: white; padding: 5px 10px; "
                "border-radius: 3px; text-decoration: none; font-size: 0.8em; margin-left: 5px; }"
                "</style></head><body>"
                "<h1>Lista de Usuários</h1>"
                "<table><tr><th>Nome</th><th>UID</th><th>Estado</th><th>Ações</th></tr>";
                
  for (int i = 0; i < numUsers; i++) {
    String uid = "";
    for (byte j = 0; j < 4; j++) {
      if (users[i].uid[j] < 0x10) uid += "0";
      uid += String(users[i].uid[j], HEX);
      if (j < 3) uid += ":";
    }
    
    html += "<tr><td>" + String(users[i].name) + "</td><td>" + uid + "</td>";
    html += "<td>" + String(users[i].lastState ? "Dentro" : "Fora") + "</td>";
    html += "<td><a href='/edit-user?index=" + String(i) + "' class='edit'>Editar</a></td></tr>";
  }
  
  html += "</table><br><a class='back' href='/'>Voltar</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

// Edit user form
void handleEditUserForm() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  if (!server.hasArg("index")) {
    server.send(400, "text/html; charset=UTF-8", "Parâmetro 'index' não encontrado.");
    return;
  }
  
  int index = server.arg("index").toInt();
  if (index < 0 || index >= numUsers) {
    server.send(400, "text/html; charset=UTF-8", "Índice de usuário inválido.");
    return;
  }
  
  String uid = "";
  for (byte j = 0; j < 4; j++) {
    if (users[index].uid[j] < 0x10) uid += "0";
    uid += String(users[index].uid[j], HEX);
    if (j < 3) uid += ":";
  }
  
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Editar Usuário</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "form { max-width: 400px; margin: 0 auto; }"
                "label, input { display: block; margin: 10px 0; }"
                "input[type='text'] { width: 100%; padding: 8px; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; padding: 10px 15px; "
                "border: none; border-radius: 5px; cursor: pointer; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                ".uid-display { background-color: #f8f8f8; padding: 8px; border: 1px solid #ddd; }"
                "</style></head><body>"
                "<h1>Editar Usuário</h1>"
                
                "<form action='/edit-user' method='post'>"
                "<input type='hidden' name='index' value='" + String(index) + "'>"
                
                "<label for='uid'>UID:</label>"
                "<div class='uid-display'>" + uid + "</div>"
                
                "<label for='name'>Nome:</label>"
                "<input type='text' id='name' name='name' maxlength='16' value='" + String(users[index].name) + "' required>"
                
                "<input type='submit' value='Salvar'>"
                "</form><br>"
                
                "<a class='back' href='/users'>Voltar</a></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

// Process user edit form submission
void handleEditUserPost() {
  if (!isLoggedIn) { // Added check here as well for POST actions
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  // Update activity time when user interacts
  lastActivityTime = millis();

  if (!server.hasArg("index") || !server.hasArg("name")) {
    server.send(400, "text/html; charset=UTF-8", "Parâmetros incompletos.");
    return;
  }
  
  int index = server.arg("index").toInt();
  String name = server.arg("name");
  
  if (index < 0 || index >= numUsers || name.length() == 0 || name.length() > NAME_LENGTH) {
    server.send(400, "text/html; charset=UTF-8", "Dados inválidos.");
    return;
  }
  
  // Update the user's name
  name.toCharArray(users[index].name, NAME_LENGTH + 1);
  saveUserData();
  
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Usuário Atualizado</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                "h1 { color: #4CAF50; }"
                "a { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "</style></head><body>"
                "<h1>Usuário Atualizado com Sucesso!</h1>"
                "<p>Nome atualizado para: " + name + "</p>"
                "<a href='/users'>Ver Usuários</a><br>"
                "<a href='/'>Voltar</a></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
  
  Serial.print("User updated: index ");
  Serial.print(index);
  Serial.print(", new name: ");
  Serial.println(name);
}

void handleRecords() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Registros</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "table { border-collapse: collapse; width: 100%; }"
                "th, td { border: 1px solid #ddd; padding: 8px; text-align: left; }"
                "tr:nth-child(even) { background-color: #f2f2f2; }"
                "th { background-color: #4CAF50; color: white; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "</style></head><body>"
                "<h1>Registros de Ponto</h1>"
                "<table><tr><th>Nome</th><th>Data/Hora</th><th>Tipo</th></tr>";

  for (int i = 0; i < numRecords; i++) {
    int userIndex = findUserByUID(records[i].uid);
    String userName = (userIndex != -1) ? users[userIndex].name : "Desconhecido";
    
    // Format timestamp usando a nova função
    time_t timestamp = getTimestampFromRecord(i);
    char dateTime[20];
    sprintf(dateTime, "%02d/%02d/%04d %02d:%02d",
            day(timestamp), month(timestamp), year(timestamp),
            hour(timestamp), minute(timestamp));
    
    html += "<tr><td>" + userName + "</td>";
    html += "<td>" + String(dateTime) + "</td>";
    html += "<td>" + String(records[i].isIn ? "Entrada" : "Saída") + "</td></tr>";
  }
  
  html += "</table><br><a class='back' href='/'>Voltar</a></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleRegisterForm() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Registrar Usuário</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "form { max-width: 400px; margin: 0 auto; }"
                "label, input { display: block; margin: 10px 0; }"
                "input[type='text'] { width: 100%; padding: 8px; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; padding: 10px 15px; "
                "border: none; border-radius: 5px; cursor: pointer; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "</style></head><body>"
                "<h1>Registrar Novo Usuário</h1>"
                "<p>Para registrar um novo usuário, aproxime o cartão não cadastrado do leitor e então preencha o formulário abaixo:</p>"
                "<form action='/register' method='post'>"
                "<label for='name'>Nome:</label>"
                "<input type='text' id='name' name='name' maxlength='16' required>"
                "<input type='submit' value='Registrar'>"
                "</form><br>"
                "<a class='back' href='/'>Voltar</a></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleRegisterPost() {
  if (!isLoggedIn) { // Added check here as well for POST actions
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  // Update activity time when user interacts
  lastActivityTime = millis();

  if (waitingForName && registrationIndex >= 0) {
    String name = server.arg("name");
    if (name.length() > 0 && name.length() <= NAME_LENGTH) {
      name.toCharArray(users[registrationIndex].name, NAME_LENGTH + 1);
      users[registrationIndex].lastState = false;      numUsers++;
      saveUserData();
      
      waitingForName = false;
      
      String html = "<!DOCTYPE html><html><head>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                    "<title>Registro Concluído</title>"
                    "<style>"
                    "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                    "h1 { color: #4CAF50; }"
                    "a { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                    "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                    "</style></head><body>"
                    "<h1>Usuário Registrado com Sucesso!</h1>"
                    "<p>Nome: " + name + "</p>"
                    "<a href='/users'>Ver Usuários</a><br>"
                    "<a href='/'>Voltar</a></body></html>";
      
      server.send(200, "text/html; charset=UTF-8", html);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Usuario salvo:");
      lcd.setCursor(0, 1);
      lcd.print(name);
      setBacklight(BACKLIGHT_DEFAULT); // Ensure backlight is bright for the message and timer reset
      delay(2000); // This is the delay at original line 1115
      
      updateIdleDisplay();
    } else {
      server.send(400, "text/html; charset=UTF-8", "Nome inválido. Deve ter entre 1 e 16 caracteres.");
    }
  } else {
    server.send(400, "text/html; charset=UTF-8", "Nenhum cartão aguardando registro. Escaneie um cartão primeiro.");
  }
}

void handleExportOptions() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Exportar Dados</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "a { display: block; margin: 10px 0; padding: 10px 15px; width: 200px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; text-align: center; }"
                ".container { max-width: 500px; margin: 0 auto; }"
                "</style></head><body>"
                "<div class='container'>"
                "<h1>Exportar Dados</h1>"
                "<p>Selecione o tipo de dados para exportar:</p>"
                "<a href='/export/users'>Exportar Usuários (CSV)</a>"
                "<a href='/export/records'>Exportar Registros (CSV)</a>"
                "<a href='/export/all'>Exportar Todos os Dados (CSV)</a>"
                "<a href='/' style='background-color: #555;'>Voltar</a>"
                "</div></body></html>";
                
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleExportRecords() {
  if (!isLoggedIn) {
    server.send(401, "text/plain", "Unauthorized"); // For file downloads, a 401 might be better than redirect
    return;
  }
  String csv = "UID,Nome,Data,Hora,Tipo\n";
  
  for (int i = 0; i < numRecords; i++) {
    int userIndex = findUserByUID(records[i].uid);
    String userName = (userIndex != -1) ? users[userIndex].name : "Desconhecido";
    
    // Add UID
    String uid = "";
    for (byte j = 0; j < 4; j++) {
      if (records[i].uid[j] < 0x10) uid += "0";
      uid += String(records[i].uid[j], HEX);
      if (j < 3) uid += ":";
    }
    csv += uid + ",";
    
    // Add name
    csv += userName + ",";
    
    // Add date and time
    time_t timestamp = getTimestampFromRecord(i);
    char date[11], time[6];
    sprintf(date, "%02d/%02d/%04d", day(timestamp), month(timestamp), year(timestamp));
    sprintf(time, "%02d:%02d", hour(timestamp), minute(timestamp));
    csv += String(date) + "," + String(time) + ",";
    
    // Add type
    csv += String(records[i].isIn ? "Entrada" : "Saida") + "\n";
  }
  
  server.sendHeader("Content-Disposition", "attachment; filename=redbone-ponto-registros.csv");
  server.send(200, "text/csv; charset=UTF-8", csv);
}

void handleExportUsers() {
  if (!isLoggedIn) {
    server.send(401, "text/plain", "Unauthorized"); // For file downloads
    return;
  }
  String csv = "UID,Nome,Estado\n";
  
  for (int i = 0; i < numUsers; i++) {
    // Add UID
    String uid = "";
    for (byte j = 0; j < 4; j++) {
      if (users[i].uid[j] < 0x10) uid += "0";
      uid += String(users[i].uid[j], HEX);
      if (j < 3) uid += ":";
    }
    csv += uid + ",";
    
    // Add name and state
    csv += String(users[i].name) + ",";
    csv += String(users[i].lastState ? "Dentro" : "Fora") + "\n";
  }
  
  server.sendHeader("Content-Disposition", "attachment; filename=redbone-ponto-usuarios.csv");
  server.send(200, "text/csv; charset=UTF-8", csv);
}

void handleExportAll() {
  if (!isLoggedIn) {
    server.send(401, "text/plain", "Unauthorized"); // For file downloads
    return;
  }
  // Create a JSON object that includes both users and records
  DynamicJsonDocument doc(20000); // Adjust size as needed
  
  JsonArray usersArray = doc.createNestedArray("users");
  for (int i = 0; i < numUsers; i++) {
    JsonObject user = usersArray.createNestedObject();
    
    // Add UID as string
    String uid = "";
    for (byte j = 0; j < 4; j++) {
      if (users[i].uid[j] < 0x10) uid += "0";
      uid += String(users[i].uid[j], HEX);
      if (j < 3) uid += ":";
    }
    user["uid"] = uid;
    
    user["name"] = users[i].name;
    user["state"] = users[i].lastState ? "Dentro" : "Fora";
  }
  
  JsonArray recordsArray = doc.createNestedArray("records");
  for (int i = 0; i < numRecords; i++) {
    JsonObject record = recordsArray.createNestedObject();
    
    // Add UID as string
    String uid = "";
    for (byte j = 0; j < 4; j++) {
      if (records[i].uid[j] < 0x10) uid += "0";
      uid += String(records[i].uid[j], HEX);
      if (j < 3) uid += ":";
    }
    record["uid"] = uid;
    
    int userIndex = findUserByUID(records[i].uid);
    record["userName"] = (userIndex != -1) ? users[userIndex].name : "Desconhecido";
    
    // Add formatted timestamp
    time_t timestamp = getTimestampFromRecord(i);
    char dateTime[20];
    sprintf(dateTime, "%02d/%02d/%04d %02d:%02d",
            day(timestamp), month(timestamp), year(timestamp),
            hour(timestamp), minute(timestamp));
    record["dateTime"] = dateTime;
    record["type"] = records[i].isIn ? "Entrada" : "Saida";
  }
  
  String jsonOutput;
  serializeJson(doc, jsonOutput);
  
  server.sendHeader("Content-Disposition", "attachment; filename=redbone-ponto-completo.json");
  server.send(200, "application/json; charset=UTF-8", jsonOutput);
}

void handleLogin() {
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Login - Redbone Ponto</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 0; padding: 0; display: flex; justify-content: center; align-items: center; height: 100vh; background-color: #f0f0f0; }"
                ".login-container { background-color: #fff; padding: 30px; border-radius: 8px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); width: 320px; text-align: center; }"
                "h1 { color: #333; margin-bottom: 25px; }"
                "label { display: block; margin-bottom: 8px; text-align: left; color: #555; font-weight: bold; }"
                "input[type='text'], input[type='password'] { width: calc(100% - 20px); padding: 10px; margin-bottom: 20px; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; padding: 12px 20px; border: none; border-radius: 4px; cursor: pointer; width: 100%; font-size: 16px; }"
                "input[type='submit']:hover { background-color: #45a049; }"
                ".error { color: red; font-size: 0.9em; margin-top: 15px; }"
                "</style></head><body>"
                "<div class='login-container'>"
                "<h1>Redbone Ponto</h1>"
                "<form action='/login' method='post'>"
                "<label for='username'>Usuário:</label>"
                "<input type='text' id='username' name='username' required>"
                "<label for='password'>Senha:</label>"
                "<input type='password' id='password' name='password' required>"
                "<input type='submit' value='Entrar'>";
  if (server.hasArg("error")) {
    html += "<p class='error'>Usuário ou senha inválidos!</p>";
  }
  html += "</form></div></body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleLoginPost() {
  if (server.hasArg("username") && server.hasArg("password")) {
    String username = server.arg("username");
    String password = server.arg("password");
    if (username.equals(adminUser) && password.equals(adminPass)) {
      isLoggedIn = true;
      server.sendHeader("Location", "/", true); // Redirect to root page
      server.send(302, "text/plain", "");
      return;
    }
  }
  // If login fails, redirect back to login page with an error query parameter
  server.sendHeader("Location", "/login?error=1", true);
  server.send(302, "text/plain", "");
}

void handleLogout() {
  isLoggedIn = false;
  server.sendHeader("Location", "/login", true); // Redirect to login page
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  server.send(404, "text/plain; charset=UTF-8", "Página não encontrada!");
}

void checkSerialCommands() {
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    if (inChar == '\n') {
      commandComplete = true;
      break;
    }
    serialCommand += inChar;
  }
  
  if (commandComplete) {
    processCommand(serialCommand);
    serialCommand = "";
    commandComplete = false;
  }
}

void processCommand(String command) {
  command.trim();

  // Activity detected via serial: reset backlight timer and turn on if dim.
  if (backlightDimmed) {
    setBacklight(BACKLIGHT_DEFAULT);
  } else {
    // Even if not dimmed, update lastActivityTime because a command is being processed.
    lastActivityTime = millis();
  }
  
  if (command.startsWith("list users")) {
    Serial.println("Registered Users:");
    Serial.println("----------------");
    for (int i = 0; i < numUsers; i++) {
      Serial.print(i + 1);
      Serial.print(". ");
      Serial.print(users[i].name);
      Serial.print(" (UID: ");
      for (byte j = 0; j < 4; j++) {
        if (users[i].uid[j] < 0x10) Serial.print("0");
        Serial.print(users[i].uid[j], HEX);
        if (j < 3) Serial.print(":");
      }
      Serial.print(") - Status: ");
      Serial.println(users[i].lastState ? "In" : "Out");
    }
    Serial.println("----------------");
  }
  else if (command.startsWith("list records")) {
    Serial.println("Punch Records:");
    Serial.println("----------------");
    for (int i = 0; i < numRecords; i++) {
      int userIndex = findUserByUID(records[i].uid);
      String userName = (userIndex != -1) ? users[userIndex].name : "Unknown";
      
      time_t timestamp = getTimestampFromRecord(i);
      char dateTime[20];
      sprintf(dateTime, "%02d/%02d/%04d %02d:%02d",
              day(timestamp), month(timestamp), year(timestamp),
              hour(timestamp), minute(timestamp));
      
      Serial.print(i + 1);
      Serial.print(". ");
      Serial.print(userName);
      Serial.print(" - ");
      Serial.print(dateTime);
      Serial.print(" - ");
      Serial.println(records[i].isIn ? "IN" : "OUT");
    }
    Serial.println("----------------");
  }
  else if (command.startsWith("delete user ")) {
    int index = command.substring(12).toInt() - 1;
    if (index >= 0 && index < numUsers) {
      // Shift all users down
      for (int i = index; i < numUsers - 1; i++) {
        memcpy(&users[i], &users[i + 1], sizeof(User));
      }
      numUsers--;
      saveUserData();
      Serial.println("User deleted successfully");
    } else {
      Serial.println("Invalid user index");
    }
  }
  else if (command.startsWith("clear records")) {
    numRecords = 0;
    saveRecordCount();
    Serial.println("All records cleared");
  }
  else if (command.startsWith("help")) {
    Serial.println("Available commands:");
    Serial.println("list users - Display all registered users");
    Serial.println("list records - Display all punch records");
    Serial.println("delete user [index] - Delete a user by number");
    Serial.println("clear records - Delete all punch records");
    Serial.println("backlight [0-255] - Set LCD backlight brightness");
    Serial.println("help - Display this help text");
  }
  else if (command.startsWith("backlight ")) {
    int level = command.substring(10).toInt();
    if (level >= 0 && level <= 255) {
      setBacklight(level);
      Serial.print("Backlight set to: ");
      Serial.println(level);
    } else {
      Serial.println("Invalid backlight level. Use 0-255.");
    }
  }
  else if (waitingForName) {
    // If waiting for a name for registration, use this input
    if (command.length() > 0 && command.length() <= NAME_LENGTH) {
      command.toCharArray(users[registrationIndex].name, NAME_LENGTH + 1);
      users[registrationIndex].lastState = false; // Start with "out" state
      numUsers++;
      saveUserData();
      
      waitingForName = false;
      
      Serial.print("User registered: ");
      Serial.println(command);
      
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Usuario salvo:");
      lcd.setCursor(0, 1);
      lcd.print(command);
      setBacklight(BACKLIGHT_DEFAULT); // Ensure backlight is bright for the message and timer reset
      delay(2000);
      
      updateIdleDisplay();
    } else {
      Serial.println("Invalid name. Must be between 1 and 16 characters.");
    }
  }
  else {
    Serial.println("Unknown command. Type 'help' for available commands.");
  }
}

void setBacklight(byte level) {
  backlightLevel = level;
  analogWrite(LCD_BACKLIGHT_PIN, backlightLevel);
  lastActivityTime = millis();
  backlightDimmed = (backlightLevel <= BACKLIGHT_LOW);
}

void manageBacklight() {
  unsigned long currentTime = millis();
  
  // Only check for timeout to dim the display
  if (!backlightDimmed && (currentTime - lastActivityTime > BACKLIGHT_TIMEOUT)) {
    setBacklight(BACKLIGHT_LOW);
  }
}

void handleBacklightForm() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Controle de Backlight</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                "form { max-width: 400px; margin: 0 auto; }"
                "label { display: block; margin: 10px 0; }"
                "input[type='range'] { width: 100%; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; "
                "padding: 10px 15px; border: none; border-radius: 5px; cursor: pointer; margin-top: 10px; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                "</style></head><body>"
                "<h1>Controle de Iluminação LCD</h1>"
                "<form action='/backlight' method='post'>"
                "<label for='level'>Brilho (0-255): <span id='brightValue'>" + String(backlightLevel) + "</span></label>"
                "<input type='range' id='level' name='level' min='0' max='255' value='" + String(backlightLevel) + "' "
                "oninput='document.getElementById(\"brightValue\").innerText = this.value'>"
                "<input type='submit' value='Aplicar'>"
                "</form><br>"
                "<a class='back' href='/'>Voltar</a>"
                "</body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleBacklightPost() {
  if (!isLoggedIn) { // Added check here as well for POST actions
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  // Update activity time when user interacts
  lastActivityTime = millis();

  if (server.hasArg("level")) {
    int level = server.arg("level").toInt();
    if (level >= 0 && level <= 255) {
      setBacklight(level);
      
      String html = "<!DOCTYPE html><html><head>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                    "<title>Backlight Atualizado</title>"
                    "<style>"
                    "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                    "h1 { color: #4CAF50; }"
                    "a { display: inline-block; margin: 10px 10px; padding: 10px 15px; "
                    "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                    "</style></head><body>"
                    "<h1>Brilho Atualizado!</h1>"
                    "<p>Nível: " + String(level) + "</p>"
                    "<a href='/backlight'>Ajustar Novamente</a>"
                    "<a href='/'>Voltar</a></body></html>";
      
      server.send(200, "text/html; charset=UTF-8", html);
    } else {
      server.send(400, "text/html; charset=UTF-8", "Nível de brilho inválido. Use 0-255.");
    }
  } else {
    server.send(400, "text/html; charset=UTF-8", "Parâmetro 'level' não encontrado.");
  }
}

void handleWiFiModeForm() {
  if (!isLoggedIn) {
    server.sendHeader("Location", "/login", true);
    server.send(302, "text/plain", "");
    return;
  }
  
  String currentMode = stayConnectedToWiFi ? "Modo Conectado (STA)" : "Modo Ponto de Acesso (AP)";
  String html = "<!DOCTYPE html><html><head>"
                "<meta charset='UTF-8'>"
                "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<title>Configuração WiFi</title>"
                "<style>"
                "body { font-family: Arial, sans-serif; margin: 20px; }"
                "h1 { color: #333; }"
                ".container { max-width: 600px; margin: 0 auto; }"
                ".card { background-color: #f8f8f8; border: 1px solid #ddd; padding: 20px; margin: 20px 0; border-radius: 5px; }"
                ".current { color: #4CAF50; font-weight: bold; }"
                ".info { color: #555; font-size: 0.9em; margin: 15px 0; }"
                "label { display: block; margin: 15px 0 5px; }"
                "select { width: 100%; padding: 8px; border-radius: 4px; border: 1px solid #ccc; }"
                "input[type='submit'] { background-color: #4CAF50; color: white; padding: 10px 15px; "
                "border: none; border-radius: 5px; cursor: pointer; margin-top: 20px; }"
                "a.back { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                "background-color: #555; color: white; text-decoration: none; border-radius: 5px; }"
                ".warning { color: #f44336; }"
                "</style></head><body>"
                "<div class='container'>"
                "<h1>Configuração de Modo WiFi</h1>"
                
                "<div class='card'>"
                "<h2>Modo WiFi Atual: <span class='current'>" + currentMode + "</span></h2>"
                
                "<p class='info'>O dispositivo pode operar em dois modos diferentes:</p>"
                "<ul>"
                "<li><strong>Modo Ponto de Acesso (AP)</strong> - O dispositivo cria sua própria rede WiFi. "
                "Usa menos energia, mas precisa se conectar temporariamente a outra WiFi para atualizar a hora.</li>"
                "<li><strong>Modo Conectado (STA)</strong> - O dispositivo se mantém conectado a uma rede WiFi existente. "
                "Permite atualizações de hora em tempo real, mas consome mais energia.</li>"
                "</ul>"
                
                "<form action='/wifi-mode' method='post'>"
                "<label for='mode'>Selecione o modo WiFi:</label>"
                "<select id='mode' name='mode'>"
                "<option value='ap' " + String(!stayConnectedToWiFi ? "selected" : "") + ">Modo Ponto de Acesso (AP)</option>"
                "<option value='sta' " + String(stayConnectedToWiFi ? "selected" : "") + ">Modo Conectado (STA)</option>"
                "</select>"
                
                "<p class='warning'><strong>Atenção:</strong> Após mudar o modo WiFi, o dispositivo será reiniciado "
                "e você precisará reconectar-se à rede apropriada.</p>"
                
                "<input type='submit' value='Salvar configuração'>"
                "</form>"
                "</div>"
                
                "<a href='/' class='back'>Voltar</a>"
                "</div></body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

void handleWiFiModePost() {
  if (!isLoggedIn) {
    server.send(401, "text/plain", "Unauthorized");
    return;
  }

  // Update activity time when user interacts
  lastActivityTime = millis();

  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    bool newSetting = (mode == "sta");
    
    // Check if the setting has actually changed
    if (newSetting != stayConnectedToWiFi) {
      // Update the setting
      stayConnectedToWiFi = newSetting;
      saveWiFiModeSetting();
      
      String html = "<!DOCTYPE html><html><head>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                    "<title>Configuração Atualizada</title>"
                    "<style>"
                    "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                    "h1 { color: #4CAF50; }"
                    "p { margin: 20px 0; }"
                    ".container { max-width: 600px; margin: 0 auto; }"
                    ".message { background-color: #f8f8f8; border: 1px solid #ddd; padding: 20px; margin: 20px 0; border-radius: 5px; }"
                    ".restarting { color: #ff9800; font-weight: bold; }"
                    "</style>"
                    "<meta http-equiv='refresh' content='5;url=/'>"
                    "</head><body>"
                    "<div class='container'>"
                    "<h1>Configuração WiFi Atualizada</h1>"
                    
                    "<div class='message'>"
                    "<p>O modo WiFi foi alterado para: <strong>" + 
                    String(stayConnectedToWiFi ? "Modo Conectado (STA)" : "Modo Ponto de Acesso (AP)") + 
                    "</strong></p>"
                    
                    "<p class='restarting'>O dispositivo está aplicando as configurações...</p>"
                    "<p>Você será redirecionado automaticamente em 5 segundos.</p>"
                    "</div>"
                    "</div></body></html>";
      
      server.send(200, "text/html; charset=UTF-8", html);
      
      // After sending the response, reconfigure WiFi
      delay(1000); // Give the browser time to receive the response
      
      // Reset and reconfigure WiFi with new mode
      setupWiFi();
      
    } else {
      // No changes were made
      String html = "<!DOCTYPE html><html><head>"
                    "<meta charset='UTF-8'>"
                    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                    "<title>Nenhuma Alteração</title>"
                    "<style>"
                    "body { font-family: Arial, sans-serif; margin: 20px; text-align: center; }"
                    "h1 { color: #555; }"
                    "a { display: inline-block; margin: 10px 0; padding: 10px 15px; "
                    "background-color: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }"
                    "</style></head><body>"
                    "<h1>Nenhuma Alteração Feita</h1>"
                    "<p>O modo WiFi continua: " + 
                    String(stayConnectedToWiFi ? "Modo Conectado (STA)" : "Modo Ponto de Acesso (AP)") + 
                    "</p>"
                    "<a href='/wifi-mode'>Voltar à Configuração</a><br>"
                    "<a href='/'>Voltar à Página Inicial</a>"
                    "</body></html>";
      
      server.send(200, "text/html; charset=UTF-8", html);
    }
  } else {
    server.send(400, "text/html; charset=UTF-8", "Parâmetro 'mode' não encontrado.");
  }
}
