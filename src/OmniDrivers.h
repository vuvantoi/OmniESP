#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <Wire.h> // Central I2C

// --- INCLUDES LIBRARIES V1 & V2 ---
#include <DHT.h>
#include <Adafruit_NeoPixel.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Servo.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_INA219.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_SSD1306.h>

enum DeviceType { SENSOR_BIN, SENSOR_VAL, ACTUATOR_BIN, ACTUATOR_VAL, DISPLAY_DEV };

// --- CLASSE MÈRE ---
class Device {
protected:
    String _id, _name, _driver;
    int _pin; // Pour I2C, ceci devient l'Adresse (ex: 0x27 = 39)
public:
    Device(String id, String name, String driver, int pin) 
        : _id(id), _name(name), _driver(driver), _pin(pin) {}
    virtual ~Device() {}

    String getId() { return _id; }
    String getName() { return _name; }
    String getDriver() { return _driver; } 
    int getPin() { return _pin; } // Returns GPIO or I2C Address
    
    virtual void begin() = 0;
    virtual void read(JsonObject& doc) = 0; 
    virtual void write(String cmd, float val) {} 
    // Nouvelle méthode pour envoyer du texte (LCD/OLED)
    virtual void writeText(String text) {} 
    virtual DeviceType getType() = 0;
};

// ==========================================
// SECTION V1 : GPIO DRIVERS (Compatible)
// ==========================================

class Driver_Digital : public Device {
    bool _isOutput, _inverted, _state;
public:
    Driver_Digital(String id, String name, String type, int pin, bool out, bool inv) 
        : Device(id, name, type, pin), _isOutput(out), _inverted(inv), _state(false) {}
    void begin() override { pinMode(_pin, _isOutput ? OUTPUT : INPUT_PULLUP); if(_isOutput) apply(); }
    void write(String cmd, float val) override { if(!_isOutput) return; _state = (cmd == "toggle") ? !_state : (val >= 1); apply(); }
    void apply() { digitalWrite(_pin, _inverted ? !_state : _state); }
    void read(JsonObject& doc) override {
        bool phy = digitalRead(_pin);
        if(!_isOutput) _state = (phy == (_inverted ? LOW : HIGH));
        doc["val"] = _state ? 1 : 0;
        doc["human"] = _state ? "ON" : "OFF";
    }
    DeviceType getType() override { return _isOutput ? ACTUATOR_BIN : SENSOR_BIN; }
};

class Driver_Analog : public Device {
public:
    Driver_Analog(String id, String name, String type, int pin) : Device(id, name, type, pin) {}
    void begin() override { pinMode(_pin, INPUT); }
    void read(JsonObject& doc) override {
        int raw = analogRead(_pin);
        doc["val"] = raw; doc["volts"] = (raw * 3.3) / 4095.0;
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

class Driver_DHT : public Device {
    DHT* dht;
public:
    Driver_DHT(String id, String name, int pin, int type) : Device(id, name, type==DHT11?"DHT11":"DHT22", pin) { dht = new DHT(pin, type); }
    ~Driver_DHT() { delete dht; }
    void begin() override { dht->begin(); }
    void read(JsonObject& doc) override {
        float t = dht->readTemperature(); float h = dht->readHumidity();
        if(isnan(t)) doc["error"] = "Sensor Error"; else { doc["temp"] = t; doc["hum"] = h; }
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

class Driver_Dallas : public Device {
    OneWire* oneWire; DallasTemperature* sensors;
public:
    Driver_Dallas(String id, String name, int pin) : Device(id, name, "DS18B20", pin) { oneWire = new OneWire(pin); sensors = new DallasTemperature(oneWire); }
    ~Driver_Dallas() { delete sensors; delete oneWire; }
    void begin() override { sensors->begin(); }
    void read(JsonObject& doc) override {
        sensors->requestTemperatures();
        float t = sensors->getTempCByIndex(0);
        if(t == -127) doc["error"] = "Disc."; else doc["temp"] = t;
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

class Driver_Servo : public Device {
    Servo servo; int _pos = 0;
public:
    Driver_Servo(String id, String name, int pin) : Device(id, name, "SERVO", pin) {}
    ~Driver_Servo() { servo.detach(); }
    void begin() override { servo.attach(_pin); }
    void write(String cmd, float val) override { _pos = constrain((int)val, 0, 180); servo.write(_pos); }
    void read(JsonObject& doc) override { doc["angle"] = _pos; }
    DeviceType getType() override { return ACTUATOR_VAL; }
};

class Driver_Neo : public Device {
    Adafruit_NeoPixel* pixels; int _count;
public:
    Driver_Neo(String id, String name, int pin, int count) : Device(id, name, "NEOPIXEL", pin), _count(count) { pixels = new Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800); }
    ~Driver_Neo() { delete pixels; }
    void begin() override { pixels->begin(); pixels->show(); }
    void write(String cmd, float val) override {
        for(int i=0; i<_count; i++) pixels->setPixelColor(i, pixels->ColorHSV((long)val));
        pixels->show();
    }
    void read(JsonObject& doc) override { doc["status"] = "OK"; }
    DeviceType getType() override { return ACTUATOR_VAL; }
};

// ==========================================
// SECTION V2 : I2C INDUSTRIAL DRIVERS
// ==========================================

// Helper Class pour I2C
class Driver_I2C_Base : public Device {
public:
    Driver_I2C_Base(String id, String name, String type, int addr) : Device(id, name, type, addr) {}
    // Note: Wire.begin() est géré dans le main setup
    bool checkConnection() {
        Wire.beginTransmission(_pin);
        return (Wire.endTransmission() == 0);
    }
};

// --- 7. INA219 (Gestion Énergie: Volts/Amps/Watts) ---
class Driver_INA219 : public Driver_I2C_Base {
    Adafruit_INA219* ina;
public:
    Driver_INA219(String id, String name, int addr) : Driver_I2C_Base(id, name, "INA219", addr) {
        ina = new Adafruit_INA219(addr);
    }
    ~Driver_INA219() { delete ina; }
    void begin() override { 
        if(!ina->begin()) Serial.printf("INA219 not found at 0x%X\n", _pin);
    }
    void read(JsonObject& doc) override {
        doc["volts"] = ina->getBusVoltage_V();
        doc["mA"] = ina->getCurrent_mA();
        doc["mW"] = ina->getPower_mW();
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 8. BME280 / BMP280 (Environnement de précision) ---
class Driver_BME280 : public Driver_I2C_Base {
    Adafruit_BME280* bme;
public:
    Driver_BME280(String id, String name, int addr) : Driver_I2C_Base(id, name, "BME280", addr) {
        bme = new Adafruit_BME280();
    }
    ~Driver_BME280() { delete bme; }
    void begin() override { 
        if(!bme->begin(_pin)) Serial.printf("BME280 error at 0x%X\n", _pin);
    }
    void read(JsonObject& doc) override {
        doc["temp"] = bme->readTemperature();
        doc["hum"] = bme->readHumidity();
        doc["pres"] = bme->readPressure() / 100.0F; // hPa
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 9. BH1750 (Luminosité Luxmètre Industriel) ---
class Driver_BH1750 : public Driver_I2C_Base {
    BH1750* lightMeter;
public:
    Driver_BH1750(String id, String name, int addr) : Driver_I2C_Base(id, name, "BH1750", addr) {
        lightMeter = new BH1750(addr); // 0x23 ou 0x5C
    }
    ~Driver_BH1750() { delete lightMeter; }
    void begin() override { 
        if(!lightMeter->begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) Serial.println("BH1750 error");
    }
    void read(JsonObject& doc) override {
        doc["lux"] = lightMeter->readLightLevel();
    }
    DeviceType getType() override { return SENSOR_VAL; }
};

// --- 10. LCD 1602/2004 I2C (Affichage Local) ---
class Driver_LCD : public Driver_I2C_Base {
    LiquidCrystal_I2C* lcd;
    String _lastMsg;
public:
    Driver_LCD(String id, String name, int addr) : Driver_I2C_Base(id, name, "LCD_I2C", addr) {
        lcd = new LiquidCrystal_I2C(addr, 16, 2); // Default 16x2
    }
    ~Driver_LCD() { delete lcd; }
    void begin() override { 
        lcd->init(); lcd->backlight(); 
        lcd->setCursor(0,0); lcd->print("OmniESP V2");
        lcd->setCursor(0,1); lcd->print("Ready...");
    }
    void write(String cmd, float val) override {
        // Simple numeric display
        lcd->clear();
        lcd->setCursor(0,0); lcd->print(_name);
        lcd->setCursor(0,1); lcd->print(val);
        _lastMsg = String(val);
    }
    void writeText(String text) override {
        lcd->clear();
        lcd->setCursor(0,0); lcd->print(_name);
        // Coupe si trop long
        String line2 = text.length() > 16 ? text.substring(0, 16) : text;
        lcd->setCursor(0,1); lcd->print(line2);
        _lastMsg = text;
    }
    void read(JsonObject& doc) override { doc["display"] = _lastMsg; }
    DeviceType getType() override { return DISPLAY_DEV; }
};

// ==========================================
// FACTORY UPGRADE
// ==========================================
class DeviceFactory {
public:
    static Device* create(String type, String id, String name, int pin_or_addr) {
        // --- V1 GPIO DEVICES ---
        if (type == "RELAY" || type == "VALVE" || type == "LOCK") 
            return new Driver_Digital(id, name, type, pin_or_addr, true, false);
        if (type == "BUTTON" || type == "DOOR" || type == "PIR") 
            return new Driver_Digital(id, name, type, pin_or_addr, false, type!="PIR");
        if (type == "LDR" || type == "SOIL" || type == "MQ2") 
            return new Driver_Analog(id, name, type, pin_or_addr);
        if (type == "DHT22") return new Driver_DHT(id, name, pin_or_addr, DHT22);
        if (type == "DHT11") return new Driver_DHT(id, name, pin_or_addr, DHT11);
        if (type == "DS18B20") return new Driver_Dallas(id, name, pin_or_addr);
        if (type == "SERVO") return new Driver_Servo(id, name, pin_or_addr);
        if (type == "NEOPIXEL") return new Driver_Neo(id, name, pin_or_addr, 16);

        // --- V2 I2C DEVICES (pin_or_addr est l'Adresse I2C) ---
        if (type == "INA219") return new Driver_INA219(id, name, pin_or_addr); // Def: 0x40
        if (type == "BME280") return new Driver_BME280(id, name, pin_or_addr); // Def: 0x76
        if (type == "BH1750") return new Driver_BH1750(id, name, pin_or_addr); // Def: 0x23
        if (type == "LCD_I2C") return new Driver_LCD(id, name, pin_or_addr);   // Def: 0x27

        return nullptr;
    }
};
