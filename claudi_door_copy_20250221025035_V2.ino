#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

// Pin definitions and constants
#define RELAY_PIN D1          // Pin controlling the door relay
#define RESET_PIN D4          // Pin for the reset button
#define MAX_TOKEN_LENGTH 50   // Maximum length for the Telegram bot token
#define MAX_USER_IDS 10       // Maximum number of authorized users
#define MAX_GROUP_IDS 5       // Maximum number of authorized groups

// Timing constants (in milliseconds)
const uint16_t RELAY_ACTIVATION_TIME = 800;      // How long to activate the relay
const uint16_t RESET_BUTTON_HOLD_TIME = 3000;    // How long to hold reset button
const uint8_t WIFI_CONNECT_TIMEOUT = 20;         // Seconds to wait for WiFi
const uint16_t BOT_CHECK_INTERVAL = 1000;        // How often to check Telegram
unsigned long startTime = 0;  // Will store the boot time
unsigned int wifiReconnects = 0;  // Counter for WiFi reconnections

// Global objects
ESP8266WebServer server(80);
WiFiClientSecure client;
UniversalTelegramBot* bot = nullptr;

// Authentication data structure
struct AuthData {
    char userIds[MAX_USER_IDS][16];    // Array of authorized user IDs
    char groupIds[MAX_GROUP_IDS][16];   // Array of authorized group IDs
    uint8_t numUsers;                   // Current number of authorized users
    uint8_t numGroups;                  // Current number of authorized groups
};

// Configuration structure
struct Config {
    char wifi_ssid[32];
    char wifi_password[64];
    char bot_token[MAX_TOKEN_LENGTH];
    AuthData auth_data;
};

// Class to manage configuration
class ConfigManager {
private:
    Config config;
    const char* config_file = "/config.json";

public:
    bool saveConfig() {
        File file = LittleFS.open(config_file, "w");
        if (!file) return false;
        
        StaticJsonDocument<1024> doc;
        doc["wifi_ssid"] = config.wifi_ssid;
        doc["wifi_password"] = config.wifi_password;
        doc["bot_token"] = config.bot_token;
        
        JsonArray users = doc.createNestedArray("users");
        for (uint8_t i = 0; i < config.auth_data.numUsers; i++) {
            users.add(config.auth_data.userIds[i]);
        }
        
        JsonArray groups = doc.createNestedArray("groups");
        for (uint8_t i = 0; i < config.auth_data.numGroups; i++) {
            groups.add(config.auth_data.groupIds[i]);
        }
        
        serializeJson(doc, file);
        file.close();
        return true;
    }

    bool loadConfig() {
        File file = LittleFS.open(config_file, "r");
        if (!file) return false;
        
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        
        if (error) return false;
        
        strlcpy(config.wifi_ssid, doc["wifi_ssid"] | "", sizeof(config.wifi_ssid));
        strlcpy(config.wifi_password, doc["wifi_password"] | "", sizeof(config.wifi_password));
        strlcpy(config.bot_token, doc["bot_token"] | "", sizeof(config.bot_token));
        
        config.auth_data.numUsers = 0;
        JsonArray users = doc["users"];
        for (JsonVariant user : users) {
            if (config.auth_data.numUsers < MAX_USER_IDS) {
                strlcpy(config.auth_data.userIds[config.auth_data.numUsers++], 
                        user.as<const char*>(), 16);
            }
        }
        
        config.auth_data.numGroups = 0;
        JsonArray groups = doc["groups"];
        for (JsonVariant group : groups) {
            if (config.auth_data.numGroups < MAX_GROUP_IDS) {
                strlcpy(config.auth_data.groupIds[config.auth_data.numGroups++], 
                        group.as<const char*>(), 16);
            }
        }
        
        return true;
    }

    Config& getConfig() { return config; }
};

// Door controller class
class DoorController {
private:
    uint8_t relay_pin;
    unsigned long last_activation = 0;
    const unsigned long MIN_ACTIVATION_INTERVAL = 2000;

public:
    DoorController(uint8_t pin) : relay_pin(pin) {
        pinMode(relay_pin, OUTPUT);
        digitalWrite(relay_pin, LOW);
    }

    bool activateDoor() {
        unsigned long current_time = millis();
        if (current_time - last_activation < MIN_ACTIVATION_INTERVAL) {
            return false;
        }
        
        digitalWrite(relay_pin, HIGH);
        delay(RELAY_ACTIVATION_TIME);
        digitalWrite(relay_pin, LOW);
        last_activation = current_time;
        return true;
    }
};

// Global objects
ConfigManager configManager;
DoorController doorController(RELAY_PIN);

// Function to connect to WiFi
bool connectToWiFi() {
    Config& config = configManager.getConfig();
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < WIFI_CONNECT_TIMEOUT) {
        delay(1000);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    } else {
        return false;
    }
}

// Function to set up Access Point mode
void setupAccessPoint() {
    WiFi.mode(WIFI_AP);
    IPAddress local_ip(192,168,9,9);
    IPAddress gateway(192,168,9,1);
    IPAddress subnet(255,255,255,0);
    WiFi.softAPConfig(local_ip, gateway, subnet);
    WiFi.softAP("set_me_up");
    Serial.println("Setting up Access Point");
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
}

// Function to check the filesystem contents
void listFiles() {
    Dir dir = LittleFS.openDir("/");
    Serial.println("Files in filesystem:");
    while (dir.next()) {
        Serial.print("  - ");
        Serial.println(dir.fileName());
    }
}

// Function to set up OTA updates
void setupOTA() {
    ArduinoOTA.onStart([]() {
        Serial.println("Starting OTA update");
    });
    
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA update complete");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    
    ArduinoOTA.begin();
}

// Function to set up the Telegram bot
void setupBot() {
    Config& config = configManager.getConfig();
    
    // Check if the bot token is set
    if (strlen(config.bot_token) == 0) {
        Serial.println("Bot token not configured, skipping bot setup");
        return;
    }
    
    // Delete previous bot instance if it exists
    if (bot != nullptr) {
        delete bot;
        bot = nullptr;
    }
    
    client.setInsecure();  // Required for ESP8266 to connect to Telegram
    bot = new UniversalTelegramBot(config.bot_token, client);
    
    // Send a startup message to all authorized users
    if (bot != nullptr) {
        Serial.println("Bot initialized successfully");
        
        // Send startup notification to all authorized users
        Config& config = configManager.getConfig();
        String startupMessage = "🟢 Door Controller is now online!";
        
        for (uint8_t i = 0; i < config.auth_data.numUsers; i++) {
            bot->sendMessage(config.auth_data.userIds[i], startupMessage, "");
        }
    } else {
        Serial.println("Failed to initialize bot");
    }
}

// Function to set up the web server
// Add these API endpoint handlers to your setupWebServer() function:

void setupWebServer() {
    // Existing code for serving index.html
    server.on("/", HTTP_GET, []() {
        Serial.println("Received request for index page");
        if (!LittleFS.exists("/index.html")) {
            Serial.println("index.html not found!");
            server.send(404, "text/plain", "index.html not found!");
            return;
        }
        File file = LittleFS.open("/index.html", "r");
        if (!file) {
            Serial.println("Failed to open index.html");
            server.send(500, "text/plain", "Failed to open file");
            return;
        }
        Serial.println("Serving index.html");
        server.streamFile(file, "text/html");
        file.close();
    });
    
    // API endpoint to get configuration
    server.on("/config", HTTP_GET, []() {
        Config& config = configManager.getConfig();
        
        StaticJsonDocument<1024> doc;
        doc["wifi_ssid"] = config.wifi_ssid;
        doc["wifi_password"] = config.wifi_password;
        doc["bot_token"] = config.bot_token;
        
        JsonArray users = doc.createNestedArray("users");
        for (uint8_t i = 0; i < config.auth_data.numUsers; i++) {
            users.add(config.auth_data.userIds[i]);
        }
        
        JsonArray groups = doc.createNestedArray("groups");
        for (uint8_t i = 0; i < config.auth_data.numGroups; i++) {
            groups.add(config.auth_data.groupIds[i]);
        }
        
        String jsonResponse;
        serializeJson(doc, jsonResponse);
        server.send(200, "application/json", jsonResponse);
    });
    
    // API endpoint to save configuration
    server.on("/config", HTTP_POST, []() {
        String jsonBody = server.arg("plain");
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, jsonBody);
        
        if (error) {
            server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
            return;
        }
        
        Config& config = configManager.getConfig();
        
        strlcpy(config.wifi_ssid, doc["wifi_ssid"] | "", sizeof(config.wifi_ssid));
        strlcpy(config.wifi_password, doc["wifi_password"] | "", sizeof(config.wifi_password));
        strlcpy(config.bot_token, doc["bot_token"] | "", sizeof(config.bot_token));
        
        config.auth_data.numUsers = 0;
        JsonArray users = doc["users"];
        for (JsonVariant user : users) {
            if (config.auth_data.numUsers < MAX_USER_IDS) {
                strlcpy(config.auth_data.userIds[config.auth_data.numUsers++], 
                        user.as<const char*>(), 16);
            }
        }
        
        config.auth_data.numGroups = 0;
        JsonArray groups = doc["groups"];
        for (JsonVariant group : groups) {
            if (config.auth_data.numGroups < MAX_GROUP_IDS) {
                strlcpy(config.auth_data.groupIds[config.auth_data.numGroups++], 
                        group.as<const char*>(), 16);
            }
        }
        
        if (configManager.saveConfig()) {
            server.send(200, "application/json", "{\"status\":\"success\"}");
        } else {
            server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to save configuration\"}");
        }
    });
    
    // API endpoint to restart the device
    server.on("/restart", HTTP_GET, []() {
        server.send(200, "application/json", "{\"status\":\"restarting\"}");
        delay(1000);
        ESP.restart();
    });
    
    server.begin();
    Serial.println("HTTP server started");
}

// Function to handle the reset button
void handleResetButton() {
    static unsigned long resetPressStart = 0;
    static bool resetPressed = false;
    
    if (digitalRead(RESET_PIN) == LOW) {  // Button pressed
        if (!resetPressed) {
            resetPressed = true;
            resetPressStart = millis();
        } else if (millis() - resetPressStart >= RESET_BUTTON_HOLD_TIME) {
            // Reset configuration
            LittleFS.remove("/config.json");
            Serial.println("Configuration reset. Restarting...");
            ESP.restart();
        }
    } else {
        resetPressed = false;
    }
}

// Function to check and handle Telegram messages
void checkTelegramMessages() {
    if (!bot) {
        Serial.println("Bot not initialized, skipping message check");
        return;
    }
    
    Serial.println("Checking for new Telegram messages...");
    int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
    Serial.printf("Found %d new messages\n", numNewMessages);
    
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = bot->messages[i].chat_id;
        String text = bot->messages[i].text;
        String from_name = bot->messages[i].from_name;
        
        Serial.printf("Message from %s (chat ID: %s): %s\n", 
                     from_name.c_str(), chat_id.c_str(), text.c_str());
        
        // Check if user is authorized
        bool isAuthorized = false;
        Config& config = configManager.getConfig();
        
        // Check user IDs
        for (uint8_t j = 0; j < config.auth_data.numUsers; j++) {
            if (chat_id == config.auth_data.userIds[j]) {
                isAuthorized = true;
                break;
            }
        }
        
        // Check group IDs
        if (!isAuthorized) {
            for (uint8_t j = 0; j < config.auth_data.numGroups; j++) {
                if (chat_id == config.auth_data.groupIds[j]) {
                    isAuthorized = true;
                    break;
                }
            }
        }
        
        // Handle /start command (works for everyone)
        if (text == "/start") {
            String welcomeMessage = "Welcome to the Door Controller Bot!\n\n";
            welcomeMessage += "This bot allows authorized users to remotely control a door.\n\n";
            welcomeMessage += "Available commands:\n";
            welcomeMessage += "/open - Activate the door opener\n";
            welcomeMessage += "/status - Show system status information\n\n";
            
            if (!isAuthorized) {
                welcomeMessage += "⚠️ You are not authorized to use this bot. Please contact the administrator.";
            }
            
            bot->sendMessage(chat_id, welcomeMessage, "");
            continue;
        }
        
        if (!isAuthorized) {
            String unauthorizedMessage = "⛔ Access Denied\n\n";
            unauthorizedMessage += "You are not authorized to control this door.\n";
            unauthorizedMessage += "Your chat ID: " + chat_id + "\n\n";
            unauthorizedMessage += "If you should have access, please contact the administrator.";
            
            bot->sendMessage(chat_id, unauthorizedMessage, "");
            
            // Log unauthorized access attempt
            Serial.printf("Unauthorized access attempt from %s (chat ID: %s)\n", 
                         from_name.c_str(), chat_id.c_str());
            continue;
        }
        
        // Handle authorized commands
        if (text == "/open") {
            if (doorController.activateDoor()) {
                bot->sendMessage(chat_id, "🔓 Door activated", "");
                Serial.printf("Door activated by %s (chat ID: %s)\n", 
                             from_name.c_str(), chat_id.c_str());
            } else {
                bot->sendMessage(chat_id, "⏳ Please wait before trying again", "");
            }
        }
        else if (text == "/status") {
            String statusMessage = "📊 System Status:\n\n";
            
            // WiFi signal strength
            long rssi = WiFi.RSSI();
            String signalQuality;
            if (rssi > -50) signalQuality = "Excellent (📶📶📶📶)";
            else if (rssi > -60) signalQuality = "Very Good (📶📶📶)";
            else if (rssi > -70) signalQuality = "Good (📶📶)";
            else if (rssi > -80) signalQuality = "Fair (📶)";
            else signalQuality = "Poor (⚠️)";
            
            statusMessage += "📡 WiFi Signal: " + String(rssi) + " dBm (" + signalQuality + ")\n";
            
            // Uptime calculation
            unsigned long uptime = millis() / 1000; // convert to seconds
            int days = uptime / 86400;
            uptime %= 86400;
            int hours = uptime / 3600;
            uptime %= 3600;
            int minutes = uptime / 60;
            int seconds = uptime % 60;
            
            statusMessage += "⏱️ Uptime: ";
            if (days > 0) statusMessage += String(days) + " days, ";
            statusMessage += String(hours) + "h " + String(minutes) + "m " + String(seconds) + "s\n";
            
            // WiFi reconnects
            statusMessage += "🔄 WiFi reconnects: " + String(wifiReconnects) + "\n";
            
            // IP Address
            statusMessage += "🌐 IP Address: " + WiFi.localIP().toString() + "\n";
            
            // Memory info
            statusMessage += "💾 Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
            
            bot->sendMessage(chat_id, statusMessage, "");
        } else {
            // Handle unknown commands from authorized users
            String helpMessage = "Unknown command. Available commands:\n";
            helpMessage += "/open - Activate the door\n";
            helpMessage += "/status - Show system status\n";
            helpMessage += "/start - Show welcome message";
            
            bot->sendMessage(chat_id, helpMessage, "");
        }
    }
}

// Debug function to help diagnose issues
void debugTelegramBot() {
    // Only run this periodically (e.g., every minute)
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime < 60000) return; // Check once per minute
    
    lastDebugTime = millis();
    
    if (!bot) {
        Serial.println("DEBUG: Bot not initialized");
        return;
    }
    
    Config& config = configManager.getConfig();
    Serial.println("DEBUG: Bot token: " + String(config.bot_token));
    Serial.println("DEBUG: Authorized users: " + String(config.auth_data.numUsers));
    Serial.println("DEBUG: Authorized groups: " + String(config.auth_data.numGroups));
    
    // Try a getMe call to test the bot's connection to Telegram
    bot->getMe();
    //Serial.println("DEBUG: Bot username: " + bot->telegramUserName);
}
// Function to monitor WiFi and count reconnects
void monitorWiFi() {
    static bool wasConnected = false;
    bool isConnected = (WiFi.status() == WL_CONNECTED);
    
    if (!wasConnected && isConnected) {
        // WiFi just connected
        wasConnected = true;
    } else if (wasConnected && !isConnected) {
        // WiFi disconnected
        wasConnected = false;
        // Try to reconnect
        if (connectToWiFi()) {
            wifiReconnects++;
        }
    }
}

void setup() {
    Serial.begin(115200);
    startTime = millis();  // Record the start time
    Serial.println("\nStarting up...");
    pinMode(RESET_PIN, INPUT_PULLUP);
    
    if (!LittleFS.begin()) {
        Serial.println("Failed to mount file system");
        setupAccessPoint();
        return;
    }
    Serial.println("Filesystem mounted successfully");
    
    listFiles();  // Add this to list all files
    
    if (!configManager.loadConfig()) {
        Serial.println("Failed to load configuration");
        Serial.println("Switching to setup mode...");
        setupAccessPoint();
        setupWebServer();  // Make sure we call this!
        return;
    }
    
    setupOTA();
    
    if (!connectToWiFi()) {
        Serial.println("Failed to connect to WiFi");
        setupAccessPoint();
        setupWebServer();  // Make sure we call this!
        return;
    }
    
    setupBot();
    setupWebServer();
    
    Serial.println("Setup completed successfully");
}

void loop() {
    static unsigned long last_bot_check = 0;
    unsigned long current_time = millis();
    
    ArduinoOTA.handle();
    server.handleClient();
    handleResetButton();
    monitorWiFi();
    
    if (current_time - last_bot_check >= BOT_CHECK_INTERVAL) {
        checkTelegramMessages();
        last_bot_check = current_time;
    }
    
    // Add debug function call
    debugTelegramBot();
    
    ESP.wdtFeed();
    yield();
}