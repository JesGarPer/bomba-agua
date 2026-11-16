// Bomba de agua controlada desde WhatsApp.
// NodeMCU (ESP8266) + modulo de relevador (que activa un contactor) + sensor de flujo tipo YF-S201.
//
// Cada INTERVALO_REPORTE_MS le manda al servicio si la bomba esta encendida, el flujo y el
// total de litros; en la respuesta recibe si la bomba debe estar encendida.
// Ademas sirve una pagina con botones ENCENDER / APAGAR en http://<IP de la placa>/ para
// probar sin WhatsApp; lo que se haga ahi se le avisa al servicio en el siguiente reporte.
// Las protecciones (tiempo maximo, funcionamiento en seco y perdida de conexion) se aplican
// AQUI, en la placa, para que funcionen aunque se caiga el internet o el servicio.
//
// Arduino IDE: placa "NodeMCU 1.0 (ESP-12E Module)" (paquete esp8266) y la libreria
// ArduinoJson version 7 (Herramientas > Gestor de librerias).
// El WiFi y las claves van en secretos.h (copia secretos.ejemplo.h y llenalo).

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "secretos.h"

// ---------- Configuracion ----------
// Modo prueba: un LED en lugar de la bomba. Desactiva la proteccion en seco (un LED no genera
// flujo) y la de perdida de conexion (para que los botones funcionen sin el servicio).
// Ponlo en false antes de conectar la bomba de verdad.
const bool MODO_PRUEBA = true;

const int  PIN_RELE  = D1;  // LED de prueba o relevador. GPIO5: no cambia de estado al arrancar la placa
const int  PIN_FLUJO = D2;  // GPIO4: admite interrupciones
const bool RELE_ACTIVO_EN_BAJO = false;  // LED: false. Modulo de relevador: casi siempre true

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
bool servicioOk = false;
bool wifiAvisado = false;
// Motivo del ultimo apagado automatico. Se manda al servicio hasta que responde; el servicio
// lo registra y deja la bomba apagada (para no reencender una bomba en seco).
String apagadoPor = "";
// Ultima orden dada con los botones de la pagina, pendiente de avisar al servicio.
String ordenLocal = "";

// Un solo cliente HTTPS para todo el programa: crearlo en cada reporte fragmenta la poca RAM del ESP8266.
BearSSL::WiFiClientSecure cliente;
ESP8266WebServer servidor(80);

void IRAM_ATTR alPulso() {
  pulsos++;
}

void ponerRele(bool on) {
  digitalWrite(PIN_RELE, (on != RELE_ACTIVO_EN_BAJO) ? HIGH : LOW);
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

void guardarTotal() {
  File f = LittleFS.open("/total", "w");
  if (!f) return;
  f.print(totalLitros, 3);
  f.close();
}

double leerTotal() {
  File f = LittleFS.open("/total", "r");
  if (!f) return 0;
  double t = f.readString().toDouble();
  f.close();
  return t;
}

void medir() {
  unsigned long ahora = millis();
  unsigned long dt = ahora - ultimaMedicion;
  if (dt < 1000) return;

  noInterrupts();
  uint32_t p = pulsos;
  pulsos = 0;
  interrupts();
  ultimaMedicion = ahora;

  float litros = p / PULSOS_POR_LITRO;
  totalLitros += litros;
  caudalLmin = litros * 60000.0 / dt;
  if (caudalLmin >= FLUJO_MINIMO_LMIN) ultimoFlujoOk = ahora;

  // Guardar el total en la memoria de la placa cada minuto (no cada segundo: desgasta la flash).
  if (ahora - ultimoGuardado >= 60000 && totalLitros - totalGuardado >= 0.1) {
    guardarTotal();
    totalGuardado = totalLitros;
    ultimoGuardado = ahora;
  }
}

void protecciones() {
  if (!encendida) return;
  unsigned long ahora = millis();
  if (ahora - encendidaDesde >= MAX_ENCENDIDA_MS) {
    apagarPor("tiempo");
  } else if (!MODO_PRUEBA && ahora - encendidaDesde >= GRACIA_ARRANQUE_MS && ahora - ultimoFlujoOk >= SECO_MS) {
    apagarPor("seco");
  } else if (!MODO_PRUEBA && ahora - ultimaRespuestaOk >= SIN_CONEXION_MS) {
    apagarPor("sin_conexion");
  }
}

void reportar() {
  if (WiFi.status() != WL_CONNECTED) return;

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
  if (ordenLocal.length()) doc["orden_local"] = ordenLocal;
  String cuerpo;
  serializeJson(doc, cuerpo);

  int codigo = http.POST(cuerpo);
  if (codigo == 200) {
    JsonDocument respuesta;
    if (!deserializeJson(respuesta, http.getString())) {
      ultimaRespuestaOk = millis();
      servicioOk = true;
      apagadoPor = "";  // el servicio ya registro el apagado automatico
      ordenLocal = "";  // y la orden de los botones
      bool pedirEncendida = respuesta["encender"] | false;
      if (pedirEncendida != encendida) {
        ponerRele(pedirEncendida);
        Serial.println(pedirEncendida ? "Orden de WhatsApp: encender" : "Orden de WhatsApp: apagar");
      }
    }
  } else {
    servicioOk = false;
    Serial.printf("El reporte fallo: HTTP %d (%s)\n", codigo, http.errorToString(codigo).c_str());
  }
  http.end();
}

// ---------- Pagina de pruebas (http://<IP de la placa>/) ----------

void paginaPrincipal() {
  String pagina = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="5">
  <title>Bomba de agua</title>
  <style>
    body { background: #151923; color: white; font-family: Arial; text-align: center; padding: 40px 10px; }
    button { padding: 20px 35px; font-size: 22px; border: none; border-radius: 12px; margin: 10px; cursor: pointer; }
    .on { background: #32cd72; }
    .off { background: #ff5252; }
    .estado { font-size: 20px; margin: 20px 0; }
    .nota { color: #9aa3b5; font-size: 14px; }
  </style>
</head>
<body>
  <h1>Bomba de agua</h1>
  <div class="estado">%ESTADO%</div>
  <form action="/on" method="post"><button class="on">ENCENDER</button></form>
  <form action="/off" method="post"><button class="off">APAGAR</button></form>
  <p class="nota">%DETALLE%</p>
</body>
</html>
)rawliteral";

  pagina.replace("%ESTADO%", encendida ? "&#128994; Encendida" : "&#9898; Apagada");
  String detalle = "Flujo: " + String(caudalLmin, 1) + " L/min &middot; Total: " + String(totalLitros, 1) + " L<br>";
  detalle += servicioOk ? "Conectado al servicio (WhatsApp)" : "Sin conexion con el servicio: solo funcionan estos botones";
  if (MODO_PRUEBA) detalle += "<br>MODO PRUEBA (LED)";
  pagina.replace("%DETALLE%", detalle);
  servidor.send(200, "text/html", pagina);
}

void volverAlInicio() {
  // Redirige a "/" para que recargar la pagina no repita la orden.
  servidor.sendHeader("Location", "/");
  servidor.send(303);
}

void botonEncender() {
  ponerRele(true);
  ordenLocal = "encender";
  Serial.println("Boton de la pagina: encender");
  volverAlInicio();
}

void botonApagar() {
  ponerRele(false);
  ordenLocal = "apagar";
  Serial.println("Boton de la pagina: apagar");
  volverAlInicio();
}

void setup() {
  Serial.begin(115200);

  // Primero el nivel de "apagado" y despues OUTPUT, para que el relevador no se active al arrancar.
  digitalWrite(PIN_RELE, RELE_ACTIVO_EN_BAJO ? HIGH : LOW);
  pinMode(PIN_RELE, OUTPUT);
  ponerRele(false);

  // La senal del sensor (5 V) llega por un divisor de voltaje a 3.3 V, ver README.
  pinMode(PIN_FLUJO, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FLUJO), alPulso, FALLING);

  LittleFS.begin();
  totalLitros = leerTotal();
  totalGuardado = totalLitros;

  cliente.setInsecure();  // no valida el certificado del servidor (ver README, "Seguridad")

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("\nConectando al WiFi...");

  servidor.on("/", paginaPrincipal);
  servidor.on("/on", botonEncender);
  servidor.on("/off", botonApagar);
  servidor.begin();

  ultimaMedicion = millis();
  ultimaRespuestaOk = millis();
}

void loop() {
  servidor.handleClient();
  medir();
  protecciones();

  if (WiFi.status() == WL_CONNECTED && !wifiAvisado) {
    wifiAvisado = true;
    Serial.print("WiFi conectado. Pagina de pruebas: http://");
    Serial.println(WiFi.localIP());
  } else if (WiFi.status() != WL_CONNECTED) {
    wifiAvisado = false;
  }

  if (millis() - ultimoReporte >= INTERVALO_REPORTE_MS) {
    ultimoReporte = millis();
    reportar();
  }
  delay(10);
}
