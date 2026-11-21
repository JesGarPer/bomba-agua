# Bomba de agua controlada desde WhatsApp

Enciende y apaga una bomba de agua y consulta su sensor de flujo desde WhatsApp, usando la
plataforma de bots (proyecto aparte) como interfaz. Casi todo se configura desde la plataforma;
en el código solo quedan las claves y el botón BOOT.

```
 WhatsApp ──► Plataforma de bots (flow) ──http_request──► Este servicio (Railway)
                 │                                             ▲   │
     Datos: "Autorizados" (quién entra)        reporte cada 2 s │   │ orden + ajustes
            "Equipos" (pines, modo, protecciones)               │   ▼
                                           ESP32 + relevador + contactor + sensor de flujo YF-S201
```

| Carpeta / archivo | Qué es |
|---|---|
| `index.js` | Servicio (Node 20, sin dependencias). Guarda el estado y hace de puente |
| `firmware/bomba_esp32/` | Programa del ESP32 |
| `whatsapp/flujo.json` | Flow para importar en la plataforma de bots |

## Qué se configura dónde

| Qué | Dónde |
|---|---|
| Quién puede usar la bomba | Plataforma → Datos → **Autorizados** |
| Pines, modo prueba/normal, relevador activo en bajo, protecciones, calibración, minutos | Plataforma → Datos → **Equipos** |
| WiFi de la placa | Celular → red **Bomba-Config** (la placa sin WiFi no puede recibir nada de la plataforma) |
| Claves (`DEVICE_KEY`, `API_KEY`) y URL del servicio | `secretos.h` y variables de Railway |

---

## 1. Materiales

| Pieza | Para qué | Nota |
|---|---|---|
| ESP32 DevKit (ESP32-WROOM-32) | Cerebro con WiFi | |
| Módulo de relevador de 1 canal, 5 V, con optoacoplador | Activa la bobina del contactor | Que funcione con señal de 3.3 V |
| **Contactor** con bobina de 127 V AC | Conecta y desconecta la bomba | De corriente igual o mayor a la de la bomba. **No conectes la bomba directo al relevador del módulo**: el arranque del motor quema sus contactos |
| Sensor de flujo YF-S201 (o el del diámetro de tu tubo) | Mide litros por minuto | El YF-S201 es para tubo de ½" y de 1 a 30 L/min |
| Fuente de 5 V (cargador USB de 2 A, o módulo HLK-PM01) | Alimenta ESP32, relevador y sensor | |
| Resistencias de 10 kΩ y 20 kΩ | Divisor de voltaje de la señal del sensor (5 V → 3.3 V) | |
| Pastilla termomagnética para la bomba | Protección | |
| Caja de plástico | Que nada quede expuesto | |

## 2. Conexiones

> ⚠️ **127 V puede matar.** La parte de corriente alterna (contactor, bomba, pastilla) la debe
> instalar un electricista. Trabaja siempre con la pastilla **abajo**.

Pines por defecto (se cambian en Datos → Equipos):

| Desde | Hacia |
|---|---|
| Fuente 5 V (+) | ESP32 `VIN` (o `5V`), relevador `VCC`, sensor cable rojo |
| Fuente 5 V (−) | ESP32 `GND`, relevador `GND`, sensor cable negro |
| ESP32 `GPIO26` (`pin_rele`) | relevador `IN` (o LED de prueba con resistencia de 220 Ω a GND) |
| Sensor cable amarillo (señal) | resistencia 10 kΩ → `GPIO27` (`pin_flujo`); de `GPIO27`, resistencia 20 kΩ → `GND` |
| `GPIO2` (`pin_led`) | LED azul de la placa: encendido = bomba encendida |

Pines que **no** se pueden usar (la plataforma y la placa los rechazan): 6–11 (memoria flash),
0 y 12 (arranque), 1 y 3 (USB). 34, 35, 36 y 39 solo sirven para el sensor (son solo entrada).

**Alta tensión (electricista):**

```
Fase ── pastilla ──┬── contactor L1 ── contactor T1 ── bomba ── Neutro
                   └── relevador COM ── relevador NO ── bobina del contactor (A1)
                                                         bobina del contactor (A2) ── Neutro
```

## 3. Servicio en Railway

1. Repositorio en GitHub → Railway: **+ Add → GitHub Repo → bomba-agua**. Detecta Node y usa `npm start`.
2. **Settings → Networking → Generate Domain**.
3. **Variables**: `DEVICE_KEY` y `API_KEY` (textos largos al azar, distintos). `AUTORIZADOS` vacía.
   `MINUTOS_DEFAULT` / `MINUTOS_MAX` son el respaldo si Datos → Equipos no los trae.

Para generar los textos al azar: `node -e "console.log(require('crypto').randomBytes(24).toString('hex'))"`

Prueba: `https://<tu-dominio>/salud` → `{"ok":true}`.

> El estado vive en memoria: si el servicio se reinicia, la bomba queda **apagada** hasta nueva
> orden (lo seguro) y los ajustes se vuelven a mandar en la siguiente conversación. La placa
> guarda sus propios ajustes y el total de litros.

## 4. Programa del ESP32

En `firmware/bomba_esp32/`, copia `secretos.ejemplo.h` como `secretos.h` y llena
`PORTAL_PASSWORD`, `SERVICIO_URL` (`https://<tu-dominio>/dispositivo/reporte`) y `DEVICE_KEY`.
`secretos.h` no se sube a GitHub.

**Con Arduino IDE:** placa **esp32** (Espressif) en el Gestor de placas; librerías **ArduinoJson** 7
y **WiFiManager** (tzapu); placa **ESP32 Dev Module**, *Partition Scheme*: **Minimal SPIFFS**; **Subir**.

**Sin Arduino IDE (como se hace en esta computadora, donde Windows bloquea `arduino-cli`):**
se compila en Docker y se sube con `python -m esptool`:

```
docker run --rm -v "<ruta>/firmware/bomba_esp32:/src/bomba_esp32:ro" -v "<salida>:/out" debian:bookworm-slim bash -c '
  apt-get update -qq && apt-get install -y -qq curl ca-certificates python3
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/local/bin sh -s 1.6.0
  arduino-cli config init && arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  arduino-cli core update-index && arduino-cli core install esp32:esp32 && arduino-cli lib install ArduinoJson WiFiManager
  cp -r /src/bomba_esp32 /tmp/ && arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs --output-dir /out /tmp/bomba_esp32'
python -m pip install --user esptool
python -m esptool --chip esp32 --port COM8 write-flash 0x0 <salida>/bomba_esp32.ino.merged.bin
```

Si Windows no reconoce la placa: driver **CP210x** (Silicon Labs, viene por Windows Update) o
**CH340** (WCH), según el chip junto al USB.

**WiFi (sin reprogramar):** si la placa no encuentra su red (primera vez, otro módem u otra
contraseña), abre la red **Bomba-Config** (contraseña: `PORTAL_PASSWORD`). Conéctate desde el
celular; se abre una página (si no, `http://192.168.4.1`) → **Configure WiFi** → elige la red,
escribe la contraseña → **Save**. Solo redes de 2.4 GHz. Para cambiar de red a propósito: botón
**BOOT** 5 segundos, o **Cambiar WiFi** en la página de pruebas. Si nadie configura en 3 minutos,
se reinicia y reintenta la red guardada. Sin WiFi la bomba queda apagada.

**Página de pruebas:** en la misma red, `http://<IP de la placa>/` (la IP sale en el Monitor
serie a 115200). Botones **ENCENDER / APAGAR**, estado, flujo, modo y pines. Lo que se haga ahí
lo ve también WhatsApp.

## 5. En la plataforma

1. **Datos → colección `Autorizados`** con un campo `telefono`. Un registro por número con acceso
   (10 dígitos, o con 52 / 521; da igual el formato). Borrar el registro quita el acceso.
2. **Datos → colección `Equipos`** con **un solo registro** y estos campos (todos opcionales:
   vacío = se conserva el valor actual de la placa):

| Campo | Valores | Por defecto en la placa |
|---|---|---|
| `modo` | `prueba` (LED, sin protección en seco ni por conexión) / `normal` | `normal` |
| `rele_activo_en_bajo` | `si` (módulo de relevador típico) / `no` (LED) | `si` |
| `pin_rele` | GPIO del relevador o LED | `26` |
| `pin_flujo` | GPIO de la señal del sensor | `27` |
| `pin_led` | GPIO del LED de estado, o `-1` para ninguno | `2` |
| `minutos_default` | Minutos al escribir "encender" sin decir cuántos (1–240) | `15` |
| `minutos_max` | Máximo que se puede pedir por WhatsApp (1–240) | `60` |
| `max_encendida_min` | Tope absoluto en la placa, aunque no haya internet (1–240) | `60` |
| `gracia_seg` | Segundos para que la bomba se cebe antes de exigir flujo | `15` |
| `seco_seg` | Segundos sin flujo para apagar por "en seco" (`0` = desactivado) | `10` |
| `sin_conexion_seg` | Segundos sin servicio para apagar (10–3600) | `30` |
| `pulsos_por_litro` | Calibración del sensor | `450` |

3. **Flows** → abre el flujo → **JSON** → pega `whatsapp/flujo.json` (con `base_url` y `api_key`
   del nodo `inicio` llenos) → **Importar**. Elige la colección en los dos nodos de Datos:
   `buscar_autorizados` → **Autorizados**, `leer_equipo` → **Equipos**. **Guardar** → **Publicado**.

Los ajustes de **Equipos** se mandan al servicio cada vez que alguien autorizado empieza una
conversación ("hola"); la placa los aplica en su siguiente reporte (unos 2 s) y los guarda. Si
cambian los pines, la placa se reinicia sola para aplicarlos. Si la placa rechaza un ajuste (por
ejemplo un pin que no se puede usar), lo dice en el estado (**3**).

Lo que entiende el bot:

| Escribe | Hace |
|---|---|
| `1`, `encender`, `prende` | Enciende por `minutos_default` |
| `encender 10 min` | Enciende 10 minutos |
| `2`, `apagar`, `apágala` | Apaga |
| `3`, `estado`, `flujo` | Estado, flujo, total, conexión, modo y avisos |
| `menú` | Opciones |

WhatsApp no deja que el bot escriba primero: la persona inicia la conversación.

**Calibrar el sensor:** llena un recipiente de volumen conocido, compara con el total medido y
ajusta `pulsos_por_litro` en Equipos (nuevo = actual × medido ÷ real).

## 6. Seguridad

- La placa y el bot usan claves distintas (`DEVICE_KEY`, `API_KEY`).
- Solo los números de **Autorizados** pueden usar el menú; el servicio solo acepta órdenes con
  la `API_KEY`, es decir, solo del flujo.
- Pines peligrosos rechazados en el servicio y otra vez en la placa.
- La placa no valida el certificado HTTPS del servidor (`setInsecure()`), por simplicidad.

## API del servicio

| Método y ruta | Clave | Uso |
|---|---|---|
| `GET /salud` | — | Verificación |
| `POST /dispositivo/reporte` | `x-device-key` | Placa: `{encendida, caudal_lmin, total_litros, modo, config_version, pines, apagado_por?, orden_local?, config_error?}` → `{encender, config?}` |
| `GET /estado` | `x-api-key` | Bot: `{ok, mensaje, conectado, encendida, caudal_lmin, total_litros}` |
| `POST /bomba` | `x-api-key` | Bot: `{accion: "encender"\|"apagar", telefono, minutos?}` → `{ok, mensaje}`. Espera hasta 6 s la confirmación |
| `POST /config` | `x-api-key` | Bot: ajustes de Datos → Equipos → `{ok, cambio, config}` |
