// Servicio de la bomba de agua: puente entre el ESP32 (reporta lecturas y recibe la orden
// de encender/apagar) y el bot de WhatsApp (consulta y manda ordenes con el nodo
// http_request de la plataforma). Sin dependencias: solo Node 20.
//
// Las protecciones fuertes (tiempo maximo, funcionamiento en seco, perdida de conexion)
// viven en el firmware del ESP32, para que funcionen aunque se caiga este servicio.
// Aqui solo se lleva el apagado programado ("enciende 10 min") y quien puede ordenar.

const http = require("node:http");
const crypto = require("node:crypto");

const PORT = Number(process.env.PORT || 3000);
const DEVICE_KEY = process.env.DEVICE_KEY || "";
const API_KEY = process.env.API_KEY || "";
// Numeros que pueden encender/apagar, separados por coma. Vacio = cualquiera (solo para pruebas).
const AUTORIZADOS = (process.env.AUTORIZADOS || "").split(",").map(ultimosDiez).filter(Boolean);
const MINUTOS_DEFAULT = Number(process.env.MINUTOS_DEFAULT || 15);
const MINUTOS_MAX = Number(process.env.MINUTOS_MAX || 60);
const ZONA = process.env.ZONA_HORARIA || "America/Mexico_City";
// Si el ESP32 no reporta en este tiempo se considera desconectado (reporta cada ~2 s).
const DESCONECTADO_MS = 15000;
// Cuanto espera POST /bomba a que el ESP32 confirme la orden (el http_request del bot corta a los 8 s).
const ESPERA_CONFIRMACION_MS = 6000;

if (!DEVICE_KEY || !API_KEY) {
  console.error("Faltan las variables DEVICE_KEY y/o API_KEY");
  process.exit(1);
}

const estado = {
  deseada: false, // lo que pidio WhatsApp
  apagarEn: null, // epoch ms del apagado programado
  reporte: null, // ultimo reporte del ESP32: { encendida, caudal_lmin, total_litros }
  vistoEn: null, // epoch ms del ultimo reporte
  ultimoAviso: null, // { texto, en } del ultimo apagado automatico
};

const MOTIVOS = {
  seco: "no detectó flujo de agua (posible funcionamiento en seco)",
  tiempo: "llegó al tiempo máximo de encendido",
  sin_conexion: "el equipo perdió la conexión a internet",
};

function ultimosDiez(telefono) {
  // WhatsApp manda los numeros de Mexico como 521XXXXXXXXXX; se comparan los ultimos 10 digitos.
  return String(telefono || "").replace(/\D/g, "").slice(-10);
}

function claveValida(recibida, esperada) {
  const a = Buffer.from(String(recibida || ""));
  const b = Buffer.from(esperada);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

function conectado() {
  return estado.vistoEn !== null && Date.now() - estado.vistoEn < DESCONECTADO_MS;
}

function aplicarApagadoProgramado() {
  if (estado.deseada && estado.apagarEn !== null && Date.now() >= estado.apagarEn) {
    estado.deseada = false;
    estado.apagarEn = null;
    estado.ultimoAviso = { texto: "Se apagó al terminar el tiempo programado", en: Date.now() };
  }
}

function numero(n) {
  return Number(n || 0).toLocaleString("es-MX", { maximumFractionDigits: 1 });
}

function hora(ms) {
  return new Date(ms).toLocaleTimeString("es-MX", { timeZone: ZONA, hour: "2-digit", minute: "2-digit" });
}

function hace(ms) {
  const seg = Math.round((Date.now() - ms) / 1000);
  if (seg < 60) return `hace ${seg} s`;
  if (seg < 3600) return `hace ${Math.round(seg / 60)} min`;
  return `a las ${hora(ms)}`;
}

function textoEstado() {
  const r = estado.reporte;
  if (!r) return "⚠️ El equipo de la bomba todavía no se ha conectado.";
  const lineas = ["💧 *Estado de la bomba*", ""];
  lineas.push(`Bomba: ${r.encendida ? "🟢 Encendida" : "⚪ Apagada"}`);
  lineas.push(`Flujo: ${numero(r.caudal_lmin)} L/min`);
  lineas.push(`Total medido: ${numero(r.total_litros)} L`);
  if (r.encendida && estado.apagarEn !== null) {
    lineas.push(`Se apaga sola en: ${Math.max(1, Math.round((estado.apagarEn - Date.now()) / 60000))} min`);
  }
  lineas.push(conectado() ? `Equipo: 🟢 Conectado (${hace(estado.vistoEn)})` : `Equipo: 🔴 Sin conexión (último reporte ${hace(estado.vistoEn)})`);
  if (estado.ultimoAviso) lineas.push("", `Último aviso (${hora(estado.ultimoAviso.en)}): ${estado.ultimoAviso.texto}.`);
  return lineas.join("\n");
}

function esperarConfirmacion(desde, encender) {
  return new Promise((resolve) => {
    const inicio = Date.now();
    const revisar = setInterval(() => {
      const listo = estado.vistoEn !== null && estado.vistoEn > desde && estado.reporte.encendida === encender;
      if (listo || Date.now() - inicio >= ESPERA_CONFIRMACION_MS) {
        clearInterval(revisar);
        resolve(listo);
      }
    }, 250);
  });
}

// ---------- Rutas ----------

function reporteDelDispositivo(body) {
  estado.reporte = {
    encendida: Boolean(body.encendida),
    caudal_lmin: Number(body.caudal_lmin) || 0,
    total_litros: Number(body.total_litros) || 0,
  };
  estado.vistoEn = Date.now();

  // El ESP32 se apago solo por una proteccion: no volver a encenderlo hasta nueva orden.
  if (body.apagado_por) {
    estado.deseada = false;
    estado.apagarEn = null;
    estado.ultimoAviso = { texto: `Se apagó sola: ${MOTIVOS[body.apagado_por] || body.apagado_por}`, en: Date.now() };
    console.log(`Apagado automatico reportado por el equipo: ${body.apagado_por}`);
  }
  aplicarApagadoProgramado();
  return { encender: estado.deseada };
}

async function ordenDesdeWhatsApp(body) {
  const accion = String(body.accion || "").toLowerCase();
  if (accion !== "encender" && accion !== "apagar") {
    return { ok: false, mensaje: "No entendí la orden. Usa *encender* o *apagar*." };
  }
  if (AUTORIZADOS.length && !AUTORIZADOS.includes(ultimosDiez(body.telefono))) {
    return { ok: false, mensaje: "🚫 Tu número no está autorizado para controlar la bomba." };
  }

  const encender = accion === "encender";
  if (encender && !conectado()) {
    return { ok: false, mensaje: "⚠️ El equipo de la bomba no está conectado, así que no puedo encenderla. Revisa que tenga luz e internet." };
  }

  const pedidos = Number(body.minutos) || MINUTOS_DEFAULT;
  const minutos = Math.min(Math.max(1, Math.round(pedidos)), MINUTOS_MAX);
  const desde = Date.now();
  estado.deseada = encender;
  estado.apagarEn = encender ? desde + minutos * 60000 : null;
  console.log(`Orden ${accion}${encender ? ` (${minutos} min)` : ""} de ...${ultimosDiez(body.telefono).slice(-4)}`);

  if (!conectado()) {
    return { ok: true, mensaje: "⚪ Listo: la bomba queda marcada como apagada. El equipo está sin conexión; si estaba encendida, se apaga sola al perder la conexión." };
  }
  const confirmado = await esperarConfirmacion(desde, encender);
  if (!confirmado) {
    return { ok: true, mensaje: "⏳ Envié la orden, pero el equipo todavía no la confirma. Escribe *3* en unos segundos para ver el estado." };
  }
  return encender
    ? { ok: true, mensaje: `🟢 Bomba encendida. Se apagará sola en ${minutos} min.\nEl agua tarda unos segundos en empezar a fluir; escribe *3* para ver el flujo.` }
    : { ok: true, mensaje: `⚪ Bomba apagada.\nTotal medido: ${numero(estado.reporte.total_litros)} L` };
}

// ---------- Servidor ----------

function leerJson(req) {
  return new Promise((resolve, reject) => {
    let datos = "";
    req.on("data", (parte) => {
      datos += parte;
      if (datos.length > 10000) reject(new Error("cuerpo demasiado grande"));
    });
    req.on("end", () => {
      try {
        resolve(datos ? JSON.parse(datos) : {});
      } catch {
        reject(new Error("JSON invalido"));
      }
    });
    req.on("error", reject);
  });
}

function responder(res, codigo, cuerpo) {
  res.writeHead(codigo, { "Content-Type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(cuerpo));
}

const servidor = http.createServer(async (req, res) => {
  const ruta = new URL(req.url, "http://localhost").pathname;
  try {
    if (req.method === "GET" && ruta === "/salud") return responder(res, 200, { ok: true });

    if (req.method === "POST" && ruta === "/dispositivo/reporte") {
      if (!claveValida(req.headers["x-device-key"], DEVICE_KEY)) return responder(res, 401, { error: "clave invalida" });
      return responder(res, 200, reporteDelDispositivo(await leerJson(req)));
    }

    // Rutas que usa el bot. Los errores de negocio responden 200 con ok:false y un mensaje
    // listo para mostrar, porque el nodo http_request descarta las respuestas que no son 2xx.
    if (ruta === "/estado" || ruta === "/bomba") {
      if (!claveValida(req.headers["x-api-key"], API_KEY)) return responder(res, 401, { error: "clave invalida" });
      aplicarApagadoProgramado();
      if (req.method === "GET" && ruta === "/estado") {
        const r = estado.reporte;
        return responder(res, 200, {
          ok: true,
          mensaje: textoEstado(),
          conectado: conectado(),
          encendida: r ? r.encendida : false,
          caudal_lmin: r ? r.caudal_lmin : 0,
          total_litros: r ? r.total_litros : 0,
        });
      }
      if (req.method === "POST" && ruta === "/bomba") return responder(res, 200, await ordenDesdeWhatsApp(await leerJson(req)));
    }

    responder(res, 404, { error: "no encontrado" });
  } catch (err) {
    responder(res, 400, { error: err.message });
  }
});

servidor.listen(PORT, () => {
  console.log(`Servicio de la bomba escuchando en el puerto ${PORT}`);
  if (!AUTORIZADOS.length) console.warn("AUTORIZADOS esta vacio: cualquier numero puede controlar la bomba");
});
