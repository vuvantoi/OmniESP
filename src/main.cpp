#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Wire.h> // Vital pour I2C
#include "OmniDrivers.h"

// --- VARIABLES GLOBALES ---
std::vector<Device*> devices;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SemaphoreHandle_t mutex;

// Structure Automation inchangée
struct Rule { String srcId; String param; String op; float threshold; String tgtId; float actionVal; };
std::vector<Rule> rules;

// --- I2C SCANNER TOOL (Nouvelle fonction V2) ---
String scanI2C() {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("i2c_devices");
    
    // Scan standard 0x01 à 0x7F
    for(byte address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        byte error = Wire.endTransmission();
        if (error == 0) {
            JsonObject obj = arr.createNestedObject();
            obj["addr_dec"] = address;
            char hexStr[5]; sprintf(hexStr, "0x%02X", address);
            obj["addr_hex"] = String(hexStr);
            
            // Deviner le device
            if(address == 0x27) obj["hint"] = "LCD 1602";
            else if(address == 0x3C) obj["hint"] = "OLED SSD1306";
            else if(address == 0x40) obj["hint"] = "INA219 Power";
            else if(address == 0x76) obj["hint"] = "BME280/BMP280";
            else if(address == 0x23) obj["hint"] = "BH1750 Light";
            else obj["hint"] = "Unknown";
        }
    }
    String output; serializeJson(doc, output);
    return output;
}

// --- VALIDATION & UTILITAIRES ---
bool isI2CDriver(String type) {
    return (type == "INA219" || type == "BME280" || type == "BH1750" || type == "LCD_I2C" || type == "OLED");
}

bool isPinValid(int pin, String type) {
    if(isI2CDriver(type)) return (pin >= 1 && pin <= 127); // Validation Adresse I2C
    // Validation GPIO standard
    if (pin < 0 || pin > 39) return false;
    if (pin == 1 || pin == 3 || (pin >= 6 && pin <= 11)) return false; 
    return true;
}

// Nettoyage Mémoire
void clearDevices() {
    for(auto d : devices) delete d;
    devices.clear();
}

// --- CONFIGURATION SAVE/LOAD ---
void saveConfig() {
    File f = LittleFS.open("/config.json", "w");
    DynamicJsonDocument doc(8192);
    JsonArray devArr = doc.createNestedArray("devices");
    
    xSemaphoreTake(mutex, portMAX_DELAY);
    for(auto d : devices) {
        JsonObject obj = devArr.createNestedObject();
        obj["id"] = d->getId();
        obj["driver"] = d->getDriver(); 
        obj["name"] = d->getName(); 
        obj["pin"] = d->getPin();
    }
    xSemaphoreGive(mutex);

    JsonArray ruleArr = doc.createNestedArray("rules");
    for(auto r : rules) {
        JsonObject obj = ruleArr.createNestedObject();
        obj["src"] = r.srcId; obj["prm"] = r.param; 
        obj["op"] = r.op; obj["val"] = r.threshold;
        obj["tgt"] = r.tgtId; obj["act"] = r.actionVal;
    }
    serializeJson(doc, f); f.close();
}

void loadConfig() {
    if(!LittleFS.exists("/config.json")) return;
    File f = LittleFS.open("/config.json", "r");
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, f); f.close();

    JsonArray arr = doc["devices"];
    for(JsonObject obj : arr) {
        String type = obj["driver"]; int pin = obj["pin"];
        if(!isPinValid(pin, type)) continue;

        Device* d = DeviceFactory::create(type, obj["id"], obj["name"], pin);
        if(d) { d->begin(); devices.push_back(d); }
    }
    
    rules.clear();
    JsonArray rArr = doc["rules"];
    for(JsonObject obj : rArr) {
        rules.push_back({obj["src"], obj["prm"], obj["op"], obj["val"], obj["tgt"], obj["act"]});
    }
}

// --- LOGIQUE AUTOMATION ---
void checkRules() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    StaticJsonDocument<1024> doc;
    
    for(auto& r : rules) {
        Device* src = nullptr; Device* tgt = nullptr;
        for(auto d : devices) { if(d->getId()==r.srcId) src=d; if(d->getId()==r.tgtId) tgt=d; }

        if(src && tgt) {
            doc.clear(); JsonObject obj = doc.to<JsonObject>(); src->read(obj);
            if(obj.containsKey(r.param)) {
                float val = obj[r.param];
                bool trig = (r.op == ">" && val > r.threshold) || (r.op == "<" && val < r.threshold);
                // Si la cible est un écran, on formate le message
                if(trig) {
                    if(tgt->getType() == DISPLAY_DEV) tgt->writeText(src->getName() + ": " + String(val));
                    else tgt->write("set", r.actionVal);
                }
            }
        }
    }
    xSemaphoreGive(mutex);
}

// --- SETUP & LOOP ---
void setup() {
    Serial.begin(115200);
    mutex = xSemaphoreCreateMutex();
    
    // --- INIT I2C (Default: SDA=21, SCL=22) ---
    Wire.begin();
    Wire.setClock(100000); // Standard speed 100kHz pour compatibilité max

    if(!LittleFS.begin(true)) Serial.println("LittleFS Mount Failed");

    loadConfig();

    WiFiManager wm;
    wm.setClass("invert"); // Dark mode theme :)
    if(!wm.autoConnect("OmniESP-V2", "admin1234")) ESP.restart(); 
    Serial.println(WiFi.localIP());

    // --- API STATUS ---
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req){
        AsyncResponseStream *res = req->beginResponseStream("application/json");
        DynamicJsonDocument doc(8192);
        JsonArray arr = doc.createNestedArray("devices");
        xSemaphoreTake(mutex, portMAX_DELAY);
        for(auto d : devices) {
            JsonObject obj = arr.createNestedObject();
            obj["id"] = d->getId(); obj["name"] = d->getName();
            obj["driver"] = d->getDriver(); obj["pin"] = d->getPin();
            JsonObject val = obj.createNestedObject("val"); d->read(val);
        }
        xSemaphoreGive(mutex);
        serializeJson(doc, *res); req->send(res);
    });

    // --- API I2C SCANNER (Nouveau Endpoint) ---
    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *req){
        xSemaphoreTake(mutex, portMAX_DELAY); // Pause automation pendant le scan
        String res = scanI2C();
        xSemaphoreGive(mutex);
        req->send(200, "application/json", res);
    });

    // --- API CONTROL (Ajout support texte pour LCD) ---
    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req){
        if(req->hasParam("id", true)) {
            String id = req->getParam("id", true)->value();
            xSemaphoreTake(mutex, portMAX_DELAY);
            Device* d = nullptr;
            for(auto dev : devices) if(dev->getId() == id) d = dev;

            if(d) {
                if(req->hasParam("text", true)) {
                    d->writeText(req->getParam("text", true)->value());
                } else if(req->hasParam("cmd", true)) {
                    float v = req->hasParam("val", true) ? req->getParam("val", true)->value().toFloat() : 0;
                    d->write(req->getParam("cmd", true)->value(), v);
                }
            }
            xSemaphoreGive(mutex);
            req->send(200);
        } else req->send(400);
    });

    // --- API CONFIG ---
    server.onRequestBody([](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
        if(req->url() == "/api/config") {
            DynamicJsonDocument doc(8192);
            if (deserializeJson(doc, data)) { req->send(400, "text/plain", "Bad JSON"); return; }

            xSemaphoreTake(mutex, portMAX_DELAY);
            clearDevices(); 
            JsonArray arr = doc["devices"];
            for(JsonObject obj : arr) {
                 String type = obj["driver"]; int pin = obj["pin"];
                 String id = obj["id"]; String name = obj["name"];
                 if (isPinValid(pin, type)) {
                    Device* d = DeviceFactory::create(type, id, name, pin);
                    if(d) { d->begin(); devices.push_back(d); }
                 }
            }
            xSemaphoreGive(mutex);
            saveConfig();
            req->send(200, "text/plain", "Saved");
        }
    });

    server.addHandler(&ws);
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.begin();
}

void loop() {
    checkRules(); 
    static unsigned long last = 0;
    if(millis() - last > 2000) {
        last = millis();
        DynamicJsonDocument doc(4096);
        JsonArray arr = doc.createNestedArray("devices");
        if(xSemaphoreTake(mutex, (TickType_t)100) == pdTRUE) {
            for(auto d : devices) {
                JsonObject obj = arr.createNestedObject();
                obj["id"] = d->getId();
                JsonObject val = obj.createNestedObject("val");
                d->read(val);
            }
            xSemaphoreGive(mutex);
            String out; serializeJson(doc, out);
            ws.textAll(out);
        }
    }
}
