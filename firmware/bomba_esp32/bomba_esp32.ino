// Bomba de agua controlada desde WhatsApp.
// ESP32 + modulo de relevador (que activa un contactor) + sensor de flujo tipo YF-S201.
//
// Cada INTERVALO_REPORTE_MS le manda al servicio si la bomba esta encendida, el flujo y el
// total de litros; en la respuesta recibe si la bomba debe estar encendida y, cuando cambian,
// los ajustes que se administran en la plataforma (Datos > "Equipos"): modo prueba/normal,
// relevador activo en bajo, tiempo maximo, protecciones y calibracion. Los ajustes se guardan
// en la placa, asi que se conservan aunque se reinicie o se caiga el servicio.
// Las protecciones se aplican AQUI, en la placa, para que funcionen aunque no haya internet.
//
// WiFi: la placa se conecta a la red que tenga guardada. Si no puede (primera vez, otro modem,
// otra contrasena) abre su propia red "Bomba-Config": desde el celular se elige el WiFi y se
// escribe la contrasena. Para cambiar de red a proposito: boton BOOT 5 s, o "Cambiar WiFi" en
// la pagina de pruebas (http://<IP de la placa>/).
//
// Arduino IDE: placa "ESP32 Dev Module" (paquete esp32 de Espressif) y las librerias
// ArduinoJson version 7 y WiFiManager (tzapu). Las claves van en secretos.h (copia
// secretos.ejemplo.h y llenalo).

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "secretos.h"

// Unico pin fijo: el boton BOOT de la placa, que tiene que funcionar aunque no haya ajustes.
// Los demas pines (relevador, sensor, LED) se configuran desde la plataforma.
const int PIN_BOTON_WIFI = 0;   // boton BOOT: 5 s presionado = olvidar el WiFi

const char* RED_CONFIG = "Bomba-Config";
const unsigned long PORTAL_TIMEOUT_S = 180;  // sin configurar en 3 min: reinicia y reintenta la red guardada
const unsigned long INTERVALO_REPORTE_MS = 2000;
const float FLUJO_MINIMO_LMIN = 0.5;

// ---------- Ajustes (vienen de la plataforma; estos son los valores si nunca han llegado) ----------
// Por defecto: modo normal (con protecciones) y relevador activo en bajo, que es lo seguro para
// la mayoria de los modulos de relevador.
struct Ajustes {
  String version = "";
  bool modoPrueba = false;          // prueba: sin proteccion en seco ni por perdida de conexion
  bool releActivoEnBajo = true;
  unsigned long maxEncendidaMs = 60UL * 60UL * 1000UL;
  unsigned long graciaMs = 15000;    // tiempo para que la bomba se cebe antes de exigir flujo
  unsigned long secoMs = 10000;      // encendida sin flujo este tiempo = en seco (0 = desactivado)
  unsigned long sinConexionMs = 30000;
  float pulsosPorLitro = 450.0;      // YF-S201: ~450 pulsos por litro
  int pinRele = 26;                  // relevador, o LED de prueba
  int pinFlujo = 27;                 // senal del sensor de flujo (por divisor de voltaje a 3.3 V)
  int pinLed = 2;                    // LED que muestra el estado (2 = el azul de la placa); -1 = ninguno
} ajustes;

// Pines que se pueden usar sin riesgo. Fuera quedan: 6-11 (memoria flash de la placa: usarlos
// la bloquea), 0 y 12 (definen como arranca la placa), 1 y 3 (USB/serial) y 34-39 (solo entrada).
const int PINES_SALIDA[] = {2, 4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
const int PINES_SOLO_ENTRADA[] = {34, 35, 36, 39};

bool pinEnLista(int p, const int* lista, size_t n) {
  for (size_t i = 0; i < n; i++) if (lista[i] == p) return true;
  return false;
}
bool pinSalidaValido(int p) { return pinEnLista(p, PINES_SALIDA, sizeof(PINES_SALIDA) / sizeof(int)); }
bool pinEntradaValido(int p) { return pinSalidaValido(p) || pinEnLista(p, PINES_SOLO_ENTRADA, sizeof(PINES_SOLO_ENTRADA) / sizeof(int)); }

// Error de los ultimos ajustes recibidos (se le avisa al servicio para mostrarlo en WhatsApp).
String errorAjustes = "";

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
bool servicioOk = false;
// Motivo del ultimo apagado automatico. Se manda al servicio hasta que responde; el servicio
// lo registra y deja la bomba apagada (para no reencender una bomba en seco).
String apagadoPor = "";
// Ultima orden dada con los botones de la pagina, pendiente de avisar al servicio.
String ordenLocal = "";

WiFiClientSecure cliente;
WebServer servidor(80);
Preferences prefs;

void IRAM_ATTR alPulso() {
  portENTER_CRITICAL_ISR(&muxPulsos);
  pulsos = pulsos + 1;
  portEXIT_CRITICAL_ISR(&muxPulsos);
}

// ---------- Bomba ----------

void escribirRele() {
  digitalWrite(ajustes.pinRele, (encendida != ajustes.releActivoEnBajo) ? HIGH : LOW);
  if (ajustes.pinLed >= 0) digitalWrite(ajustes.pinLed, encendida ? HIGH : LOW);
}

void ponerRele(bool on) {
  if (on && !encendida) {
    encendidaDesde = millis();
    ultimoFlujoOk = millis();
  }
  encendida = on;
  escribirRele();
}

void apagarPor(const char* motivo) {
  if (!encendida) return;
  ponerRele(false);
  apagadoPor = motivo;
  Serial.printf("Apagado automatico: %s\n", motivo);
}

// ---------- Ajustes guardados ----------

void cargarAjustes() {
  ajustes.version          = prefs.getString("version", ajustes.version);
  ajustes.modoPrueba       = prefs.getBool("prueba", ajustes.modoPrueba);
  ajustes.releActivoEnBajo = prefs.getBool("releBajo", ajustes.releActivoEnBajo);
  ajustes.maxEncendidaMs   = prefs.getULong("maxMs", ajustes.maxEncendidaMs);
  ajustes.graciaMs         = prefs.getULong("graciaMs", ajustes.graciaMs);
  ajustes.secoMs           = prefs.getULong("secoMs", ajustes.secoMs);
  ajustes.sinConexionMs    = prefs.getULong("sinConMs", ajustes.sinConexionMs);
  ajustes.pulsosPorLitro   = prefs.getFloat("pulsosL", ajustes.pulsosPorLitro);
  ajustes.pinRele          = prefs.getInt("pinRele", ajustes.pinRele);
  ajustes.pinFlujo         = prefs.getInt("pinFlujo", ajustes.pinFlujo);
  ajustes.pinLed           = prefs.getInt("pinLed", ajustes.pinLed);
}

void guardarAjustes() {
  prefs.putString("version", ajustes.version);
  prefs.putBool("prueba", ajustes.modoPrueba);
  prefs.putBool("releBajo", ajustes.releActivoEnBajo);
  prefs.putULong("maxMs", ajustes.maxEncendidaMs);
  prefs.putULong("graciaMs", ajustes.graciaMs);
  prefs.putULong("secoMs", ajustes.secoMs);
  prefs.putULong("sinConMs", ajustes.sinConexionMs);
  prefs.putFloat("pulsosL", ajustes.pulsosPorLitro);
  prefs.putInt("pinRele", ajustes.pinRele);
  prefs.putInt("pinFlujo", ajustes.pinFlujo);
  prefs.putInt("pinLed", ajustes.pinLed);
}

// Revisa los pines nuevos. Devuelve "" si sirven, o el motivo si no (y no se aplican).
String revisarPines(int rele, int flujo, int led) {
  if (!pinSalidaValido(rele)) return "pin_rele " + String(rele) + " no se puede usar como salida";
  if (!pinEntradaValido(flujo)) return "pin_flujo " + String(flujo) + " no se puede usar";
  if (led != -1 && !pinSalidaValido(led)) return "pin_led " + String(led) + " no se puede usar como salida";
  if (rele == flujo || rele == led || (led != -1 && flujo == led)) return "hay pines repetidos";
  return "";
}

// Aplica los ajustes que mando el servicio. Solo cambia los campos que vienen.
void aplicarAjustes(JsonObject c) {
  int rele = c["pin_rele"].is<int>() ? c["pin_rele"].as<int>() : ajustes.pinRele;
  int flujo = c["pin_flujo"].is<int>() ? c["pin_flujo"].as<int>() : ajustes.pinFlujo;
  int led = c["pin_led"].is<int>() ? c["pin_led"].as<int>() : ajustes.pinLed;
  errorAjustes = revisarPines(rele, flujo, led);
  bool cambianPines = errorAjustes.length() == 0 && (rele != ajustes.pinRele || flujo != ajustes.pinFlujo || led != ajustes.pinLed);
  if (errorAjustes.length()) {
    Serial.printf("Pines rechazados: %s. Se conservan los actuales.\n", errorAjustes.c_str());
  } else {
    ajustes.pinRele = rele;
    ajustes.pinFlujo = flujo;
    ajustes.pinLed = led;
  }

  if (c["modo"].is<const char*>()) ajustes.modoPrueba = String(c["modo"].as<const char*>()) == "prueba";
  if (c["rele_activo_en_bajo"].is<bool>()) ajustes.releActivoEnBajo = c["rele_activo_en_bajo"].as<bool>();
  if (c["max_encendida_min"].is<float>()) ajustes.maxEncendidaMs = (unsigned long)(c["max_encendida_min"].as<float>() * 60000);
  if (c["gracia_seg"].is<float>()) ajustes.graciaMs = (unsigned long)(c["gracia_seg"].as<float>() * 1000);
  if (c["seco_seg"].is<float>()) ajustes.secoMs = (unsigned long)(c["seco_seg"].as<float>() * 1000);
  if (c["sin_conexion_seg"].is<float>()) ajustes.sinConexionMs = (unsigned long)(c["sin_conexion_seg"].as<float>() * 1000);
  if (c["pulsos_por_litro"].is<float>()) ajustes.pulsosPorLitro = c["pulsos_por_litro"].as<float>();
  ajustes.version = c["version"] | "";
  guardarAjustes();
  Serial.printf("Ajustes nuevos (version %s): modo %s, relevador en GPIO%d activo en %s, sensor GPIO%d, LED GPIO%d, max %lu min, seco %lu s\n",
                ajustes.version.c_str(), ajustes.modoPrueba ? "prueba" : "normal", ajustes.pinRele,
                ajustes.releActivoEnBajo ? "bajo" : "alto", ajustes.pinFlujo, ajustes.pinLed,
                ajustes.maxEncendidaMs / 60000, ajustes.secoMs / 1000);
  if (cambianPines) {
    // Cambiar pines en marcha deja el anterior en un estado incierto: se apaga todo y se
    // reinicia la placa, que arranca ya con los pines nuevos.
    Serial.println("Cambiaron los pines: reiniciando la placa para aplicarlos");
    ponerRele(false);
    delay(300);
    ESP.restart();
  }
  escribirRele();  // por si cambio la polaridad del relevador
}

// ---------- Medicion y protecciones ----------

void medir() {
  unsigned long ahora = millis();
  unsigned long dt = ahora - ultimaMedicion;
  if (dt < 1000) return;

  portENTER_CRITICAL(&muxPulsos);
  uint32_t p = pulsos;
  pulsos = 0;
  portEXIT_CRITICAL(&muxPulsos);
  ultimaMedicion = ahora;

  float litros = p / ajustes.pulsosPorLitro;
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
  if (ahora - encendidaDesde >= ajustes.maxEncendidaMs) {
    apagarPor("tiempo");
  } else if (!ajustes.modoPrueba && ajustes.secoMs > 0 && ahora - encendidaDesde >= ajustes.graciaMs &&
             ahora - ultimoFlujoOk >= ajustes.secoMs) {
    apagarPor("seco");
  } else if (!ajustes.modoPrueba && ahora - ultimaRespuestaOk >= ajustes.sinConexionMs) {
    apagarPor("sin_conexion");
  }
}

// ---------- Comunicacion con el servicio ----------

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
  doc["modo"] = ajustes.modoPrueba ? "prueba" : "normal";
  doc["config_version"] = ajustes.version;
  if (apagadoPor.length()) doc["apagado_por"] = apagadoPor;
  if (ordenLocal.length()) doc["orden_local"] = ordenLocal;
  if (errorAjustes.length()) doc["config_error"] = errorAjustes;
  JsonObject pines = doc["pines"].to<JsonObject>();
  pines["rele"] = ajustes.pinRele;
  pines["flujo"] = ajustes.pinFlujo;
  pines["led"] = ajustes.pinLed;
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
      if (respuesta["config"].is<JsonObject>()) aplicarAjustes(respuesta["config"].as<JsonObject>());
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

// ---------- WiFi ----------

// Conecta a la red guardada; si no puede, abre la red RED_CONFIG con la pagina para elegir el
// WiFi. Bloquea hasta conectar o hasta PORTAL_TIMEOUT_S; la bomba ya esta apagada en ese momento.
void conectarWiFi() {
  WiFiManager wm;
  wm.setTitle("Bomba de agua");
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  Serial.println("\nConectando al WiFi guardado...");
  if (!wm.autoConnect(RED_CONFIG, PORTAL_PASSWORD)) {
    // Nadie configuro a tiempo (o el modem esta apagado): reiniciar y volver a intentar.
    Serial.println("Sin WiFi: reiniciando para volver a intentar");
    ESP.restart();
  }
  WiFi.setAutoReconnect(true);
  Serial.print("WiFi conectado a ");
  Serial.print(WiFi.SSID());
  Serial.print(". Pagina de pruebas: http://");
  Serial.println(WiFi.localIP());
}

void olvidarWiFi() {
  Serial.println("Borrando el WiFi guardado; al reiniciar se abre la red Bomba-Config");
  ponerRele(false);
  WiFiManager wm;
  wm.resetSettings();
  delay(500);
  ESP.restart();
}

void revisarBotonWiFi() {
  static unsigned long presionadoDesde = 0;
  if (digitalRead(PIN_BOTON_WIFI) == LOW) {
    if (presionadoDesde == 0) presionadoDesde = millis();
    if (millis() - presionadoDesde >= 5000) olvidarWiFi();
  } else {
    presionadoDesde = 0;
  }
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
  <form action="/wifi" method="post" onsubmit="return confirm('La placa olvidara el WiFi actual y abrira la red Bomba-Config para elegir otro. ¿Continuar?')">
    <button style="background:#2b3242;color:#9aa3b5;font-size:14px;padding:10px 18px">Cambiar WiFi</button>
  </form>
</body>
</html>
)rawliteral";

  pagina.replace("%ESTADO%", encendida ? "&#128994; Encendida" : "&#9898; Apagada");
  String detalle = "Flujo: " + String(caudalLmin, 1) + " L/min &middot; Total: " + String(totalLitros, 1) + " L<br>";
  detalle += "WiFi: " + WiFi.SSID() + "<br>";
  detalle += servicioOk ? "Conectado al servicio (WhatsApp)" : "Sin conexion con el servicio: solo funcionan estos botones";
  detalle += "<br>Modo: " + String(ajustes.modoPrueba ? "PRUEBA (sin proteccion en seco)" : "normal");
  detalle += "<br>Pines: relevador GPIO" + String(ajustes.pinRele) + " &middot; sensor GPIO" + String(ajustes.pinFlujo) +
             " &middot; LED " + (ajustes.pinLed >= 0 ? "GPIO" + String(ajustes.pinLed) : String("ninguno"));
  if (errorAjustes.length()) detalle += "<br>Ajustes rechazados: " + errorAjustes;
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

void botonCambiarWiFi() {
  servidor.send(200, "text/html; charset=utf-8",
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<body style='background:#151923;color:white;font-family:Arial;text-align:center;padding:40px 10px'>"
                "<h2>La placa se esta reiniciando</h2>"
                "<p>Conectate desde el celular a la red <b>Bomba-Config</b> y elige el WiFi nuevo.</p></body>");
  delay(1000);  // que la respuesta alcance a salir antes de reiniciar
  olvidarWiFi();
}

// ---------- Arranque ----------

void setup() {
  Serial.begin(115200);

  prefs.begin("bomba", false);
  cargarAjustes();
  totalLitros = prefs.getDouble("total", 0);
  totalGuardado = totalLitros;

  // Por si quedaron guardados pines invalidos (no deberia pasar): volver a los de fabrica.
  if (revisarPines(ajustes.pinRele, ajustes.pinFlujo, ajustes.pinLed).length()) {
    ajustes.pinRele = 26;
    ajustes.pinFlujo = 27;
    ajustes.pinLed = 2;
  }
  Serial.printf("\nPines: relevador GPIO%d, sensor GPIO%d, LED GPIO%d\n", ajustes.pinRele, ajustes.pinFlujo, ajustes.pinLed);

  // Primero el nivel de "apagado" y despues OUTPUT, para que el relevador no se active al arrancar.
  digitalWrite(ajustes.pinRele, ajustes.releActivoEnBajo ? HIGH : LOW);
  pinMode(ajustes.pinRele, OUTPUT);
  if (ajustes.pinLed >= 0) pinMode(ajustes.pinLed, OUTPUT);
  ponerRele(false);

  pinMode(ajustes.pinFlujo, INPUT);
  attachInterrupt(digitalPinToInterrupt(ajustes.pinFlujo), alPulso, FALLING);
  pinMode(PIN_BOTON_WIFI, INPUT_PULLUP);

  cliente.setInsecure();  // no valida el certificado del servidor (ver README, "Seguridad")
  conectarWiFi();

  servidor.on("/", paginaPrincipal);
  servidor.on("/on", botonEncender);
  servidor.on("/off", botonApagar);
  servidor.on("/wifi", HTTP_POST, botonCambiarWiFi);
  servidor.begin();

  ultimaMedicion = millis();
  ultimaRespuestaOk = millis();
}

void loop() {
  servidor.handleClient();
  revisarBotonWiFi();
  medir();
  protecciones();
  if (millis() - ultimoReporte >= INTERVALO_REPORTE_MS) {
    ultimoReporte = millis();
    reportar();
  }
  delay(10);
}
