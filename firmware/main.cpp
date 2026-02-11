#include "IIOTDEVKIT4G.h"
#include <String>
#include <esp_sleep.h>
#include <esp_task_wdt.h>

#define Debug 0
IIOTDEVKIT4G IIOT_Dev_kit;
Broker TB_Broker0;

#define LIGHT_SLEEP_S 2
#define WAKE_UP_DELAY_MS 300
#define LIGHT_SLEEP_DURATIONS_MS (LIGHT_SLEEP_S * 1000 - 2 * WAKE_UP_DELAY_MS)
#define LIGHT_SLEEP_DURATIONS_US (LIGHT_SLEEP_DURATIONS_MS * 1000)

#define MQTT_SERVER  "thingsboard.cloud"
#define MQTT_PORT    "1883"
#define MQTT_CLIENTID "130u9ei0zyyctp9vsi53"
#define MQTT_USERNAME "w4q2tmsjvmi37q1bkkob"
#define MQTT_PASSWORD "qw054oib8tbeu0b6ilrp"
#define MQTT_TELE_Topic "v1/devices/me/telemetry"

#define VOLTAGE_PIN 4

#define P_TP1 18
#define P_TP2 19
#define P_TP3 36
#define P_TP4 39
#define P_TP5 34
#define P_TP6 35
#define P_TP7 32
#define P_TP8 33

#define Buzzer 14

bool send_data = false;
bool detection_send = false;
bool first_time = true;
bool voltage_send = false;
bool connected = false;

uint8_t TP[8] = {0};
uint8_t c_TP[8] = {0};

#define _Check_Sleep     0
#define _ON_Init_4G      1
#define _MQTT_START      2
#define _MQTT_Connect    3
#define _send_data       4

int state;
int error_count = 0;
#define Error_count_th 10

unsigned long lastVoltageSend = 0;
#define VOLTAGE_INTERVAL_MS 30000  // every 30s

// Ensure PDP context is active before MQTT
bool ensurePDPActive() {
    String resp;
    Serial.println("Checking PDP context...");
    IIOT_Dev_kit.SEND_AT_CMD_RAW("AT+CGATT?\r\n", 2000, &resp);
    if (resp.indexOf("+CGATT: 1") == -1) {
        Serial.println("Attaching PDP context...");
        if (!IIOT_Dev_kit.SEND_AT_CMD_RAW("AT+CGATT=1\r\n", 5000, &resp)) {
            Serial.println("PDP attach failed");
            return false;
        }
    }

    Serial.println("Activating PDP context...");
    if (!IIOT_Dev_kit.SEND_AT_CMD_RAW("AT+CGACT=1,1\r\n", 5000, &resp)) {
        Serial.println("PDP activation failed");
        return false;
    }

    Serial.println("PDP active ✔");
    return true;
}

void IRAM_ATTR datasendtimer(){
    send_data = true;
    voltage_send = true;
}

void flush_buffers(){
    while (Serial2.available()) Serial2.read();
    delay(10);
}

void restart_device(){
    Serial.println("Restarting device due to error...");
    delay(1000);
    esp_restart();
}

void setup(){
    pinMode(P_TP1, INPUT);
    pinMode(P_TP2, INPUT);
    pinMode(P_TP3, INPUT);
    pinMode(P_TP4, INPUT);
    pinMode(P_TP5, INPUT);
    pinMode(P_TP6, INPUT);
    pinMode(P_TP7, INPUT);
    pinMode(P_TP8, INPUT);
    pinMode(Buzzer, OUTPUT);
    digitalWrite(Buzzer, LOW);

    Serial.begin(115200);
    Serial.println("Start...");
    state = _ON_Init_4G;
    detection_send = true;
    send_data = true;
    voltage_send = true;
    first_time = true;

    delay(5000);
    config_Timer(60);
    esp_task_wdt_init(5 * 60, true);
    esp_task_wdt_add(NULL);
}

void loop(){
    delay(100);

    // Trigger voltage telemetry every 30 seconds
    if (millis() - lastVoltageSend >= VOLTAGE_INTERVAL_MS) {
        voltage_send = true;
        lastVoltageSend = millis();
    }

    switch (state){

        case _Check_Sleep:
            c_TP[0] = digitalRead(P_TP1) == HIGH ? 1 : 0;
            c_TP[1] = digitalRead(P_TP2) == HIGH ? 1 : 0;
            c_TP[2] = digitalRead(P_TP3) == HIGH ? 1 : 0;
            c_TP[3] = digitalRead(P_TP4) == HIGH ? 1 : 0;
            c_TP[4] = digitalRead(P_TP5) == HIGH ? 1 : 0;
            c_TP[5] = digitalRead(P_TP6) == HIGH ? 1 : 0;
            c_TP[6] = digitalRead(P_TP7) == HIGH ? 1 : 0;
            c_TP[7] = digitalRead(P_TP8) == HIGH ? 1 : 0;

            for (uint8_t x = 0; x < 8; x++) {
                if (c_TP[x] != TP[x]) {
                    detection_send = true;
                    send_data = true;
                }
            }

            if(send_data || voltage_send){
                state = _send_data;
            } else {
                esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_DURATIONS_US);
                delay(WAKE_UP_DELAY_MS);
                esp_light_sleep_start();
                delay(WAKE_UP_DELAY_MS);
            }
            break;

        case _ON_Init_4G:
            flush_buffers();
            Serial.println("state - _ON_Init_4G");
            if(G_start_connect()){ 
                state = _MQTT_START;
            } else {
                error_count++;
                if(error_count > Error_count_th) restart_device();
                state = _Check_Sleep;
            }
            break;

        case _MQTT_START:
            Serial.println("state - _MQTT_START");

            if (!ensurePDPActive()) {
                Serial.println("PDP context not active — retrying...");
                delay(2000);
                state = _MQTT_START;
                break;
            }

            // Ensure no previous MQTT session
            Serial.println("Closing existing MQTT session...");
            {
                String dummyResp;
                IIOT_Dev_kit.SEND_AT_CMD_RAW("AT+CMQTTSTOP\r\n", 5000, &dummyResp);
                delay(1000);
                IIOT_Dev_kit.SEND_AT_CMD_RAW("AT+CMQTTREL=0\r\n", 1000, &dummyResp);
                delay(1000);
            }

            if (IIOT_Dev_kit.MQTT_SETUP(&TB_Broker0, MQTT_SERVER, MQTT_PORT)) {
                Serial.println("MQTT client started successfully");
                state = _MQTT_Connect;
            } else {
                Serial.println("MQTT_SETUP failed — retrying...");
                error_count++;
                if (error_count > Error_count_th) restart_device();
                delay(2000);
                state = _MQTT_START;
            }
            break;

        case _MQTT_Connect:
            Serial.println("state - _MQTT_Connect");
            if(IIOT_Dev_kit.MQTT_CONNECT(&TB_Broker0, MQTT_CLIENTID, MQTT_USERNAME, MQTT_PASSWORD)){
                connected = true;
                state = _send_data;
            } else {
                restart_device();
            }
            break;

        case _send_data:
            Serial.println("state - _send_data");

            if(detection_send){
                for (uint8_t x = 0; x < 8; x++){
                    if(first_time || c_TP[x] != TP[x]){
                        String data = "{'\