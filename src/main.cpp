#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "OmniDrivers.h"

// --- VARIABLES GLOBALES ---
std::vector<Device*> devices;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
SemaphoreHandle_t mutex;

// Structure pour l'Automation
struct Rule {
    String srcId;     // Qui on écoute (ex: "dht_4")
    String param;     // Quel paramètre (ex: "temp")
    String op;        // Opérateur (">", "<")
    float threshold;  // Valeur seuil (25.0)
    String tgtId;     // Qui on active (ex: "relay_23")
    float actionVal;  // Valeur à envoyer (1.0 = ON)
};
std::vector<Rule> rules;

// --- GESTION CONFIGURATION ---
void saveConfig() {
    File f = LittleFS.open("/config.json", "w");
    DynamicJsonDocument doc(8192);
    JsonArray devArr = doc.createNestedArray("devices");
    
    // Sauvegarder Devices
    xSemaphoreTake(mutex, portMAX_DELAY);
    for(auto d : devices) {
        JsonObject obj = devArr.createNestedObject();
        obj["id"] = d->getId();
        obj["driver"] = d->getDriver(); // Le type interne
        obj["name"] = d->getName(); // Le nom donné par Papy
        obj["pin"] = d->getPin();
    }
    xSemaphoreGive(mutex);

    // Sauvegarder Règles (Mockup pour l'exemple, extensible via UI)
    JsonArray ruleArr = doc.createNestedArray("rules");
    for(auto r : rules) {
        JsonObject obj = ruleArr.createNestedObject();
        obj["src"] = r.srcId; obj["prm"] = r.param; 
        obj["op"] = r.op; obj["val"] = r.threshold;
        obj["tgt"] = r.tgtId; obj["act"] = r.actionVal;
    }

    serializeJson(doc, f);
    f.close();
}

void loadConfig() {
    if(!LittleFS.exists("/config.json")) return;
    File f = LittleFS.open("/config.json", "r");
    DynamicJsonDocument doc(8192);
    deserializeJson(doc, f);
    f.close();

    JsonArray arr = doc["devices"];
    for(JsonObject obj : arr) {
        Device* d = DeviceFactory::create(obj["driver"], obj["id"], obj["name"], obj["pin"]);
        if(d) {
            d->begin();
            devices.push_back(d);
        }
    }
    
    JsonArray rArr = doc["rules"];
    for(JsonObject obj : rArr) {
        rules.push_back({obj["src"], obj["prm"], obj["op"], obj["val"], obj["tgt"], obj["act"]});
    }
}

// --- LOGIQUE AUTOMATION (Check toutes les secondes) ---
void checkRules() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    StaticJsonDocument<512> doc;
    
    for(auto& r : rules) {
        // Trouver source
        Device* src = nullptr;
        Device* tgt = nullptr;
        for(auto d : devices) {
            if(d->getId() == r.srcId) src = d;
            if(d->getId() == r.tgtId) tgt = d;
        }

        if(src && tgt) {
            doc.clear();
            JsonObject obj = doc.to<JsonObject>();
            src->read(obj);
            
            if(obj.containsKey(r.param)) {
                float val = obj[r.param];
                bool trig = false;
                if(r.op == ">" && val > r.threshold) trig = true;
                if(r.op == "<" && val < r.threshold) trig = true;
                
                if(trig) tgt->write("set", r.actionVal);
            }
        }
    }
    xSemaphoreGive(mutex);
}

// --- SETUP & LOOP ---
void setup() {
    Serial.begin(115200);
    mutex = xSemaphoreCreateMutex();
    LittleFS.begin(true);
    
    loadConfig();

    // WiFi
    WiFi.begin("SSID", "PASSWORD"); // Remplacer ou utiliser WiFiManager
    while(WiFi.status() != WL_CONNECTED) delay(500);
    Serial.println(WiFi.localIP());

    // API
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req){
        AsyncResponseStream *res = req->beginResponseStream("application/json");
        DynamicJsonDocument doc(8192);
        JsonArray arr = doc.createNestedArray("devices");
        
        xSemaphoreTake(mutex, portMAX_DELAY);
        for(auto d : devices) {
            JsonObject obj = arr.createNestedObject();
            obj["id"] = d->getId();
            obj["name"] = d->getName();
            obj["driver"] = d->getDriver();
            JsonObject val = obj.createNestedObject("val");
            d->read(val);
        }
        xSemaphoreGive(mutex);
        
        serializeJson(doc, *res);
        req->send(res);
    });

    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req){
        if(req->hasParam("id", true) && req->hasParam("cmd", true)) {
            String id = req->getParam("id", true)->value();
            String cmd = req->getParam("cmd", true)->value();
            float v = req->hasParam("val", true) ? req->getParam("val", true)->value().toFloat() : 0;
            
            xSemaphoreTake(mutex, portMAX_DELAY);
            for(auto d : devices) if(d->getId() == id) d->write(cmd, v);
            xSemaphoreGive(mutex);
            req->send(200);
        } else req->send(400);
    });

    // API pour ajouter un device (Config dynamique)
    server.onRequestBody([](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
        if(req->url() == "/api/config") {
            DynamicJsonDocument doc(4096);
            deserializeJson(doc, data);
            
            // Re-création complète config
            xSemaphoreTake(mutex, portMAX_DELAY);
            // Vider devices actuels (leak memory here in simple implementation, use smart pointers in full prod)
            devices.clear(); 
            JsonArray arr = doc["devices"];
            for(JsonObject obj : arr) {
                 Device* d = DeviceFactory::create(obj["driver"], obj["id"], obj["name"], obj["pin"]);
                 if(d) { d->begin(); devices.push_back(d); }
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
    checkRules(); // Automation
    
    // Broadcast WS
    static unsigned long last = 0;
    if(millis() - last > 2000) {
        last = millis();
        DynamicJsonDocument doc(4096);
        JsonArray arr = doc.createNestedArray("devices");
        xSemaphoreTake(mutex, portMAX_DELAY);
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
