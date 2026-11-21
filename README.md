# Bomba de agua controlada desde WhatsApp

Enciende y apaga una bomba de agua y consulta su sensor de flujo desde WhatsApp, usando la
plataforma de bots (proyecto aparte) como interfaz.

```
 WhatsApp ──► Plataforma de bots (flow) ──http_request──► Este servicio (Railway)
                                                              ▲   │
                                          reporte cada 2 s   │   │ orden: encender / apagar
                                                              │   ▼
                           NodeMCU (ESP8266) + relevador + contactor + sensor de flujo YF-S201
```

| Carpeta / archivo | Qué es |
|---|---|
| `index.js` | Servicio (Node 20, sin dependencias). Guarda el estado y hace de puente |
| `firmware/bomba_esp8266/` | Programa de la NodeMCU (Arduino IDE) |
| `whatsapp/flujo.json` | Flow para importar en la plataforma de bots |

---

## 1. Materiales

| Pieza | Para qué | Nota |
|---|---|---|
| NodeMCU v2/v3 (ESP8266, ESP-12E) | Cerebro con WiFi | |
| Módulo de relevador de 1 canal, 5 V, con optoacoplador | Activa la bobina del contactor | La NodeMCU da señales de 3.3 V: que el módulo diga que funciona con 3.3 V (los de optoacoplador y activación en bajo casi siempre sirven) |
| **Contactor** con bobina de 127 V AC | Conecta y desconecta la bomba | De corriente igual o mayor a la de la bomba (ver su etiqueta). **No conectes la bomba directo al relevador del módulo**: el arranque del motor quema sus contactos |
| Sensor de flujo YF-S201 (o el que corresponda al diámetro del tubo) | Mide litros por minuto | El YF-S201 es para tubo de ½" y de 1 a 30 L/min |
| Fuente de 5 V (cargador USB de 2 A, o módulo HLK-PM01) | Alimenta la placa, el relevador y el sensor | |
| Resistencias de 10 kΩ y 20 kΩ | Divisor de voltaje de la señal del sensor (5 V → 3.3 V) | |
| Pastilla termomagnética (breaker) para la bomba | Protección | |
| Caja de plástico (gabinete) | Que nada quede expuesto | |

## 2. Conexiones

> ⚠️ **127 V puede matar.** La parte de corriente alterna (contactor, bomba, pastilla)
> debe instalarla un electricista. Trabaja siempre con la pastilla **abajo** y nunca toques el
> circuito mientras esté conectado.

**Baja tensión (5 V / 3.3 V):**

| Desde | Hacia |
|---|---|
| Fuente 5 V (+) | NodeMCU `VIN`, relevador `VCC`, sensor cable rojo |
| Fuente 5 V (−) | NodeMCU `G` (GND), relevador `GND`, sensor cable negro |
| NodeMCU `D1` | relevador `IN` |
| Sensor cable amarillo (señal) | resistencia 10 kΩ → `D2`; de `D2`, resistencia 20 kΩ → `G` (GND) |

No uses `D3`, `D4` ni `D8` para el relevador: cambian de estado al arrancar la placa y podrían
encender la bomba un instante. Mientras programas por USB puedes dejar la fuente desconectada.

**Alta tensión (127 V, electricista):**

```
Fase ── pastilla ──┬── contactor L1 ── contactor T1 ── bomba ── Neutro
                   │
                   └── relevador COM ── relevador NO ── bobina del contactor (A1)
                                                         bobina del contactor (A2) ── Neutro
```

El relevador del módulo solo mueve la bobina del contactor (poca corriente); el contactor es
el que lleva la corriente de la bomba.

## 3. Servicio en Railway

1. Sube esta carpeta a un repositorio propio de GitHub (por ejemplo `JesGarPer/bomba-agua`).
2. En Railway: **+ Add → GitHub Repo → bomba-agua**. Railway detecta Node y usa `npm start`.
3. **Settings → Networking → Generate Domain**. Anota la URL.
4. **Variables**:

| Variable | Valor |
|---|---|
| `DEVICE_KEY` | Texto largo al azar. Lo usa la placa |
| `API_KEY` | Otro texto largo al azar. Lo usa el bot |
| `AUTORIZADOS` | **Déjala vacía.** Quién tiene acceso se administra desde la plataforma (ver sección 5). Solo si se llena, el servicio filtra además por estos números (separados por coma) |
| `MINUTOS_DEFAULT` | Minutos que queda encendida si no se dice cuánto (por defecto `15`) |
| `MINUTOS_MAX` | Máximo que se puede pedir por WhatsApp (por defecto `60`) |

Para generar los textos al azar:
`node -e "console.log(require('crypto').randomBytes(24).toString('hex'))"`

Prueba: `https://<tu-dominio>/salud` debe responder `{"ok":true}`.

> El estado vive en memoria: si el servicio se reinicia, la bomba queda **apagada** hasta nueva
> orden (lo seguro). El total de litros no se pierde porque lo guarda la placa.

## 4. Programa de la NodeMCU

1. Instala **Arduino IDE**. En *Archivo → Preferencias → URLs adicionales de gestor de placas*
   agrega `https://arduino.esp8266.com/stable/package_esp8266com_index.json` y en el
   *Gestor de placas* instala **esp8266** (ESP8266 Community).
2. En el *Gestor de librerías* instala **ArduinoJson** (versión 7).
3. En `firmware/bomba_esp8266/`, copia `secretos.ejemplo.h` como `secretos.h` y llena
   `WIFI_SSID`, `WIFI_PASSWORD`, `SERVICIO_URL` (`https://<tu-dominio>/dispositivo/reporte`)
   y `DEVICE_KEY`. `secretos.h` no se sube a GitHub.
4. Abre `bomba_esp8266.ino`. Placa **NodeMCU 1.0 (ESP-12E Module)**, elige el puerto (COM) y
   dale **Subir**. Si Windows no reconoce la placa, instala el driver **CH340** o **CP2102**
   (viene impreso en el chip junto al USB).
5. Abre el *Monitor serie* a 115200: muestra la IP de la placa y cada orden que recibe.

La NodeMCU solo se conecta a redes WiFi de **2.4 GHz** (no 5 GHz).

**Página de pruebas:** desde un celular o computadora en la misma red WiFi, abre
`http://<IP de la placa>/`. Tiene botones **ENCENDER / APAGAR**, el estado y el flujo. Lo que
hagas ahí se le avisa al servicio, así que WhatsApp ve el mismo estado.

**Modo prueba (`MODO_PRUEBA = true`):** para probar con un LED en `D1` (con su resistencia de
220 Ω a GND) en lugar de la bomba. Desactiva la protección en seco (un LED no genera flujo) y la
de pérdida de conexión. **Antes de conectar la bomba:** `MODO_PRUEBA = false` y
`RELE_ACTIVO_EN_BAJO = true` (si tu módulo de relevador se activa con LOW).

Protecciones que aplica la propia placa (funcionan aunque no haya internet):

| Protección | Valor por defecto | Constante |
|---|---|---|
| Tiempo máximo encendida | 60 min | `MAX_ENCENDIDA_MS` |
| En seco: sin flujo después del arranque | 15 s de gracia + 10 s sin flujo | `GRACIA_ARRANQUE_MS`, `SECO_MS` |
| Sin conexión con el servicio | 30 s | `SIN_CONEXION_MS` |

Después de un apagado automático, la bomba **no se vuelve a encender sola**: hay que pedirlo
otra vez por WhatsApp, y el estado muestra el motivo.

**Calibrar el sensor:** llena un recipiente de volumen conocido (por ejemplo 10 L), compara con
el total medido y ajusta `PULSOS_POR_LITRO` (nuevo = actual × medido ÷ real).

## 5. Flow de WhatsApp

1. En la plataforma, **Datos → nueva colección "Autorizados"** con un campo `telefono`. Agrega un
   registro por cada número con acceso (10 dígitos, o con 52 / 521; da igual el formato). Para
   quitarle el acceso a alguien, borra su registro.
2. Abre `whatsapp/flujo.json` y en el nodo `inicio` cambia `base_url` (la URL del servicio) y
   `api_key` (la misma `API_KEY` de Railway).
3. **Flows → + Nuevo → JSON → pega el contenido → Importar**. En el nodo de Datos
   (`buscar_autorizados`) elige la colección **Autorizados**. **Guardar**, trigger `default` y
   **Publicar**.

El flujo revisa la tabla cada vez que alguien empieza la conversación. Si su número no está,
contesta "🚫 Tu número no tiene acceso al control de la bomba" y no muestra el menú. Si no se
eligió la colección o está vacía, nadie entra.

Lo que entiende el bot:

| Escribe | Hace |
|---|---|
| `1`, `encender`, `prende` | Enciende por `MINUTOS_DEFAULT` |
| `encender 10 min` | Enciende 10 minutos |
| `2`, `apagar`, `apágala` | Apaga |
| `3`, `estado`, `flujo` | Estado, flujo (L/min), total de litros y conexión del equipo |
| `menú` | Opciones |

## 6. Seguridad

- La placa y el bot usan claves distintas (`DEVICE_KEY`, `API_KEY`); con una sola no se puede
  hacer lo del otro.
- Solo los números de la colección **Autorizados** (Datos de la plataforma) pueden usar el menú.
  El servicio solo acepta órdenes con la `API_KEY`, es decir, solo del flujo.
- La placa no valida el certificado HTTPS del servidor (`setInsecure()`), por simplicidad. Para
  endurecerlo, cargar el certificado raíz con `cliente.setTrustAnchors(...)`.

## API del servicio

| Método y ruta | Clave | Uso |
|---|---|---|
| `GET /salud` | — | Verificación |
| `POST /dispositivo/reporte` | `x-device-key` | Placa: `{encendida, caudal_lmin, total_litros, apagado_por?}` → `{encender}` |
| `GET /estado` | `x-api-key` | Bot: `{ok, mensaje, conectado, encendida, caudal_lmin, total_litros}` |
| `POST /bomba` | `x-api-key` | Bot: `{accion: "encender"\|"apagar", telefono, minutos?}` → `{ok, mensaje}`. Espera hasta 6 s a que la placa confirme |
