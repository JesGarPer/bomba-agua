// Bomba de agua controlada desde WhatsApp.
// ESP32 + modulo de relevador (que activa un contactor) + sensor de flujo tipo YF-S201.
//
// Cada INTERVALO_REPORTE_MS le manda al servicio si la bomba esta encendida, el flujo y el
// total de litros; en la respuesta recibe si la bomba debe estar encendida.
// Las protecciones (tiempo maximo, funcionamiento en seco y perdida de conexion) se aplican
// AQUI, en el equipo, para que funcionen aunque se caiga el internet o el servicio.
//
// Arduino IDE: placa "ESP32 Dev Module" (paquete esp32 de Espressif) y la libreria
// ArduinoJson version 7 (Herramientas > Gestor de librerias).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ---------- Configuracion: cambia esto ----------
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_CONTRASENA_WIFI";
const char* SERVICIO_URL  = "https://TU-SERVICIO.up.railway.app/dispositivo/reporte";
const char* DEVICE_KEY    = "LA_MISMA_DEVICE_KEY_DEL_SERVICIO";

const int  PIN_RELE  = 26;
const int  PIN_FLUJO = 27;
const int  PIN_LED   = 2;   // LED azul de la placa: encendido = bomba encendida
const bool RELE_ACTIVO_EN_BAJO = true;  // la mayoria de los modulos de relevador se activan con LOW

// YF-S201: ~450 pulsos por litro (frecuencia = 7.5 x L/min). Calibrar llenando un balde de volumen conocido.
const float PULSOS_POR_LITRO = 450.0;

const unsigned long INTERVALO_REPORTE_MS = 2000;
const unsigned long MAX_ENCENDIDA_MS     = 60UL * 60UL * 1000UL;  // tope absoluto: 60 min
const unsigned long GRACIA_ARRANQUE_MS   = 15000;  // tiempo para que la bomba se cebe antes de exigir flujo
const unsigned long SECO_MS              = 10000;  // encendida sin flujo este tiempo = en seco
const float         FLUJO_MINIMO_LMIN    = 0.5;
const unsigned long SIN_CONEXION_MS      = 30000;  // sin respuesta del servicio este tiempo = apagar

// ---------- Estado ----------
volatile uint32_t pulsos = 0;
portMUX_TYPE muxPulsos = portMUX_INITIALIZER_UNLOCKED;

bool encendida = false;
unsigned long encendidaDesde = 0;
unsigned long ultimoFlujoOk = 0;
unsigned long ultimaRespuestaOk = 0;
unsigned long ultimoReporte = 0;
unsigned long ultimaMedicion = 0;
unsigned long ultimoGuardado = 0;
float caudalLmin = 0;
double totalLitros = 0;
double totalGuardado = 0;
// Motivo del ultimo apagado automatico. Se manda al servicio hasta que responde, y mientras
// tanto se ignora cualquier orden de encender (para no reencender una bomba en seco).
String apagadoPor = "";
Preferences prefs;

void IRAM_ATTR alPulso() {
  portENTER_CRITICAL_ISR(&muxPulsos);
  pulsos++;
  portEXIT_CRITICAL_ISR(&muxPulsos);
}

void ponerRele(bool on) {
  digitalWrite(PIN_RELE, (on != RELE_ACTIVO_EN_BAJO) ? HIGH : LOW);
  digitalWrite(PIN_LED, on ? HIGH : LOW);
  if (on && !encendida) {
    encendidaDesde = millis();
    ultimoFlujoOk = millis();
  }
  encendida = on;
}

void apagarPor(const char* motivo) {
  if (!encendida) return;
  ponerRele(false);
  apagadoPor = motivo;
  Serial.printf("Apagado automatico: %s\n", motivo);
}

void medir() {
  unsigned long ahora = millis();
  unsigned long dt = ahora - ultimaMedicion;
  if (dt < 1000) return;

  portENTER_CRITICAL(&muxPulsos);
  uint32_t p = pulsos;
  pulsos = 0;
  portEXIT_CRITICAL(&muxPulsos);
  ultimaMedicion = ahora;

  float litros = p / PULSOS_POR_LITRO;
  totalLitros += litros;
  caudalLmin = litros * 60000.0 / dt;
  if (caudalLmin >= FLUJO_MINIMO_LMIN) ultimoFlujoOk = ahora;

  // Guardar el total en la memoria de la placa cada minuto (no cada segundo: desgasta la flash).
  if (ahora - ultimoGuardado >= 60000 && totalLitros - totalGuardado >= 0.1) {
    prefs.putDouble("total", totalLitros);
    totalGuardado = totalLitros;
    ultimoGuardado = ahora;
  }
}

void protecciones() {
  if (!encendida) return;
  unsigned long ahora = millis();
  if (ahora - encendidaDesde >= MAX_ENCENDIDA_MS) {
    apagarPor("tiempo");
  } else if (ahora - encendidaDesde >= GRACIA_ARRANQUE_MS && ahora - ultimoFlujoOk >= SECO_MS) {
    apagarPor("seco");
  } else if (ahora - ultimaRespuestaOk >= SIN_CONEXION_MS) {
    apagarPor("sin_conexion");
  }
}

void reportar() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure cliente;
  cliente.setInsecure();  // no valida el certificado del servidor (ver README, "Seguridad")
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(cliente, SERVICIO_URL)) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", DEVICE_KEY);

  JsonDocument doc;
  doc["encendida"] = encendida;
  doc["caudal_lmin"] = caudalLmin;
  doc["total_litros"] = totalLitros;
  if (apagadoPor.length()) doc["apagado_por"] = apagadoPor;
  String cuerpo;
  serializeJson(doc, cuerpo);

  int codigo = http.POST(cuerpo);
  if (codigo == 200) {
    JsonDocument respuesta;
    if (!deserializeJson(respuesta, http.getString())) {
      ultimaRespuestaOk = millis();
      apagadoPor = "";  // el servicio ya registro el apagado automatico
      bool pedirEncendida = respuesta["encender"] | false;
      if (pedirEncendida != encendida) {
        ponerRele(pedirEncendida);
        Serial.println(pedirEncendida ? "Orden: encender" : "Orden: apagar");
      }
    }
  } else {
    Serial.printf("El reporte fallo: HTTP %d\n", codigo);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);

  // Primero el nivel de "apagado" y despues OUTPUT, para que el relevador no se active al arrancar.
  digitalWrite(PIN_RELE, RELE_ACTIVO_EN_BAJO ? HIGH : LOW);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  ponerRele(false);

  // La senal del sensor (5 V) llega por un divisor de voltaje a 3.3 V, ver README.
  pinMode(PIN_FLUJO, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLUJO), alPulso, FALLING);

  prefs.begin("bomba", false);
  totalLitros = prefs.getDouble("total", 0);
  totalGuardado = totalLitros;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Conectando al WiFi...");

  ultimaMedicion = millis();
  ultimaRespuestaOk = millis();
}

void loop() {
  medir();
  protecciones();
  if (millis() - ultimoReporte >= INTERVALO_REPORTE_MS) {
    ultimoReporte = millis();
    reportar();
  }
  delay(10);
}
