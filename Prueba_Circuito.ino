#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Servo.h>

// Sketch de prueba por etapas: sirve aunque todavia no esten todos los modulos.
// Abre el Monitor Serie a 9600 para ver que modulo responde.

const uint8_t PIN_RFID_SS   = 10;
const uint8_t PIN_RFID_RST  = 9;
const uint8_t PIN_SERVO     = 8;
const uint8_t PIN_595_LATCH = A2;
const uint8_t PIN_BUZZER    = A3;

char teclas[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte pinesFilas[4] = {7, 6, 5, 4};
byte pinesColumnas[4] = {3, 2, A0, A1};

MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Keypad teclado = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, 4, 4);
Servo servo;
bool servoAbierto = false;

void escribir595(uint8_t valor) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_595_LATCH, LOW);
  SPI.transfer(valor);
  digitalWrite(PIN_595_LATCH, HIGH);
  SPI.endTransaction();
}

void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_595_LATCH, OUTPUT);
  digitalWrite(PIN_595_LATCH, HIGH);
  SPI.begin();

  // Prueba 74HC595: recorre LED1 a LED8
  Serial.println(F("Prueba 74HC595: LED1 -> LED8"));
  for (uint8_t i = 0; i < 8; i++) {
    escribir595(1 << i);
    delay(250);
  }
  escribir595(0xFF);
  delay(500);
  escribir595(0b00000010);

  // Prueba LCD
  lcd.init();
  lcd.backlight();
  lcd.print("PRUEBA CIRCUITO");
  lcd.setCursor(0, 1);
  lcd.print("ACERQUE TARJETA");

  // Prueba RC522
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.print(F("RC522 version: 0x"));
  Serial.println(version, HEX);
  if (version == 0x00 || version == 0xFF) {
    Serial.println(F("RC522 NO RESPONDE: revisa cables SPI y 3.3V"));
  } else {
    Serial.println(F("RC522 OK"));
  }

  servo.attach(PIN_SERVO);
  servo.write(0);

  digitalWrite(PIN_BUZZER, HIGH);
  delay(100);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println(F("Listo. Tarjeta = muestra UID | Tecla = la muestra | A = mueve servo"));
}

void loop() {
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    escribir595(0b00000100);
    lcd.clear();
    lcd.print("UID:");
    lcd.setCursor(0, 1);
    Serial.print(F("UID:"));
    for (byte i = 0; i < rfid.uid.size; i++) {
      if (rfid.uid.uidByte[i] < 0x10) lcd.print('0');
      lcd.print(rfid.uid.uidByte[i], HEX);
      lcd.print(' ');
      Serial.print(rfid.uid.uidByte[i] < 0x10 ? " 0" : " ");
      Serial.print(rfid.uid.uidByte[i], HEX);
    }
    Serial.println();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);
    escribir595(0b00100000);
  }

  char tecla = teclado.getKey();
  if (tecla) {
    Serial.print(F("Tecla: "));
    Serial.println(tecla);
    lcd.clear();
    lcd.print("TECLA: ");
    lcd.print(tecla);
    escribir595(0b00001000);
    digitalWrite(PIN_BUZZER, HIGH);
    delay(50);
    digitalWrite(PIN_BUZZER, LOW);
    if (tecla == 'A') {
      servoAbierto = !servoAbierto;
      servo.write(servoAbierto ? 90 : 0);
      lcd.setCursor(0, 1);
      lcd.print(servoAbierto ? "SERVO 90" : "SERVO 0");
    }
  }
}
