#include <Arduino.h>
#include <WiFi.h>

// --- Paho MQTT includes ---
#include "MQTTClient.h"
#include "MQTTFreeRTOS.h"
#include "lwip/api.h"

// --- КОНФИГУРАЦИЯ ---
const char* ssid = "WIFI";
const char* password = "password";
IPAddress local_IP(192, 168, 1, 33);   // Желаният адрес на устройството
IPAddress gateway(192, 168, 1, 1);    // Адресът на вашия рутер
IPAddress subnet(255, 255, 255, 0);   // Маската на мрежата
IPAddress primaryDNS(192, 168, 1, 20);

// MQTT Сървър (IP на Home Assistant или брокера)
const char* mqtt_server = "192.168.1.20";
const uint16_t mqtt_port = 1883;

// MQTT Логин
const char* mqtt_user = "user";
const char* mqtt_pass = "password";
const char* client_id = "LibreTiny_CHT8310_AA";
// --------------------

// Global definitions
#define SDA_PIN 22
#define SCL_PIN 20
#define BatPin 8
float current_temp = 0;
float current_hum = 0;
float current_bat = 0;

// --- Paho MQTT globals ---
Network network;
MQTTClient mqttClient;
unsigned char sendbuf[700];
unsigned char readbuf[700];

// MQTT connection data — kept global for reconnect
MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

/*
 * NetworkConnect with SO_SNDTIMEO timeout.
 * Creates socket, sets send timeout, then connects.
 * Returns 0 on success, -1 on failure.
 */
int NetworkConnectTimeout(Network* n, char* name, int port, int timeout_ms) {
    int retVal = -1;
    uint32_t ipAddress;
    struct sockaddr_in sAddr;
    struct timeval tv;

    Serial.print("[MQTT] Connecting to: ");
    Serial.print(name);
    Serial.print(":");
    Serial.println(port);

    // Try to resolve as hostname first, but if it looks like an IP address,
    // convert it directly using inet_addr()
    ipAddress = inet_addr(name);
    if (ipAddress == INADDR_NONE) {
        // Not a valid IP address, try DNS resolution
        Serial.print("[MQTT] Resolving hostname via DNS: ");
        Serial.println(name);
        if (FreeRTOS_gethostbyname(name, (ip_addr_t*)&ipAddress) == 0) {
            Serial.println("[MQTT] ERROR: gethostbyname_failed");
            goto exit;
        }
    } else {
        Serial.print("[MQTT] Using direct IP: ");
        Serial.println(name);
    }

    sAddr.sin_family = AF_INET;
    sAddr.sin_port = htons(port);
    memcpy((void*)(&sAddr.sin_addr), (void*)&ipAddress, sizeof(ipAddress));

    Serial.println("[MQTT] Creating socket...");
    n->my_socket = FreeRTOS_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (n->my_socket < 0) {
        Serial.println("[MQTT] ERROR: socket_failed");
        goto exit;
    }
    Serial.println("[MQTT] Socket created OK");

    /* Set connect timeout */
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    retVal = FreeRTOS_setsockopt(n->my_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (retVal < 0) {
        Serial.println("[MQTT] WARNING: setsockopt failed, continuing anyway");
    }

    Serial.println("[MQTT] Attempting TCP connect...");
    retVal = FreeRTOS_connect(n->my_socket, (const struct sockaddr*)&sAddr, sizeof(sAddr));
    if (retVal < 0) {
        Serial.print("[MQTT] ERROR: connect_failed, rc=");
        Serial.println(retVal);
        FreeRTOS_closesocket(n->my_socket);
        n->my_socket = 0;
        goto exit;
    }

    Serial.println("[MQTT] Connected successfully!");

exit:
    return retVal;
}

/*
 * (Re)connect to MQTT broker.
 * Closes old socket, re-initialises network, TCP connect, then MQTT CONNECT.
 * Returns SUCCESS or FAILURE.
 */
int mqtt_reconnect() {
    Serial.println("[MQTT] Reconnecting...");

    /* Close old socket if open */
    if (network.my_socket > 0) {
        FreeRTOS_closesocket(network.my_socket);
        network.my_socket = 0;
    }

    /* Reset MQTT client state */
    mqttClient.is_connected = 0;
    mqttClient.ping_outstanding = 0;

    /* New TCP connection */
    if (NetworkConnectTimeout(&network, (char*)mqtt_server, mqtt_port, 5000) != 0) {
        Serial.println("[MQTT] TCP reconnect failed");
        return FAILURE;
    }

    /* Re-init and connect */
    mqtt_client_init(&mqttClient, &network, 10000,
        sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

    int rc = mqtt_connect(&mqttClient, &connectData);
    if (rc != SUCCESS) {
        Serial.print("[MQTT] MQTT reconnect failed, rc=");
        Serial.println(rc);
        return FAILURE;
    }

    Serial.println("[MQTT] Reconnected successfully!");
    return SUCCESS;
}

/*
 * Publish with QOS1 and automatic reconnect on failure.
 * If mqtt_publish() fails, attempts one reconnect + retry.
 */
int mqtt_publish_q1(const char* topic, const char* payload, size_t len, const char* label) {
    MQTTMessage msg;
    msg.qos        = QOS1;
    msg.retained   = 1;
    msg.dup        = 0;
    msg.id         = 0;
    msg.payload    = (char*)payload;
    msg.payloadlen = len;

    int rc = mqtt_publish(&mqttClient, topic, &msg);
    if (rc == SUCCESS) {
        Serial.print("[MQTT] PUBACK received for: ");
        Serial.println(label);
        return rc;
    }

    /* Publish failed — try reconnect once */
    Serial.print("[MQTT] Publish failed (rc=");
    Serial.print(rc);
    Serial.print("), attempting reconnect... ");

    rc = mqtt_reconnect();
    if (rc != SUCCESS) {
        Serial.println("[MQTT] Reconnect failed, cannot publish: ");
        Serial.println(label);
        return FAILURE;
    }

    /* Retry publish after reconnect */
    rc = mqtt_publish(&mqttClient, topic, &msg);
    if (rc == SUCCESS) {
        Serial.print("[MQTT] PUBACK received after reconnect for: ");
        Serial.println(label);
    } else {
        Serial.print("[MQTT] FAILED to publish after reconnect (rc=");
        Serial.print(rc);
        Serial.print("): ");
        Serial.println(label);
        Serial.println("[MQTT] Entering deep sleep for 3 minutes...");
        lt_deep_sleep_config_timer(180000);  // 3 минути
        lt_deep_sleep_enter();
    }
    return rc;
}

// функции за I2C bitbanging
// Помощни функции за симулиране на Open-Drain (8051 стил)
void set_SDA(bool level) {
    if (level) {
        pinMode(SDA_PIN, INPUT_PULLUP); // Високо ниво чрез резистор
    } else {
        digitalWrite(SDA_PIN, LOW);
        pinMode(SDA_PIN, OUTPUT);       // Ниско ниво към земя
    }
}

void set_SCL(bool level) {
    if (level) {
        pinMode(SCL_PIN, INPUT_PULLUP);
    } else {
        digitalWrite(SCL_PIN, LOW);
        pinMode(SCL_PIN, OUTPUT);
    }
}

bool read_SDA() {
    pinMode(SDA_PIN, INPUT_PULLUP);
    return digitalRead(SDA_PIN);
}

// Заменяме delay(t) с микросекунди, защото LibreTiny е много по-бърз от 8051
void i2c_delay(uint8_t t) {
    delayMicroseconds(t * 5); // Коригирайте коефициента според скоростта
}

void iic_start() {
    set_SDA(1); i2c_delay(1);
    set_SCL(1); i2c_delay(5);
    set_SDA(0); i2c_delay(5);
    set_SCL(0); i2c_delay(5);
}

void iic_stop() {
    set_SDA(0); i2c_delay(1);
    set_SCL(1); i2c_delay(5);
    set_SDA(1); i2c_delay(5);
}

void writex(uint8_t val) {
    for (uint8_t i = 0; i < 8; i++) {
        set_SDA(val & 0x80);
        i2c_delay(1);
        set_SCL(1);
        val <<= 1;
        i2c_delay(5);
        set_SCL(0);
        i2c_delay(8);
    }
}

uint8_t readx() {
    uint8_t val = 0;
    set_SCL(0); i2c_delay(1);
    set_SDA(1); // Пускаме линията, за да четем
    for (uint8_t i = 0; i < 8; i++) {
        set_SCL(1);
        val <<= 1;
        if (read_SDA()) val |= 0x01;
        i2c_delay(4);
        set_SCL(0);
        i2c_delay(3);
    }
    return val;
}

bool Check_ACK() {
    set_SDA(1); i2c_delay(1);
    set_SCL(1); i2c_delay(7);
    if (read_SDA()) { // NACK (High)
        set_SCL(0); i2c_delay(6);
        return true;
    } else {          // ACK (Low)
        set_SCL(0); i2c_delay(6);
        return false;
    }
}

void set_ACK(uint8_t val) {
    set_SDA(val);
    set_SCL(1); i2c_delay(7);
    set_SCL(0); i2c_delay(7);
}

uint16_t I2C_Read_Word(uint8_t adr) {
    uint16_t val = 0;
    iic_start();
    writex(0x80); // Адрес на чипа за запис
    if (Check_ACK()) return 0;

    writex(adr);  // Регистър за четене
    if (Check_ACK()) return 0;

    delay(1); // delay_tm(1) в оригинала

    iic_start();
    writex(0x81); // Адрес на чипа за четене
    if (Check_ACK()) return 0;

    val = readx();
    val <<= 8;
    set_ACK(0);   // Пращаме ACK след първия байт

    val |= readx();
    set_ACK(1);   // Пращаме NACK след втория байт

    iic_stop();
    return val;
}

void I2C_Write_Word(uint8_t adr, uint16_t val) {
    iic_start();

    // 1. Адрес на чипа за запис (0x80)
    writex(0x80);
    if (Check_ACK()) { iic_stop(); return; }

    // 2. Адрес на регистъра
    writex(adr);
    if (Check_ACK()) { iic_stop(); return; }

    // 3. Първо изпращаме високия байт (High Byte)
    writex((uint8_t)(val >> 8));
    if (Check_ACK()) { iic_stop(); return; }

    // 4. След това изпращаме ниския байт (Low Byte)
    writex((uint8_t)(val & 0xFF));
    if (Check_ACK()) { iic_stop(); return; }

    iic_stop();
}


// Функцията, която казва на Home Assistant какви сензори имаме
void setup_mqtt_discovery() {
    // Обща дефиниция за устройството
    const char* device_json = ",\"dev\":{\"ids\":\"lt8310_01\",\"name\":\"Sensor LT8310\",\"mdl\":\"LibreTiny CHT8310\",\"mf\":\"Beken\"}";

    // 1. Температура -> ще стане sensor.lt8310_temperature
    String temp_config = "{\"name\":\"Temperature\",\"obj_id\":\"lt8310_temperature\",\"stat_t\":\"lt8310/sensor/state\",\"val_tpl\":\"{{value_json.temp}}\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"uniq_id\":\"lt8310_T\",\"stat_cla\":\"measurement\"" + String(device_json) + "}";
    mqtt_publish_q1("homeassistant/sensor/lt8310/temp/config", temp_config.c_str(), temp_config.length(), "Discovery: Temperature");

    // 2. Влажност -> ще стане sensor.lt8310_humidity
    String hum_config = "{\"name\":\"Humidity\",\"obj_id\":\"lt8310_humidity\",\"stat_t\":\"lt8310/sensor/state\",\"val_tpl\":\"{{value_json.hum}}\",\"unit_of_meas\":\"%\",\"dev_cla\":\"humidity\",\"uniq_id\":\"lt8310_H\",\"stat_cla\":\"measurement\"" + String(device_json) + "}";
    mqtt_publish_q1("homeassistant/sensor/lt8310/hum/config", hum_config.c_str(), hum_config.length(), "Discovery: Humidity");

    // 3. Батерия -> ще стане sensor.lt8310_battery
    String bat_config = "{\"name\":\"Battery\",\"obj_id\":\"lt8310_battery\",\"stat_t\":\"lt8310/sensor/state\",\"val_tpl\":\"{{value_json.bat}}\",\"unit_of_meas\":\"V\",\"dev_cla\":\"voltage\",\"uniq_id\":\"lt8310_B\",\"stat_cla\":\"measurement\"" + String(device_json) + "}";
    mqtt_publish_q1("homeassistant/sensor/lt8310/bat/config", bat_config.c_str(), bat_config.length(), "Discovery: Battery");
}



// Функция за изпращане на данните
void send_sensor_data(float t, float h, float b) {
    char buffer[128];
    // Форматираме данните в JSON: {"temp":22.5,"hum":47.0,"bat":3.25}
    snprintf(buffer, sizeof(buffer), "{\"temp\":%.1f,\"hum\":%.1f,\"bat\":%.2f}", t, h, b);

    mqtt_publish_q1("lt8310/sensor/state", buffer, strlen(buffer), "Sensor data");
}

///////// напрежение батерия
void readBat() {
    pinMode(BatPin, OUTPUT);
    digitalWrite(BatPin, HIGH);
    delay(100);
    //int rawValue = analogRead(23); // Чете стойност от 0 до 1023
    //Serial.println(rawValue);
    current_bat = analogRead(23) * 0.00778;
    //current_bat = voltage;
    digitalWrite(BatPin, LOW);
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- СЪБУЖДАНЕ (Static IP Mode) ---");

    // Не е нужно pinMode за ADC, но е добра практика
    analogReadResolution(10); // 10-битова резолюция (0-1023)

    // 1. Конфигуриране на WiFi със Статичен IP за бързина
    WiFi.mode(WIFI_STA);
    if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
        Serial.println("Статичната конфигурация се провали!");
    }

    WiFi.begin(ssid, password);

    // Таймаут 10 секунди (със статично IP обикновено отнема 2-4 сек)
    unsigned long start_wifi = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_wifi < 10000) {
        delay(100);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("\nWiFi свързан! IP: ");
        Serial.println(WiFi.localIP());

        // 2. Настройка на MQTT (Paho)
        NetworkInit(&network);
        if (NetworkConnectTimeout(&network, (char*)mqtt_server, mqtt_port, 5000) != 0) {  /// таймаут за 5 секунди
            Serial.println("Грешка при свързване с MQTT сървъра!");
            lt_deep_sleep_config_timer(60000);
            lt_deep_sleep_enter();
        } else {

            mqtt_client_init(&mqttClient, &network, 10000,  /// 10 second timeout
                sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

            connectData = MQTTPacket_connectData_initializer;
            connectData.MQTTVersion = 3;
            connectData.keepAliveInterval = 60;
            connectData.cleansession = 1;
            connectData.clientID.cstring = (char*)client_id;
            connectData.username.cstring = (char*)mqtt_user;
            connectData.password.cstring = (char*)mqtt_pass;

            Serial.println("Опит за MQTT свързка...");
            int rc = mqtt_connect(&mqttClient, &connectData);
            if (rc != SUCCESS) {
                Serial.print("Грешка при MQTT свързка: ");
                Serial.println(rc);
                lt_deep_sleep_config_timer(60000);
                lt_deep_sleep_enter();
            } else {
                Serial.println("MQTT свързан!");

                // 3. Регистрация на сензорите (Discovery)
                setup_mqtt_discovery();
            }
        }
    } else {
        Serial.println("\nWiFi връзката се провали (Таймаут).");
        lt_deep_sleep_config_timer(600000);       /// контролно събуждане на 10 минути
        lt_deep_sleep_enter();
    }
}




void loop() {
    /// четене батерия
    readBat();
    // 1. CLEAR INTERRUPT
    int16_t status = I2C_Read_Word(0x02);
    // 2. Set Standby + Config
          // MSB: 0x48 (SD=1, TME=1).
          // LSB: 0xAA (TME=1, POL=0, SRC=01, FQ=01, ATM=0). 10001010
          // FQ=00 means Fault Queue = 1.
          // SRC=01 means Temperature Only.
          // POL=0  means Alert Output Polarity bit active low
          // ATM=0 means ALERT output mode interrupt mode
    I2C_Write_Word(0x03, 0xC88A);
    // 3. One-Shot Measurement
    I2C_Write_Word(0x0F, 0x0000);
    delay(150);
    // 4. Read Temperature
    int16_t temp_raw = I2C_Read_Word(0x00);
    current_temp = (float)temp_raw / 256.0;

    int16_t hum_raw = I2C_Read_Word(0x01);
    current_hum =  (float)(hum_raw & 0x7fff) / 32768.0 * 100.0;
    // 6. Set New Limits (+/- 2C)
    int16_t limit_high = temp_raw + 128;   //256 за 1 градус целзий
    int16_t limit_low  = temp_raw - 128;   //256 за 1 градус целзий
    //lay(50);
    I2C_Write_Word(0x05, limit_high);
    I2C_Write_Word(0x06, limit_low);
    // 7. Return to Normal Mode
    // Conv Rate: 5s -> CR=011 -> 0x6000.
    // Table 7: 100 = 1s. We write 0x0400 (MSB=0x04, LSB=0x00), 0x0200 10 s, 0x0100 60s,
    I2C_Write_Word(0x04, 0x0200);  /// 60 секунди

    // Config Normal: SD=0. LSB remains 0xAA.
    I2C_Write_Word(0x03, 0x088A);
    /// изпращане на данни
    send_sensor_data(current_temp, current_hum, current_bat);

    // закъснение от 4 секунди delay(4000);
    //unsigned long start_wait = millis();
    //while (millis() - start_wait < 4000) {
    //   mqtt_yield(&mqttClient, 100); // Поддържа MQTT сесията жива и обработва ACK
    //   delay(100);    // Дава време на FreeRTOS за WiFi задачи
    //}

    const uint8_t alert_pin = (1 << 7);
    pinMode(7, INPUT_PULLUP);
    lt_deep_sleep_config_gpio(alert_pin, false);
    lt_deep_sleep_config_timer(7200000); /// контролно събуждане на 2 часа
    lt_deep_sleep_enter();
}
