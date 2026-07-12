#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>          // La librería mágica
#include <Preferences.h>
#include <time.h>

// Pines Cisterna (Abajo)
const int PIN_TRIG_CISTERNA = 5;
const int PIN_ECHO_CISTERNA = 18;

// Pines Tanque (Arriba)
const int PIN_TRIG_TANQUE = 23;
const int PIN_ECHO_TANQUE = 22;

// Pin del Motor
const int PIN_MOTOR = 2;

// --- CONFIGURACIÓN DEL SISTEMA ---
const int CAPACIDAD_CISTERNA_FALLBACK_LITROS = 500;
const int CAPACIDAD_TANQUE_FALLBACK_LITROS = 1000;
const int CISTERNA_DISTANCIA_LLENO_DEF = 20;
const int CISTERNA_DISTANCIA_VACIO_DEF = 75;
const int TANQUE_DISTANCIA_LLENO_DEF = 20;
const int TANQUE_DISTANCIA_VACIO_DEF = 90;
const int DIAMETRO_CISTERNA_DEF_CM = 0;
const int ALTURA_CISTERNA_DEF_CM = 0;
const int DIAMETRO_TANQUE_DEF_CM = 0;
const int ALTURA_TANQUE_DEF_CM = 0;
const float CAUDAL_BOMBA_DEF_LPM = 20.0f;
const int TANQUE_ARRANQUE_DEF_PORCENTAJE = 50;
const int TANQUE_OBJETIVO_DEF_PORCENTAJE = 80;
const int CISTERNA_RESERVA_DEF_PORCENTAJE = 20;
const int TANQUE_OBJETIVO_DEF_LITROS = 800;

int diametroCisternaCm = DIAMETRO_CISTERNA_DEF_CM;
int alturaCisternaCm = ALTURA_CISTERNA_DEF_CM;
int diametroTanqueCm = DIAMETRO_TANQUE_DEF_CM;
int alturaTanqueCm = ALTURA_TANQUE_DEF_CM;
float caudalBombaLpm = CAUDAL_BOMBA_DEF_LPM;
int tanqueArranquePorcentaje = TANQUE_ARRANQUE_DEF_PORCENTAJE;
int tanqueObjetivoPorcentaje = TANQUE_OBJETIVO_DEF_PORCENTAJE;
int tanqueObjetivoLitros = TANQUE_OBJETIVO_DEF_LITROS;
int cisternaReservaPorcentaje = CISTERNA_RESERVA_DEF_PORCENTAJE;
int cisternaDistanciaLlenoCm = CISTERNA_DISTANCIA_LLENO_DEF;
int cisternaDistanciaVacioCm = CISTERNA_DISTANCIA_VACIO_DEF;
int tanqueDistanciaLlenoCm = TANQUE_DISTANCIA_LLENO_DEF;
int tanqueDistanciaVacioCm = TANQUE_DISTANCIA_VACIO_DEF;

// El tiempo de seguridad se calcula con la capacidad útil y el caudal,
// pero conservando un mínimo para evitar cortes demasiado agresivos por ruido.
const unsigned long TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS = 10UL * 60UL * 1000UL;
const float FACTOR_SEGURIDAD_BOMBA = 1.2f;

// --- VARIABLES DE ESTADO ---
bool motorEncendido = false;
bool bloqueoSeguridad = false;
bool reiniciarParaConfigWiFi = false;
bool releManualDirecto = false;
unsigned long tiempoInicioBomba = 0;
unsigned long tiempoFuncionando = 0;
unsigned long tiempoMaximoBombaMs = TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
String accesoWebIp = "";
String accesoWebMdns = "http://tanque.local/";

// Control manual por Web (0 = Automático, 1 = Forzar Encendido, 2 = Forzar Apagado)
int modoManual = 0;

WiFiManager wifiManager;
Preferences preferencias;
String mensajeConfig = "";

// --- LOGS Y CONSUMO DE AGUA ---
const int MAX_LOGS = 40;
const unsigned long INTERVALO_CONSUMO_MS = 30000; // muestreo cada 30 s

struct EventoLog {
  String momento;
  String ip;
  String accion;
};

EventoLog logs[MAX_LOGS];
int logInicio = 0;
int logCantidad = 0;
float histConsumoLitros[24] = {0};
unsigned long ultimoMuestreoConsumo = 0;
int ultimoLitrosTanqueMuestreo = -1;

void guardarHistorialConsumo() {
  preferencias.begin("consumo", false);
  preferencias.putBytes("hist24", histConsumoLitros, sizeof(histConsumoLitros));
  preferencias.end();
}

void cargarHistorialConsumo() {
  preferencias.begin("consumo", true);
  size_t leidos = preferencias.getBytes("hist24", histConsumoLitros, sizeof(histConsumoLitros));
  preferencias.end();

  if (leidos != sizeof(histConsumoLitros)) {
    for (int i = 0; i < 24; i++) {
      histConsumoLitros[i] = 0;
    }
  }
}

void borrarHistorialConsumo() {
  for (int i = 0; i < 24; i++) {
    histConsumoLitros[i] = 0;
  }
  guardarHistorialConsumo();
}

String marcaTiempoActual() {
  struct tm info;
  if (getLocalTime(&info, 5)) {
    char buffer[24];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &info);
    return String(buffer);
  }

  unsigned long totalSeg = millis() / 1000;
  unsigned long hh = totalSeg / 3600;
  unsigned long mm = (totalSeg % 3600) / 60;
  unsigned long ss = totalSeg % 60;
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "uptime %02lu:%02lu:%02lu", hh, mm, ss);
  return String(buffer);
}

void registrarEvento(const String& accion, const String& ip) {
  int idx = (logInicio + logCantidad) % MAX_LOGS;
  logs[idx].momento = marcaTiempoActual();
  logs[idx].ip = ip;
  logs[idx].accion = accion;

  if (logCantidad < MAX_LOGS) {
    logCantidad++;
  } else {
    logInicio = (logInicio + 1) % MAX_LOGS;
  }
}

int obtenerHoraHistograma() {
  struct tm info;
  if (getLocalTime(&info, 5)) {
    return info.tm_hour;
  }
  return (millis() / 3600000UL) % 24;
}

void actualizarHistogramaConsumo(int litrosTanque) {
  unsigned long ahora = millis();
  if (ultimoLitrosTanqueMuestreo < 0) {
    ultimoLitrosTanqueMuestreo = litrosTanque;
    ultimoMuestreoConsumo = ahora;
    return;
  }

  if (ahora - ultimoMuestreoConsumo < INTERVALO_CONSUMO_MS) {
    return;
  }

  int delta = ultimoLitrosTanqueMuestreo - litrosTanque;
  // Ignora ruido pequeño de los ultrasónicos
  if (delta >= 2) {
    int h = obtenerHoraHistograma();
    histConsumoLitros[h] += delta;
    guardarHistorialConsumo();
  }

  ultimoLitrosTanqueMuestreo = litrosTanque;
  ultimoMuestreoConsumo = ahora;
}

String obtenerRutaSolicitud(const String& request) {
  int inicio = request.indexOf("GET /");
  if (inicio < 0) {
    return "";
  }

  int fin = request.indexOf(' ', inicio + 4);
  if (fin < 0) {
    return "";
  }

  return request.substring(inicio + 4, fin);
}

String obtenerParametroConsulta(const String& ruta, const String& nombre) {
  int pregunta = ruta.indexOf('?');
  if (pregunta < 0) {
    return "";
  }

  String consulta = ruta.substring(pregunta + 1);
  String marca = nombre + "=";
  int inicio = consulta.indexOf(marca);
  if (inicio < 0) {
    return "";
  }

  inicio += marca.length();
  int fin = consulta.indexOf('&', inicio);
  if (fin < 0) {
    fin = consulta.length();
  }

  String valor = consulta.substring(inicio, fin);
  valor.replace("+", " ");
  return valor;
}

int leerParametroEntero(const String& ruta, const String& nombre, int valorActual, int minimo, int maximo) {
  String valor = obtenerParametroConsulta(ruta, nombre);
  if (valor.length() == 0) {
    return valorActual;
  }

  int nuevoValor = valor.toInt();
  return constrain(nuevoValor, minimo, maximo);
}

float leerParametroFloat(const String& ruta, const String& nombre, float valorActual, float minimo, float maximo) {
  String valor = obtenerParametroConsulta(ruta, nombre);
  if (valor.length() == 0) {
    return valorActual;
  }

  float nuevoValor = valor.toFloat();
  if (nuevoValor < minimo) {
    nuevoValor = minimo;
  }
  if (nuevoValor > maximo) {
    nuevoValor = maximo;
  }
  return nuevoValor;
}

int calcularCapacidadCilindricaLitros(int diametroCm, int alturaCm, int capacidadFallbackLitros) {
  if (diametroCm <= 0 || alturaCm <= 0) {
    return capacidadFallbackLitros;
  }

  float radioCm = diametroCm / 2.0f;
  float volumenCm3 = 3.14159265f * radioCm * radioCm * alturaCm;
  int litros = (int)(volumenCm3 / 1000.0f + 0.5f);
  if (litros <= 0) {
    return capacidadFallbackLitros;
  }

  return litros;
}

int calcularPorcentajeDesdeDistancia(int distancia, int distanciaLleno, int distanciaVacio) {
  if (distanciaLleno == distanciaVacio) {
    return 0;
  }

  int menor = min(distanciaLleno, distanciaVacio);
  int mayor = max(distanciaLleno, distanciaVacio);
  int distanciaAjustada = constrain(distancia, menor, mayor);
  int porcentaje = map(distanciaAjustada, distanciaVacio, distanciaLleno, 0, 100);
  if (porcentaje < 0) porcentaje = 0;
  if (porcentaje > 100) porcentaje = 100;
  return porcentaje;
}

int porcentajeDesdeLitros(int litros, int capacidadLitros) {
  if (capacidadLitros <= 0) {
    return 0;
  }

  int porcentaje = (int)((litros * 100.0f) / capacidadLitros + 0.5f);
  if (porcentaje < 0) porcentaje = 0;
  if (porcentaje > 100) porcentaje = 100;
  return porcentaje;
}

bool valorConsultaRecibido(const String& ruta, const String& nombre) {
  return obtenerParametroConsulta(ruta, nombre).length() > 0;
}

bool guardarConfiguracionDesdeRuta(const String& ruta, String& error) {
  int nuevoDiametroTanqueCm = leerParametroEntero(ruta, "diam_tanque", diametroTanqueCm, 0, 500);
  int nuevaAlturaTanqueCm = leerParametroEntero(ruta, "alto_tanque", alturaTanqueCm, 0, 500);
  int nuevoDiametroCisternaCm = leerParametroEntero(ruta, "diam_cisterna", diametroCisternaCm, 0, 500);
  int nuevaAlturaCisternaCm = leerParametroEntero(ruta, "alto_cisterna", alturaCisternaCm, 0, 500);
  float nuevoCaudalBombaLpm = leerParametroFloat(ruta, "caudal_bomba", caudalBombaLpm, 0.1f, 9999.0f);
  int nuevoTanqueArranquePorcentaje = leerParametroEntero(ruta, "tanque_arranque_pct", tanqueArranquePorcentaje, 1, 99);
  int nuevoTanqueObjetivoPct = leerParametroEntero(ruta, "tanque_objetivo_pct", tanqueObjetivoPorcentaje, 1, 100);
  int nuevaCisternaReservaPct = leerParametroEntero(ruta, "cisterna_reserva_pct", cisternaReservaPorcentaje, 1, 99);
  int nuevoTanqueDistanciaLlenoCm = leerParametroEntero(ruta, "tanque_lleno", tanqueDistanciaLlenoCm, 0, 999);
  int nuevoTanqueDistanciaVacioCm = leerParametroEntero(ruta, "tanque_vacio", tanqueDistanciaVacioCm, 0, 999);
  int nuevoCisternaDistanciaLlenoCm = leerParametroEntero(ruta, "cisterna_lleno", cisternaDistanciaLlenoCm, 0, 999);
  int nuevoCisternaDistanciaVacioCm = leerParametroEntero(ruta, "cisterna_vacio", cisternaDistanciaVacioCm, 0, 999);

  int nuevaCapacidadTanqueLitros = calcularCapacidadCilindricaLitros(nuevoDiametroTanqueCm, nuevaAlturaTanqueCm, CAPACIDAD_TANQUE_FALLBACK_LITROS);
  int nuevaCapacidadCisternaLitros = calcularCapacidadCilindricaLitros(nuevoDiametroCisternaCm, nuevaAlturaCisternaCm, CAPACIDAD_CISTERNA_FALLBACK_LITROS);
  int nuevoTanqueObjetivoLitros = leerParametroEntero(ruta, "tanque_objetivo_litros", tanqueObjetivoLitros, 1, max(nuevaCapacidadTanqueLitros, 1));

  if (nuevoTanqueDistanciaLlenoCm >= nuevoTanqueDistanciaVacioCm) {
    error = "En tanque, la distancia de lleno debe ser menor que la de vacío.";
    return false;
  }
  if (nuevoCisternaDistanciaLlenoCm >= nuevoCisternaDistanciaVacioCm) {
    error = "En cisterna, la distancia de lleno debe ser menor que la de vacío.";
    return false;
  }
  if (nuevoTanqueArranquePorcentaje >= nuevoTanqueObjetivoPct) {
    error = "El arranque del tanque debe ser menor que el objetivo de carga.";
    return false;
  }
  if (nuevoTanqueObjetivoLitros > nuevaCapacidadTanqueLitros) {
    error = "Los litros objetivo superan la capacidad calculada del tanque.";
    return false;
  }
  if (nuevaCisternaReservaPct >= 100) {
    error = "La reserva de la cisterna debe ser menor a 100%.";
    return false;
  }

  diametroTanqueCm = nuevoDiametroTanqueCm;
  alturaTanqueCm = nuevaAlturaTanqueCm;
  diametroCisternaCm = nuevoDiametroCisternaCm;
  alturaCisternaCm = nuevaAlturaCisternaCm;
  caudalBombaLpm = nuevoCaudalBombaLpm;
  tanqueArranquePorcentaje = nuevoTanqueArranquePorcentaje;
  tanqueObjetivoPorcentaje = nuevoTanqueObjetivoPct;
  tanqueObjetivoLitros = nuevoTanqueObjetivoLitros;
  cisternaReservaPorcentaje = nuevaCisternaReservaPct;
  tanqueDistanciaLlenoCm = nuevoTanqueDistanciaLlenoCm;
  tanqueDistanciaVacioCm = nuevoTanqueDistanciaVacioCm;
  cisternaDistanciaLlenoCm = nuevoCisternaDistanciaLlenoCm;
  cisternaDistanciaVacioCm = nuevoCisternaDistanciaVacioCm;

  preferencias.begin("tanquecfg", false);
  preferencias.putInt("diam_tanque", diametroTanqueCm);
  preferencias.putInt("alto_tanque", alturaTanqueCm);
  preferencias.putInt("diam_cisterna", diametroCisternaCm);
  preferencias.putInt("alto_cisterna", alturaCisternaCm);
  preferencias.putFloat("caudal_bomba", caudalBombaLpm);
  preferencias.putInt("tanque_arranque_pct", tanqueArranquePorcentaje);
  preferencias.putInt("tanque_objetivo_pct", tanqueObjetivoPorcentaje);
  preferencias.putInt("tanque_objetivo_litros", tanqueObjetivoLitros);
  preferencias.putInt("cisterna_reserva_pct", cisternaReservaPorcentaje);
  preferencias.putInt("tanque_lleno", tanqueDistanciaLlenoCm);
  preferencias.putInt("tanque_vacio", tanqueDistanciaVacioCm);
  preferencias.putInt("cisterna_lleno", cisternaDistanciaLlenoCm);
  preferencias.putInt("cisterna_vacio", cisternaDistanciaVacioCm);
  preferencias.end();
  error = "";
  return true;
}

void cargarConfiguracion() {
  preferencias.begin("tanquecfg", true);
  diametroTanqueCm = preferencias.getInt("diam_tanque", DIAMETRO_TANQUE_DEF_CM);
  alturaTanqueCm = preferencias.getInt("alto_tanque", ALTURA_TANQUE_DEF_CM);
  diametroCisternaCm = preferencias.getInt("diam_cisterna", DIAMETRO_CISTERNA_DEF_CM);
  alturaCisternaCm = preferencias.getInt("alto_cisterna", ALTURA_CISTERNA_DEF_CM);
  caudalBombaLpm = preferencias.getFloat("caudal_bomba", CAUDAL_BOMBA_DEF_LPM);
  tanqueArranquePorcentaje = preferencias.getInt("tanque_arranque_pct", TANQUE_ARRANQUE_DEF_PORCENTAJE);
  tanqueObjetivoPorcentaje = preferencias.getInt("tanque_objetivo_pct", TANQUE_OBJETIVO_DEF_PORCENTAJE);
  tanqueObjetivoLitros = preferencias.getInt("tanque_objetivo_litros", TANQUE_OBJETIVO_DEF_LITROS);
  cisternaReservaPorcentaje = preferencias.getInt("cisterna_reserva_pct", CISTERNA_RESERVA_DEF_PORCENTAJE);
  tanqueDistanciaLlenoCm = preferencias.getInt("tanque_lleno", TANQUE_DISTANCIA_LLENO_DEF);
  tanqueDistanciaVacioCm = preferencias.getInt("tanque_vacio", TANQUE_DISTANCIA_VACIO_DEF);
  cisternaDistanciaLlenoCm = preferencias.getInt("cisterna_lleno", CISTERNA_DISTANCIA_LLENO_DEF);
  cisternaDistanciaVacioCm = preferencias.getInt("cisterna_vacio", CISTERNA_DISTANCIA_VACIO_DEF);
  preferencias.end();
}

void restaurarValoresPorDefecto() {
  diametroCisternaCm = DIAMETRO_CISTERNA_DEF_CM;
  alturaCisternaCm = ALTURA_CISTERNA_DEF_CM;
  diametroTanqueCm = DIAMETRO_TANQUE_DEF_CM;
  alturaTanqueCm = ALTURA_TANQUE_DEF_CM;
  caudalBombaLpm = CAUDAL_BOMBA_DEF_LPM;
  tanqueArranquePorcentaje = TANQUE_ARRANQUE_DEF_PORCENTAJE;
  tanqueObjetivoPorcentaje = TANQUE_OBJETIVO_DEF_PORCENTAJE;
  tanqueObjetivoLitros = TANQUE_OBJETIVO_DEF_LITROS;
  cisternaReservaPorcentaje = CISTERNA_RESERVA_DEF_PORCENTAJE;
  tanqueDistanciaLlenoCm = TANQUE_DISTANCIA_LLENO_DEF;
  tanqueDistanciaVacioCm = TANQUE_DISTANCIA_VACIO_DEF;
  cisternaDistanciaLlenoCm = CISTERNA_DISTANCIA_LLENO_DEF;
  cisternaDistanciaVacioCm = CISTERNA_DISTANCIA_VACIO_DEF;
}

void borrarTodoPersistido() {
  preferencias.begin("tanquecfg", false);
  preferencias.clear();
  preferencias.end();
  preferencias.begin("consumo", false);
  preferencias.clear();
  preferencias.end();
  restaurarValoresPorDefecto();
  borrarHistorialConsumo();
  logInicio = 0;
  logCantidad = 0;
  mensajeConfig = "Todo fue restablecido a valores por defecto.";
}

unsigned long calcularTiempoMaximoBombaMs(int capacidadCisternaLitros, int capacidadTanqueLitros) {
  if (caudalBombaLpm <= 0.0f) {
    return TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
  }

  int litrosUtiles = (int)(capacidadCisternaLitros * (100 - cisternaReservaPorcentaje) / 100.0f);
  if (capacidadTanqueLitros > 0) {
    int litrosNecesariosTanque = max(capacidadTanqueLitros - tanqueObjetivoLitros, 0);
    litrosUtiles = min(litrosUtiles, max(litrosNecesariosTanque, 1));
  }
  if (litrosUtiles <= 0) {
    return TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
  }

  unsigned long estimadoMs = (unsigned long)((litrosUtiles / caudalBombaLpm) * 60000.0f * FACTOR_SEGURIDAD_BOMBA);
  if (estimadoMs < TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS) {
    estimadoMs = TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
  }

  return estimadoMs;
}

// --- CONFIGURACIÓN DE IP MANUAL (ESTÁTICA) ---
// Si necesitas usar IP fija en el futuro, descomenta estas 4 líneas de abajo
// y también la línea "wifiManager.setSTAStaticIPConfig(...)" dentro del setup().
// const IPAddress IP_MANUAL(192, 168, 0, 34);
// const IPAddress GW_MANUAL(192, 168, 0, 1);
// const IPAddress MASK_MANUAL(255, 255, 255, 0);
// const IPAddress DNS_MANUAL(8, 8, 8, 8);

int distCisterna = 0;
int distTanque = 0;

WiFiServer server(80);

int medirDistancia(int pinTrig, int pinEcho) {
  digitalWrite(pinTrig, LOW);
  delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);
  long duracion = pulseIn(pinEcho, HIGH, 30000);
  if (duracion == 0) return 999;
  return duracion * 0.034 / 2;
}

void setup() {
  Serial.begin(115200);

  cargarConfiguracion();
  cargarHistorialConsumo();

  pinMode(PIN_TRIG_CISTERNA, OUTPUT); pinMode(PIN_ECHO_CISTERNA, INPUT);
  pinMode(PIN_TRIG_TANQUE, OUTPUT); pinMode(PIN_ECHO_TANQUE, INPUT);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  // --- CONFIGURACIÓN DE CONEXIÓN WI-FI ---

  // Modo simultáneo: DHCP por defecto + opción de IP manual en el portal WiFiManager.
  // Si dejas los campos IP/GW/MASK/DNS vacíos en el portal, conecta por DHCP.
  wifiManager.setShowStaticFields(true);
  wifiManager.setShowDnsFields(true);

  // Opción alternativa por código: descomenta la línea de abajo solo si descomentaste
  // los valores de IP_MANUAL arriba.
  // wifiManager.setSTAStaticIPConfig(IP_MANUAL, GW_MANUAL, MASK_MANUAL, DNS_MANUAL);

  // Eliminamos el setConnectTimeout(10) agresivo para dar tiempo a que conecte por DHCP correctamente.
  // En su lugar, si la red falla, se quedará indefinidamente en modo Portal ("Tanque_Cisterna") hasta que configures una red válida.
  
  Serial.println("Iniciando AutoConnect (DHCP por defecto / IP manual opcional)...");
  if (!wifiManager.autoConnect("Tanque_Cisterna")) {
    Serial.println("Error al conectar. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  // Si pasa de aquí, significa que está conectado con éxito a la red (por DHCP o Estática si se activa)
  Serial.println("\n¡Wi-Fi Conectado exitosamente!");
  accesoWebIp = "http://" + WiFi.localIP().toString() + "/";
  Serial.print("IP local asignada: ");
  Serial.println(WiFi.localIP());
  Serial.print("URL por IP: ");
  Serial.println(accesoWebIp);
  Serial.print("URL mDNS: ");
  Serial.println(accesoWebMdns);

  // Sin RTC, intentamos hora de red para que los logs tengan fecha/hora real.
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  registrarEvento("BOOT_OK", WiFi.localIP().toString());
  
  server.begin();
  MDNS.begin("tanque");
}

void loop() {
  distCisterna = medirDistancia(PIN_TRIG_CISTERNA, PIN_ECHO_CISTERNA);
  distTanque = medirDistancia(PIN_TRIG_TANQUE, PIN_ECHO_TANQUE);

  int capacidadCisternaLitros = calcularCapacidadCilindricaLitros(diametroCisternaCm, alturaCisternaCm, CAPACIDAD_CISTERNA_FALLBACK_LITROS);
  int capacidadTanqueLitros = calcularCapacidadCilindricaLitros(diametroTanqueCm, alturaTanqueCm, CAPACIDAD_TANQUE_FALLBACK_LITROS);
  int tanqueObjetivoLitrosActual = constrain(tanqueObjetivoLitros, 1, capacidadTanqueLitros);
  int tanqueObjetivoPctActual = porcentajeDesdeLitros(tanqueObjetivoLitrosActual, capacidadTanqueLitros);

  // Porcentajes y litros usando calibración configurable de lleno/vacío.
  int pctCisterna = calcularPorcentajeDesdeDistancia(distCisterna, cisternaDistanciaLlenoCm, cisternaDistanciaVacioCm);
  int pctTanque = calcularPorcentajeDesdeDistancia(distTanque, tanqueDistanciaLlenoCm, tanqueDistanciaVacioCm);
  int litrosCisterna = map(pctCisterna, 0, 100, 0, capacidadCisternaLitros);
  int litrosTanque = map(pctTanque, 0, 100, 0, capacidadTanqueLitros);
  actualizarHistogramaConsumo(litrosTanque);

  // --- LÓGICA AUTOMÁTICA DE LA BOMBA ---
  bool cisternaTieneAgua = (pctCisterna > cisternaReservaPorcentaje);
  bool tanqueNecesitaCarga = (pctTanque <= tanqueArranquePorcentaje);
  bool tanqueAlcanzoObjetivo = (pctTanque >= tanqueObjetivoPctActual) || (litrosTanque >= tanqueObjetivoLitrosActual);
  bool motorDeberiaEncender = motorEncendido;

  if (pctTanque > tanqueArranquePorcentaje && pctTanque < tanqueObjetivoPctActual && !motorEncendido) {
    bloqueoSeguridad = false;
  }

  if (!cisternaTieneAgua || tanqueAlcanzoObjetivo) {
    motorDeberiaEncender = false; 
  } else if (tanqueNecesitaCarga) {
    motorDeberiaEncender = true;
  }

  // --- APLICAR MODOS MANUALES / SEGURIDAD ---
  if (releManualDirecto) {
    motorEncendido = true; // Relé directo por confirmación manual (salta lógica de niveles)
  } else if (bloqueoSeguridad) {
    motorEncendido = false;
  } else if (modoManual == 1) {
    motorEncendido = cisternaTieneAgua;
  } else if (modoManual == 2) {
    motorEncendido = false;
  } else {
    motorEncendido = motorDeberiaEncender;
  }

  // --- CONTROL DE TIEMPO Y TEMPORIZADOR ---
  if (motorEncendido) {
    if (tiempoInicioBomba == 0) {
      tiempoInicioBomba = millis(); 
      tiempoMaximoBombaMs = calcularTiempoMaximoBombaMs(capacidadCisternaLitros, capacidadTanqueLitros);
    }
    tiempoFuncionando = millis() - tiempoInicioBomba;

    if (tiempoFuncionando >= tiempoMaximoBombaMs) {
      bloqueoSeguridad = true;
      motorEncendido = false;
      modoManual = 0;
      releManualDirecto = false;
    }
  } else {
    tiempoInicioBomba = 0;
    tiempoFuncionando = 0;
    tiempoMaximoBombaMs = TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
  }

  digitalWrite(PIN_MOTOR, motorEncendido ? HIGH : LOW);

  // --- SERVIDOR WEB ---
  WiFiClient client = server.available();
  if (client) {
    String currentLine = "";
    String request = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        request += c;
        if (c == '\n') {
          if (currentLine.length() == 0) {
            String ipCliente = client.remoteIP().toString();
            String ruta = obtenerRutaSolicitud(request);
            bool esPaginaPrincipal = (ruta == "/" || ruta.length() == 0);
            bool enPaginaConfig = ruta.startsWith("/config");
            bool enPaginaLog = ruta.startsWith("/log");
            bool configGuardada = false;
            int intervaloAutoRefreshMs = esPaginaPrincipal ? 15000 : 0;

            // --- FILTRO DE SEGURIDAD PARA BOTONES MANUALES ---
            if (ruta.startsWith("/ENCENDER")) {
              releManualDirecto = true;
              modoManual = 1;
              bloqueoSeguridad = false;
              registrarEvento("ENCENDER_MANUAL", ipCliente);
            }
            if (ruta.startsWith("/APAGAR")) {
              releManualDirecto = false;
              modoManual = 2;
              registrarEvento("APAGAR", ipCliente);
            }
            if (ruta.startsWith("/AUTO")) {
              releManualDirecto = false;
              modoManual = 0;
              bloqueoSeguridad = false;
              registrarEvento("AUTO_RESET", ipCliente);
            }
            if (ruta.startsWith("/config/cambiar_wifi")) {
              releManualDirecto = false;
              reiniciarParaConfigWiFi = true;
              wifiManager.resetSettings();
              registrarEvento("CAMBIAR_WIFI", ipCliente);
            }
            if (ruta.startsWith("/config/guardar")) {
              String errorGuardar = "";
              if (guardarConfiguracionDesdeRuta(ruta, errorGuardar)) {
                configGuardada = true;
                registrarEvento("GUARDAR_CONFIG", ipCliente);
              } else {
                mensajeConfig = errorGuardar;
              }
            }
            if (ruta.startsWith("/config/borrar_datos")) {
              borrarTodoPersistido();
              configGuardada = true;
              registrarEvento("BORRAR_DATOS", ipCliente);
            }

            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
            client.println("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
            client.println("<title>Monitor de Agua ESP32</title>");
            client.println("<style>body{font-family:Arial,sans-serif;text-align:center;background:#f4f4f9;padding:10px;} .card{background:white;padding:15px;border-radius:15px;margin:12px auto;max-width:400px;box-shadow:0 4px 8px rgba(0,0,0,0.1);} h1{color:#0288d1;} .status{font-weight:bold;padding:10px;border-radius:5px;margin-top:10px;} .on{background:#c8e6c9;color:#256029;} .off{background:#ffcdd2;color:#c62828;} .alert{background:#ffe0b2;color:#e65100;} .val{font-size:24px;color:#0288d1;font-weight:bold;} .btn{display:inline-block;width:125px;box-sizing:border-box;text-align:center;padding:10px 5px;margin:5px;font-size:14px;font-weight:bold;color:white;text-decoration:none;border-radius:5px;box-shadow:0 2px 4px rgba(0,0,0,0.2);} .btn-on{background:#4caf50;} .btn-off{background:#f44336;} .btn-auto{background:#2196f3;} .btn-warn{background:#d32f2f;} .modal{display:none;position:fixed;z-index:999;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.55);} .modal-content{background:#fff;border-radius:12px;max-width:360px;margin:15% auto;padding:16px;box-shadow:0 8px 20px rgba(0,0,0,0.25);} .modal-title{margin:0 0 8px 0;color:#b71c1c;font-size:18px;} .modal-text{margin:0 0 12px 0;color:#333;font-size:14px;line-height:1.35;}</style>");

            if (intervaloAutoRefreshMs > 0) {
              client.println("<script>setInterval(function(){location.reload();}, " + String(intervaloAutoRefreshMs) + ");function openConfirmModal(){document.getElementById('confirmModal').style.display='block';document.body.style.overflow='hidden';}function closeConfirmModal(){document.getElementById('confirmModal').style.display='none';document.body.style.overflow='';}function confirmarArranqueManual(){window.location.href='/ENCENDER';}function bindModalEvents(){var m=document.getElementById('confirmModal');if(!m){return;}window.addEventListener('click',function(e){if(e.target===m){closeConfirmModal();}});document.addEventListener('keydown',function(e){if(e.key==='Escape'||e.key==='Esc'){closeConfirmModal();}});}document.addEventListener('DOMContentLoaded',bindModalEvents);</script>");
            } else {
              client.println("<script>function openConfirmModal(){document.getElementById('confirmModal').style.display='block';document.body.style.overflow='hidden';}function closeConfirmModal(){document.getElementById('confirmModal').style.display='none';document.body.style.overflow='';}function confirmarArranqueManual(){window.location.href='/ENCENDER';}function bindModalEvents(){var m=document.getElementById('confirmModal');if(!m){return;}window.addEventListener('click',function(e){if(e.target===m){closeConfirmModal();}});document.addEventListener('keydown',function(e){if(e.key==='Escape'||e.key==='Esc'){closeConfirmModal();}});}document.addEventListener('DOMContentLoaded',bindModalEvents);</script>");
            }
            client.println("</head><body>");
            client.println("<h1>💧 Control de Agua 💧</h1>");
            // client.println("<h1>💧 ESP32 💧</h1>");

            client.println("<div class='card'><h2>Tanque Superior (" + String(capacidadTanqueLitros) + "L)</h2><p class='val'>" + String(pctTanque) + " %</p><p>Distancia actual: <strong>" + String(distTanque) + " cm</strong></p><p>Aprox: <strong>" + String(litrosTanque) + " Litros</strong></p></div>");
            client.println("<div class='card'><h2>Cisterna (" + String(capacidadCisternaLitros) + "L)</h2><p class='val'>" + String(pctCisterna) + " %</p><p>Distancia actual: <strong>" + String(distCisterna) + " cm</strong></p><p>Aprox: <strong>" + String(litrosCisterna) + " Litros</strong></p></div>");

            client.println("<div class='card'><h2>Datos de Conexión</h2>");
            client.println("<p style='margin:6px 0;'>IP local: <strong>" + accesoWebIp + "</strong></p>");
            client.println("<p style='margin:6px 0;'>mDNS: <strong>" + accesoWebMdns + "</strong></p>");
            if (WiFi.status() == WL_CONNECTED) {
              // client.println("<p style='margin:10px 0 0 0; font-size:13px; color:#555;'>Ya estás en el panel de control.</p>");
            }
            client.println("</div>");

            client.println("<div class='card'><h2>Estado de la Bomba</h2>");
            if (bloqueoSeguridad) {
              client.println("<div class='status off alert'>🚨 BLOQUEO DE SEGURIDAD (Excedió el límite estimado)</div>");
            } else if (motorEncendido) {
              unsigned long seg = (tiempoFuncionando / 1000) % 60;
              unsigned long min = (tiempoFuncionando / 1000) / 60;
              unsigned long segMax = (tiempoMaximoBombaMs / 1000) % 60;
              unsigned long minMax = (tiempoMaximoBombaMs / 1000) / 60;
              client.println("<div class='status on'>BOMBEANDO AGUA...</div>");
              client.println("<p style='margin-top:10px;'>Tiempo activo: <strong>" + String(min) + " min " + String(seg) + " seg</strong></p>");
              client.println("<p style='margin-top:6px;'>Límite estimado: <strong>" + String(minMax) + " min " + String(segMax) + " seg</strong></p>");
            } else {
              client.println("<div class='status off'>MOTOR APAGADO</div>");
            }

            String modoTexto = releManualDirecto ? "MANUAL DIRECTO (RELÉ FORZADO)" : ((modoManual == 0) ? "AUTOMÁTICO" : ((modoManual == 1) ? "MANUAL (ENCENDIDO)" : "MANUAL (APAGADO)"));
            client.println("<p style='font-size:12px; color:#666;'>Modo actual: " + modoTexto + "</p>");
            client.println("</div>");

            client.println("<div class='card'><h2>Controles Manuales</h2>");
            // client.println("<p style='margin:0 0 10px 0; color:#e65100; font-weight:bold;'>Arranque manual con confirmación de seguridad.</p>");
            client.println("<a href='#' onclick='openConfirmModal();return false;' class='btn btn-on'>ARRANQUE</a>");
            client.println("<a href='/APAGAR' class='btn btn-off'>PARADA</a>");
            client.println("<a href='/AUTO' class='btn btn-auto'>AUTO / RESET</a>");
            client.println("</div>");

            if (enPaginaConfig) {
              client.println("<div class='card'><h2>Configuración</h2>");
              if (configGuardada) {
                client.println("<p style='margin:0 0 10px 0; color:#1b5e20; font-weight:bold;'>Configuración guardada.</p>");
              }
              if (mensajeConfig.length() > 0) {
                client.println("<p style='margin:0 0 10px 0; color:#b71c1c; font-weight:bold;'>" + mensajeConfig + "</p>");
                mensajeConfig = "";
              }
              client.println("<p style='margin:0 0 6px 0; color:#666;'>Capacidad tanque calculada: <strong>" + String(capacidadTanqueLitros) + " L</strong></p>");
              client.println("<p style='margin:0 0 6px 0; color:#666;'>Capacidad cisterna calculada: <strong>" + String(capacidadCisternaLitros) + " L</strong></p>");
              client.println("<p style='margin:0 0 6px 0; color:#666;'>Arranque tanque: <strong>" + String(tanqueArranquePorcentaje) + "%</strong></p>");
              client.println("<p style='margin:0 0 6px 0; color:#666;'>Objetivo tanque: <strong>" + String(tanqueObjetivoLitrosActual) + " L</strong> (<strong>" + String(tanqueObjetivoPctActual) + "%</strong>)</p>");
              client.println("<p style='margin:0 0 6px 0; color:#666;'>Reserva cisterna: <strong>" + String(cisternaReservaPorcentaje) + "%</strong></p>");
              client.println("<p style='margin:0 0 10px 0; color:#666;'>Si diámetro y altura están en cero, se usa capacidad de respaldo.</p>");
              client.println("<a href='/log' class='btn btn-auto'>VER LOG</a>");
              client.println("<a href='/config/cambiar_wifi' class='btn btn-auto'>CAMBIAR WIFI</a>");
              client.println("<a href='/config/borrar_datos' class='btn btn-warn'>BORRAR TODO</a>");
              client.println("<a href='/' class='btn btn-off'>VOLVER</a>");
              client.println("</div>");

              client.println("<div class='card'><h2>Guardar configuración</h2>");
              client.println("<form action='/config/guardar' method='GET'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Diámetro tanque (cm)</p><input type='number' name='diam_tanque' value='" + String(diametroTanqueCm) + "' min='0' max='500' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Altura tanque (cm)</p><input type='number' name='alto_tanque' value='" + String(alturaTanqueCm) + "' min='0' max='500' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Diámetro cisterna (cm)</p><input type='number' name='diam_cisterna' value='" + String(diametroCisternaCm) + "' min='0' max='500' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Altura cisterna (cm)</p><input type='number' name='alto_cisterna' value='" + String(alturaCisternaCm) + "' min='0' max='500' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Caudal bomba (L/min)</p><input type='number' step='0.1' name='caudal_bomba' value='" + String(caudalBombaLpm, 1) + "' min='0.1' max='9999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Arranque tanque (%)</p><input type='number' name='tanque_arranque_pct' value='" + String(tanqueArranquePorcentaje) + "' min='1' max='99' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Objetivo de carga tanque (L)</p><input type='number' name='tanque_objetivo_litros' value='" + String(tanqueObjetivoLitros) + "' min='1' max='99999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Objetivo tanque de respaldo (%)</p><input type='number' name='tanque_objetivo_pct' value='" + String(tanqueObjetivoPorcentaje) + "' min='1' max='100' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Reserva cisterna (%)</p><input type='number' name='cisterna_reserva_pct' value='" + String(cisternaReservaPorcentaje) + "' min='1' max='99' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Tanque lleno - distancia (cm)</p><input type='number' name='tanque_lleno' value='" + String(tanqueDistanciaLlenoCm) + "' min='0' max='999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Tanque vacío - distancia (cm)</p><input type='number' name='tanque_vacio' value='" + String(tanqueDistanciaVacioCm) + "' min='0' max='999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Cisterna llena - distancia (cm)</p><input type='number' name='cisterna_lleno' value='" + String(cisternaDistanciaLlenoCm) + "' min='0' max='999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Cisterna vacía - distancia (cm)</p><input type='number' name='cisterna_vacio' value='" + String(cisternaDistanciaVacioCm) + "' min='0' max='999' style='width:100%; padding:10px; box-sizing:border-box;'>");
              client.println("<div style='margin-top:12px;'><button type='submit' class='btn btn-auto' style='border:none; cursor:pointer; width:100%;'>GUARDAR</button></div>");
              client.println("</form>");
              client.println("<p style='font-size:12px; color:#666; margin-top:10px;'>La capacidad se calcula con diámetro y altura. Si el objetivo supera la capacidad calculada, no se guarda.</p>");
              client.println("</div>");
            }

            if (enPaginaLog) {
              registrarEvento("VER_LOG", ipCliente);

              client.println("<div class='card'><h2>Bitácora de comandos</h2>");
              client.println("<div style='text-align:left; font-size:12px; color:#333; max-height:220px; overflow:auto; border:1px solid #eee; border-radius:8px; padding:8px;'>");
              if (logCantidad == 0) {
                client.println("<p style='margin:0;'>Sin eventos registrados.</p>");
              } else {
                for (int i = 0; i < logCantidad; i++) {
                  int idx = (logInicio + i) % MAX_LOGS;
                  client.println("<p style='margin:0 0 6px 0;'><strong>" + logs[idx].momento + "</strong> | " + logs[idx].ip + " | " + logs[idx].accion + "</p>");
                }
              }
              client.println("</div>");
              client.println("</div>");

              float maxConsumo = 0;
              for (int h = 0; h < 24; h++) {
                if (histConsumoLitros[h] > maxConsumo) maxConsumo = histConsumoLitros[h];
              }
              if (maxConsumo < 1) maxConsumo = 1;

              client.println("<div class='card'><h2>Histograma de consumo (L/h)</h2>");
              client.println("<div style='text-align:left;'>");
              for (int h = 0; h < 24; h++) {
                int ancho = (int)((histConsumoLitros[h] / maxConsumo) * 100.0);
                if (ancho < 2 && histConsumoLitros[h] > 0) ancho = 2;
                client.println("<div style='display:flex; align-items:center; gap:6px; margin:3px 0;'>");
                if (h < 10) client.println("<span style='width:26px; font-size:11px;'>0" + String(h) + "h</span>");
                else client.println("<span style='width:26px; font-size:11px;'>" + String(h) + "h</span>");
                client.println("<div style='height:12px; width:" + String(ancho) + "%; background:#0288d1; border-radius:6px; min-width:2px;'></div>");
                client.println("<span style='font-size:11px; color:#333; min-width:52px; text-align:right;'>" + String((int)histConsumoLitros[h]) + " L</span>");
                client.println("</div>");
              }
              client.println("</div>");
              client.println("<p style='font-size:11px; color:#666; margin-top:8px;'>Estimado por variación del tanque. Puede incluir ruido de sensor.</p>");
              client.println("<a href='/config' class='btn btn-auto'>VOLVER A CONFIG</a>");
              client.println("</div>");
            }

            client.println("<div id='confirmModal' class='modal'><div class='modal-content'>");
            client.println("<h3 class='modal-title'>⚠ Confirmar arranque manual</h3>");
            client.println("<p class='modal-text'>Vas a intentar encender la bomba en modo manual. Verifica niveles de agua y estado general antes de continuar.</p>");
            client.println("<a href='#' onclick='confirmarArranqueManual();return false;' class='btn btn-warn'>ACEPTAR</a>");
            client.println("<a href='#' onclick='closeConfirmModal();return false;' class='btn btn-auto'>CANCELAR</a>");
            client.println("</div></div>");

            client.println("</body></html>");
            break;
          } else {
            currentLine = "";
          }
        } else if (c != '\r') {
          currentLine += c;
        }
      }
    }
    client.stop();
  }

  if (reiniciarParaConfigWiFi) {
    delay(1000);
    ESP.restart();
  }

  delay(10);
}