#define LED0 13
#define LED1 14
#define LED2 21

#define BUT_BOOT 0
#define BUT_PLUS 39
#define BUT_MINUS 40

void setup() {
    Serial.begin(115200);
    Serial.println("Hello, ESP32-S3!");
    pinMode(LED0, OUTPUT);
    pinMode(LED1, OUTPUT);
    pinMode(LED2, OUTPUT);

    pinMode(BUT_BOOT, INPUT_PULLUP);
    pinMode(BUT_PLUS, INPUT_PULLUP);
    pinMode(BUT_MINUS, INPUT_PULLUP);
}

void loop() {
    digitalWrite(LED0,!digitalRead(BUT_BOOT));
    digitalWrite(LED1,!digitalRead(BUT_PLUS));
    digitalWrite(LED2,!digitalRead(BUT_MINUS));
    delay(10);
}