#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Servo.h>
#include <EEPROM.h>

// ---------------- Asignacion de pines ----------------
const uint8_t PIN_RFID_SS   = 10;
const uint8_t PIN_RFID_RST  = 9;
const uint8_t PIN_SERVO     = 8;
const uint8_t PIN_595_LATCH = A2;
const uint8_t PIN_BUZZER    = A3;
const uint8_t LCD_DIRECCION = 0x27;

const byte FILAS = 4;
const byte COLUMNAS = 4;
char teclas[FILAS][COLUMNAS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte pinesFilas[FILAS] = {7, 6, 5, 4};
byte pinesColumnas[COLUMNAS] = {3, 2, A0, A1};

// ---------------- Teclas de control ----------------
const char TECLA_BORRAR     = '*';
const char TECLA_CONFIRMAR  = '#';
const char TECLA_CERRAR     = 'A';
const char TECLA_REGISTRO   = 'B';
const char TECLA_CANCELAR   = 'C';
const char TECLA_ALARMA_OFF = 'D';

// ---------------- Estados (valor enviado al 74HC595) ----------------
enum Estado : uint8_t {
  INICIO         = 0b00000001,
  ESPERANDO_RFID = 0b00000010,
  VALIDANDO_RFID = 0b00000100,
  ESPERANDO_PIN  = 0b00001000,
  VALIDANDO_PIN  = 0b00010000,
  AUTORIZADO     = 0b00100000,
  DENEGADO       = 0b01000000,
  ALARMA         = 0b10000000
};

// ---------------- Organizacion de la EEPROM ----------------
const int     EE_DIR_FIRMA     = 0;
const uint8_t EE_FIRMA         = 0xA5;
const int     EE_DIR_CANTIDAD  = 1;
const int     EE_DIR_USUARIOS  = 2;
const uint8_t MAX_USUARIOS     = 10;
const uint8_t USUARIOS_INICIALES = 2;
const uint8_t UID_MAX          = 7;
const uint8_t PIN_LARGO        = 4;

struct Usuario {
  uint8_t uidLargo;
  uint8_t uid[UID_MAX];
  char pin[PIN_LARGO];
};

// ---------------- Parametros ----------------
const uint8_t SERVO_CERRADO = 0;
const uint8_t SERVO_ABIERTO = 90;
const uint8_t MAX_INTENTOS  = 3;
const unsigned long TIEMPO_MENSAJE = 1500;

// ---------------- Objetos ----------------
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
LiquidCrystal_I2C lcd(LCD_DIRECCION, 16, 2);
Keypad teclado = Keypad(makeKeymap(teclas), pinesFilas, pinesColumnas, FILAS, COLUMNAS);
Servo puerta;

// ---------------- Variables de estado (SRAM) ----------------
Estado estadoActual = INICIO;
int8_t usuarioActual = -1;
uint8_t intentosFallidos = 0;
char pinIngresado[PIN_LARGO];
uint8_t digitosIngresados = 0;

// =====================================================
// 74HC595: comparte MOSI (D11) y SCK (D13) con el RC522.
// Las salidas solo cambian con el flanco de subida del latch (A2).
// =====================================================
void escribir595(uint8_t valor) {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_595_LATCH, LOW);
  SPI.transfer(valor);
  digitalWrite(PIN_595_LATCH, HIGH);
  SPI.endTransaction();
}

void cambiarEstado(Estado nuevo) {
  estadoActual = nuevo;
  escribir595((uint8_t)nuevo);
  Serial.print(F("Estado -> "));
  Serial.println((uint8_t)nuevo, BIN);
}

// =====================================================
// LCD y buzzer
// =====================================================
void mostrar(const char* linea1, const char* linea2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(linea1);
  lcd.setCursor(0, 1);
  lcd.print(linea2);
}

void pitido(unsigned int ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

// =====================================================
// EEPROM
// =====================================================
bool memoriaInicializada() {
  return EEPROM.read(EE_DIR_FIRMA) == EE_FIRMA;
}

uint8_t cantidadUsuarios() {
  uint8_t n = EEPROM.read(EE_DIR_CANTIDAD);
  return (n > MAX_USUARIOS) ? 0 : n;
}

int direccionUsuario(uint8_t indice) {
  return EE_DIR_USUARIOS + indice * sizeof(Usuario);
}

void leerUsuario(uint8_t indice, Usuario &u) {
  EEPROM.get(direccionUsuario(indice), u);
}

void guardarUsuario(uint8_t indice, const Usuario &u) {
  EEPROM.put(direccionUsuario(indice), u);
}

int8_t buscarUsuario(const uint8_t* uid, uint8_t largo) {
  uint8_t total = cantidadUsuarios();
  Usuario u;
  for (uint8_t i = 0; i < total; i++) {
    leerUsuario(i, u);
    if (u.uidLargo == largo && memcmp(u.uid, uid, largo) == 0) {
      return i;
    }
  }
  return -1;
}

// =====================================================
// RFID
// =====================================================
bool leerTarjeta(uint8_t* uid, uint8_t &largo) {
  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;
  largo = (rfid.uid.size > UID_MAX) ? UID_MAX : rfid.uid.size;
  memcpy(uid, rfid.uid.uidByte, largo);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  imprimirUID(uid, largo);
  return true;
}

void imprimirUID(const uint8_t* uid, uint8_t largo) {
  Serial.print(F("UID:"));
  for (uint8_t i = 0; i < largo; i++) {
    Serial.print(uid[i] < 0x10 ? " 0" : " ");
    Serial.print(uid[i], HEX);
  }
  Serial.println();
}

// =====================================================
// Ingreso de PIN
// =====================================================
void actualizarPinLCD() {
  lcd.setCursor(0, 1);
  for (uint8_t i = 0; i < PIN_LARGO; i++) {
    lcd.print(i < digitosIngresados ? '*' : '_');
  }
  lcd.print(F("            "));
}

// Devuelve true cuando el PIN quedo confirmado con 4 digitos
bool procesarTeclaPIN(char tecla) {
  if (tecla >= '0' && tecla <= '9') {
    if (digitosIngresados < PIN_LARGO) {
      pinIngresado[digitosIngresados++] = tecla;
      actualizarPinLCD();
    }
  } else if (tecla == TECLA_BORRAR) {
    if (digitosIngresados > 0) {
      digitosIngresados--;
      actualizarPinLCD();
    }
  } else if (tecla == TECLA_CONFIRMAR) {
    if (digitosIngresados == PIN_LARGO) return true;
    pitido(80);
  }
  return false;
}

// =====================================================
// Registro de usuarios (se guarda en EEPROM)
// =====================================================
void registrarUsuarios() {
  EEPROM.update(EE_DIR_FIRMA, 0x00);
  EEPROM.update(EE_DIR_CANTIDAD, 0);
  char linea[17];

  for (uint8_t i = 0; i < USUARIOS_INICIALES; i++) {
    Usuario u;
    memset(&u, 0, sizeof(u));
    snprintf(linea, sizeof(linea), "REGISTRO USR %u", i + 1);
    mostrar(linea, "PRESENTE TARJETA");
    while (!leerTarjeta(u.uid, u.uidLargo)) {}

    if (buscarUsuario(u.uid, u.uidLargo) >= 0) {
      mostrar("TARJETA YA", "REGISTRADA");
      pitido(400);
      delay(TIEMPO_MENSAJE);
      i--;
      continue;
    }
    pitido(100);

    mostrar("PIN NUEVO (4):", "");
    digitosIngresados = 0;
    actualizarPinLCD();
    bool listo = false;
    while (!listo) {
      char tecla = teclado.getKey();
      if (tecla) listo = procesarTeclaPIN(tecla);
    }
    memcpy(u.pin, pinIngresado, PIN_LARGO);

    guardarUsuario(i, u);
    EEPROM.update(EE_DIR_CANTIDAD, i + 1);
    mostrar("USUARIO", "GUARDADO");
    pitido(100);
    delay(1000);
  }
  EEPROM.update(EE_DIR_FIRMA, EE_FIRMA);
  mostrar("REGISTRO", "COMPLETADO");
  delay(TIEMPO_MENSAJE);
}

// =====================================================
// Transiciones del sistema
// =====================================================
void pantallaPrincipal() {
  usuarioActual = -1;
  intentosFallidos = 0;
  digitosIngresados = 0;
  cambiarEstado(ESPERANDO_RFID);
  mostrar("CONTROL ACCESO", "PRESENTE TARJETA");
}

void iniciarIngresoPIN() {
  digitosIngresados = 0;
  cambiarEstado(ESPERANDO_PIN);
  mostrar("INGRESE SU PIN", "");
  actualizarPinLCD();
}

void autorizarAcceso() {
  cambiarEstado(AUTORIZADO);
  mostrar("ACCESO", "AUTORIZADO");
  pitido(150);
  puerta.write(SERVO_ABIERTO);
  delay(TIEMPO_MENSAJE);
  mostrar("PUERTA ABIERTA", "[A] CERRAR");
}

void activarAlarma() {
  cambiarEstado(DENEGADO);
  mostrar("ACCESO DENEGADO", "");
  delay(TIEMPO_MENSAJE);
  cambiarEstado(ALARMA);
  digitalWrite(PIN_BUZZER, HIGH);
  mostrar("ACCESO BLOQUEADO", "ALARMA ACTIVADA");
}

void validarPIN() {
  cambiarEstado(VALIDANDO_PIN);
  Usuario u;
  leerUsuario(usuarioActual, u);

  if (memcmp(u.pin, pinIngresado, PIN_LARGO) == 0) {
    autorizarAcceso();
    return;
  }

  intentosFallidos++;
  if (intentosFallidos >= MAX_INTENTOS) {
    activarAlarma();
    return;
  }

  char linea[17];
  snprintf(linea, sizeof(linea), "INTENTO %u DE %u", intentosFallidos, MAX_INTENTOS);
  cambiarEstado(DENEGADO);
  mostrar("PIN INCORRECTO", linea);
  pitido(300);
  delay(TIEMPO_MENSAJE);
  iniciarIngresoPIN();
}

// =====================================================
// Manejadores por estado (llamados desde loop)
// =====================================================
void atenderEsperaRFID() {
  uint8_t uid[UID_MAX];
  uint8_t largo;
  if (!leerTarjeta(uid, largo)) return;

  cambiarEstado(VALIDANDO_RFID);
  int8_t indice = buscarUsuario(uid, largo);

  if (indice < 0) {
    cambiarEstado(DENEGADO);
    mostrar("TARJETA INVALIDA", "ACCESO DENEGADO");
    pitido(600);
    delay(TIEMPO_MENSAJE);
    pantallaPrincipal();
    return;
  }

  usuarioActual = indice;
  intentosFallidos = 0;
  pitido(80);
  iniciarIngresoPIN();
}

void atenderEsperaPIN() {
  char tecla = teclado.getKey();
  if (!tecla) return;
  if (tecla == TECLA_CANCELAR) {
    pantallaPrincipal();
    return;
  }
  if (procesarTeclaPIN(tecla)) validarPIN();
}

void atenderPuertaAbierta() {
  if (teclado.getKey() != TECLA_CERRAR) return;
  puerta.write(SERVO_CERRADO);
  mostrar("CERRANDO", "PUERTA");
  delay(1000);
  pantallaPrincipal();
}

void atenderAlarma() {
  if (teclado.getKey() != TECLA_ALARMA_OFF) return;
  digitalWrite(PIN_BUZZER, LOW);
  mostrar("ALARMA", "DESACTIVADA");
  delay(TIEMPO_MENSAJE);
  pantallaPrincipal();
}

// =====================================================
// Programa principal
// =====================================================
void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_595_LATCH, OUTPUT);
  digitalWrite(PIN_595_LATCH, HIGH);

  SPI.begin();
  cambiarEstado(INICIO);

  lcd.init();
  lcd.backlight();
  mostrar("SISTEMA ACCESO", "INICIANDO...");

  rfid.PCD_Init();
  puerta.attach(PIN_SERVO);
  puerta.write(SERVO_CERRADO);

  bool pedirRegistro = false;
  unsigned long inicio = millis();
  while (millis() - inicio < 2000) {
    if (teclado.getKey() == TECLA_REGISTRO) pedirRegistro = true;
  }

  if (!memoriaInicializada() || cantidadUsuarios() < USUARIOS_INICIALES || pedirRegistro) {
    registrarUsuarios();
  }

  Serial.print(F("Usuarios en EEPROM: "));
  Serial.println(cantidadUsuarios());
  pantallaPrincipal();
}

void loop() {
  switch (estadoActual) {
    case ESPERANDO_RFID: atenderEsperaRFID();    break;
    case ESPERANDO_PIN:  atenderEsperaPIN();     break;
    case AUTORIZADO:     atenderPuertaAbierta(); break;
    case ALARMA:         atenderAlarma();        break;
    default:             pantallaPrincipal();    break;
  }
}
