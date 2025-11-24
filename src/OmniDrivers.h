#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// --- INCLUDES LIBRARIES ---
#include <DHT.h>
#include <Adafruit_BME280.h>
#include <Adafruit_NeoPixel.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Servo.h>

enum DeviceType { SENSOR_BIN, SENSOR_VAL, ACTUATOR_BIN, ACTUATOR_VAL };

// --- CLASSE MÈRE ---
class Device {
protected:
    String _id, _name, _driver;
    int _pin;
public:
    Device(String id, String name, String driver, int pin) 
        : _id(id), _name(name), _driver(driver), _pin(pin) {}
    
    // Destructeur virtuel essentiel pour le nettoyage polymorphique
    virtual ~Device() {}

    String getId() { return _id; }
    String getName() { return _name; }
    String getDriver() { return _driver; } 
    int getPin() { return _pin; }
    
    virtual void begin() = 0;
    virtual void read(JsonObject& doc) = 0; 
    virtual void write(String cmd, float val) {} 
    virtual DeviceType getType() = 0;
};

// --- 1. DIGITAL I/O ---
class Driver_Digital : public Device {
    bool _isOutput, _inverted, _state;
public:
    Driver_Digital(String id, String name, String type, int pin, bool out, bool inv) 
        : Device(id, name, type, pin), _isOutput(out), _inverted(inv), _state(false) {}

    void begin() override {
        pinMode(_pin, _isOutput ? OUTPUT : INPUT_PULLUP);
        if(_isOutput) apply();
    }
    void write(String cmd, float val) override {
        if(!_isOutput) return;
        if(cmd == "toggle") _state = !_state; else _state = (val >= 1);
        apply();
    }
    void apply() { digitalWrite(_pin, _inverted ? !_state : _state); }
    void read(JsonObject& doc) override {
        bool phy = digitalRead(_pin);
        if(!_isOutput) _state = (phy == (_inverted ? LOW : HIGH));
        doc["val"] = _state ? 1 : 0;
        doc["human"] = _state ? "ACTIF" : "INACTIF";
    }
    DeviceType getType() override { return _isOutput ? ACTUATOR_BIN : SENSOR_BIN; }
};

// --- 2. ANALOG INPUT ---
class Driver_Analog : public Device {
public:
    Driver_Analog(String id, String name, String type, int pin) : Device(id, name, type, pin) {}
    void begin() override { pinMode(_pin, INPUT); }
    void read(JsonObject& doc) override {
        int raw = analogRead(_pin);
        doc["val"] = raw;
        doc["percent"] = map(raw, 0, 4095, 0, 100);
        doc["volts"] = (raw * 3.3) / 4095.0;
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 3. DHT SENSOR ---
class Driver_DHT : public Device {
    DHT* dht;
public:
    Driver_DHT(String id, String name, int pin, int type) : Device(id, name, type==DHT11?"DHT11":"DHT22", pin) {
        dht = new DHT(pin, type);
    }
    // Destructeur ajouté pour libérer la mémoire du driver
    ~Driver_DHT() {
        if(dht) { delete dht; dht = nullptr; }
    }

    void begin() override { dht->begin(); }
    void read(JsonObject& doc) override {
        float t = dht->readTemperature();
        float h = dht->readHumidity();
        if (isnan(t) || isnan(h)) {
            doc["error"] = "Read Fail";
        } else {
            doc["temp"] = t;
            doc["hum"] = h;
        }
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 4. DALLAS DS18B20 ---
class Driver_Dallas : public Device {
    OneWire* oneWire;
    DallasTemperature* sensors;
public:
    Driver_Dallas(String id, String name, int pin) : Device(id, name, "DS18B20", pin) {
        oneWire = new OneWire(pin);
        sensors = new DallasTemperature(oneWire);
    }
    // Destructeur complet pour libérer OneWire et DallasTemp
    ~Driver_Dallas() {
        if(sensors) { delete sensors; sensors = nullptr; }
        if(oneWire) { delete oneWire; oneWire = nullptr; }
    }

    void begin() override { sensors->begin(); }
    void read(JsonObject& doc) override {
        sensors->requestTemperatures();
        float t = sensors->getTempCByIndex(0);
        if(t == -127) doc["error"] = "Disconnected";
        else doc["temp"] = t;
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 5. SERVO MOTEUR ---
class Driver_Servo : public Device {
    Servo servo;
    int _pos = 0;
public:
    Driver_Servo(String id, String name, int pin) : Device(id, name, "SERVO", pin) {}
    // ServoESP32 gère sa propre mémoire interne généralement, mais detach est conseillé
    ~Driver_Servo() { servo.detach(); }
    
    void begin() override { servo.attach(_pin); }
    void write(String cmd, float val) override {
        _pos = constrain((int)val, 0, 180);
        servo.write(_pos);
    }
    void read(JsonObject& doc) override { doc["angle"] = _pos; }
    DeviceType getType() override { return ACTUATOR_VAL; }
};

// --- 6. NEOPIXEL ---
class Driver_Neo : public Device {
    Adafruit_NeoPixel* pixels;
    int _count;
public:
    Driver_Neo(String id, String name, int pin, int count) : Device(id, name, "NEOPIXEL", pin), _count(count) {
        pixels = new Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
    }
    // Destructeur ajouté
    ~Driver_Neo() {
        if(pixels) { delete pixels; pixels = nullptr; }
    }

    void begin() override { pixels->begin(); pixels->show(); }
    void write(String cmd, float val) override {
        for(int i=0; i<_count; i++) pixels->setPixelColor(i, pixels->ColorHSV((long)val));
        pixels->show();
    }
    void read(JsonObject& doc) override { doc["status"] = "Ready"; }
    DeviceType getType() override { return ACTUATOR_VAL; }
};

// --- FACTORY INTELLIGENTE ---
class DeviceFactory {
public:
    static Device* create(String type, String id, String name, int pin) {
        // ACTUATORS
        if (type == "RELAY" || type == "VALVE" || type == "PUMP" || type == "HEATER" || type == "LOCK") 
            return new Driver_Digital(id, name, type, pin, true, false); 
        
        if (type == "LIGHT_INV") return new Driver_Digital(id, name, type, pin, true, true);

        // SENSORS DIGITAL
        if (type == "BUTTON" || type == "DOOR" || type == "WINDOW" || type == "REED") 
            return new Driver_Digital(id, name, type, pin, false, true); 

        if (type == "PIR" || type == "MOTION" || type == "VIBRATION" || type == "SOUND_DIG") 
            return new Driver_Digital(id, name, type, pin, false, false); 

        // SENSORS ANALOG 
        if (type == "POT" || type == "LDR" || type == "SOIL" || type == "WATER" || 
            type == "MQ2" || type == "MQ135" || type == "MQ7" || type == "VOLTAGE") 
            return new Driver_Analog(id, name, type, pin);

        // SPECIFICS
        if (type == "DHT11") return new Driver_DHT(id, name, pin, DHT11);
        if (type == "DHT22") return new Driver_DHT(id, name, pin, DHT22);
        if (type == "DS18B20") return new Driver_Dallas(id, name, pin);
        if (type == "SERVO") return new Driver_Servo(id, name, pin);
        if (type == "NEOPIXEL") return new Driver_Neo(id, name, pin, 12);

        return nullptr;
    }
};
