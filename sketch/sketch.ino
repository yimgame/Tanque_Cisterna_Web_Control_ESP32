#include <WiFi.h>         // Librería WiFi para ESP32
#include <WebServer.h>    // Servidor web para la interfaz de usuario
#include <DNSServer.h>    // Servidor DNS para el portal de configuración WiFiManager
#include <ESPmDNS.h>      // mDNS para "tanque.local"
#include <WiFiManager.h>  // Configuración wifi
#include <Preferences.h>  // Librería para almacenamiento persistente en la memoria flash del ESP32
#include <time.h>         // Librería para obtener la hora de red (NTP) y formatearla

// Pines Cisterna (Abajo)
const int PIN_TRIG_CISTERNA = 4;
const int PIN_ECHO_CISTERNA = 18;

// Pines Tanque (Arriba)
const int PIN_TRIG_TANQUE = 23;
const int PIN_ECHO_TANQUE = 22;

// Pin del Motor
const int PIN_MOTOR = 2;

// --- CONFIGURACIÓN DEL SISTEMA ---
const int CAPACIDAD_CISTERNA_FALLBACK_LITROS = 100;
const int CAPACIDAD_TANQUE_FALLBACK_LITROS = 100;

// Valores geométricos y de control por defecto
const int DIAMETRO_CISTERNA_DEF_CM = 50;
const int ALTURA_CISTERNA_DEF_CM = 90;
const int DIAMETRO_TANQUE_DEF_CM = 50;
const int ALTURA_TANQUE_DEF_CM = 90;
const float CAUDAL_BOMBA_DEF_LPM = 20.0f;

// Nuevos parámetros de control unificados en porcentajes
const int TANQUE_ARRANQUE_DEF_PORCENTAJE = 10;   // Default 10%
const int TANQUE_CORTE_DEF_PORCENTAJE = 95;      // Default 95%
const int CISTERNA_RESERVA_DEF_PORCENTAJE = 10;  // Default 10%
const int CISTERNA_MAX_DEF_PORCENTAJE = 95;      // Default 95%

const int CAPTURAS_PROMEDIO_DEF = 10;
const int CAPTURAS_PROMEDIO_MAX = 50;
const int TIEMPO_ESPERA_ENTRE_ARRANQUES_DEF_MIN = 60;
const int TIEMPO_ESPERA_ENTRE_ARRANQUES_MIN_MIN = 5;
const int TIEMPO_ESPERA_ENTRE_ARRANQUES_MAX_MIN = 240;

int diametroCisternaCm = DIAMETRO_CISTERNA_DEF_CM;
int alturaCisternaCm = ALTURA_CISTERNA_DEF_CM;
int diametroTanqueCm = DIAMETRO_TANQUE_DEF_CM;
int alturaTanqueCm = ALTURA_TANQUE_DEF_CM;
float caudalBombaLpm = CAUDAL_BOMBA_DEF_LPM;

int tanqueArranquePorcentaje = TANQUE_ARRANQUE_DEF_PORCENTAJE;
int tanqueCortePorcentaje = TANQUE_CORTE_DEF_PORCENTAJE;
int cisternaReservaPorcentaje = CISTERNA_RESERVA_DEF_PORCENTAJE;
int cisternaMaxPorcentaje = CISTERNA_MAX_DEF_PORCENTAJE;

int capturasPromedio = CAPTURAS_PROMEDIO_DEF;
int tiempoEsperaEntreArranquesMin = TIEMPO_ESPERA_ENTRE_ARRANQUES_DEF_MIN;

int muestrasCisterna[CAPTURAS_PROMEDIO_MAX] = {0};
int muestrasTanque[CAPTURAS_PROMEDIO_MAX] = {0};
long sumaMuestrasCisterna = 0;
long sumaMuestrasTanque = 0;
int indiceMuestra = 0;
int cantidadMuestras = 0;

const unsigned long TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS = 10UL * 60UL * 1000UL;
const unsigned long TIEMPO_SEGURIDAD_MINIMO_CALCULADO_MS = 60UL * 1000UL;
const float FACTOR_SEGURIDAD_BOMBA = 1.2f;

// --- VARIABLES DE ESTADO ---
bool motorEncendido = false;
bool bloqueoSeguridad = false;
bool reiniciarParaConfigWiFi = false;
bool releManualDirecto = false;
bool pedidoArranqueAutomatico = false;
unsigned long tiempoInicioBomba = 0;
unsigned long tiempoFuncionando = 0;
unsigned long tiempoMaximoBombaMs = TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
unsigned long proximoArranquePermitidoMs = 0;
String accesoWebIp = "";
String accesoWebMdns = "http://tanque.local/";
unsigned long ultimaSalidaSerialSensores = 0;
const unsigned long INTERVALO_SERIAL_SENSORES_MS = 1000;

int modoManual = 0;

WiFiManager wifiManager;
Preferences preferences;
String mensajeConfig = "";

// --- LOGS Y CONSUMO DE AGUA ---
const int MAX_LOGS = 40;
const unsigned long INTERVALO_CONSUMO_MS = 30000;

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
  preferences.begin("consumo", false);
  preferences.putBytes("hist24", histConsumoLitros, sizeof(histConsumoLitros));
  preferences.end();
}

void cargarHistorialConsumo() {
  preferences.begin("consumo", true);
  size_t leidos = preferences.getBytes("hist24", histConsumoLitros, sizeof(histConsumoLitros));
  preferences.end();

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

void borrarPersistenciaConsumo() {
  preferences.begin("consumo", false);
  preferences.remove("hist24");
  preferences.end();
  borrarHistorialConsumo();
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
  if (inicio < 0) return "";
  int fin = request.indexOf(' ', inicio + 5);
  if (fin < 0) return "";
  return request.substring(inicio + 5, fin);
}

String obtenerParametroConsulta(const String& ruta, const String& nombre) {
  int pregunta = ruta.indexOf('?');
  if (pregunta < 0) return "";
  String consulta = ruta.substring(pregunta + 1);
  String marca = nombre + "=";
  int inicio = consulta.indexOf(marca);
  if (inicio < 0) return "";
  inicio += marca.length();
  int fin = consulta.indexOf('&', inicio);
  if (fin < 0) fin = consulta.length();
  String valor = consulta.substring(inicio, fin);
  valor.replace("+", " ");
  return valor;
}

int leerParametroEntero(const String& ruta, const String& nombre, int valorActual, int minimo, int maximo) {
  String valor = obtenerParametroConsulta(ruta, nombre);
  if (valor.length() == 0) return valorActual;
  return constrain(valor.toInt(), minimo, maximo);
}

float leerParametroFloat(const String& ruta, const String& nombre, float valorActual, float minimo, float maximo) {
  String valor = obtenerParametroConsulta(ruta, nombre);
  if (valor.length() == 0) return valorActual;
  float nuevoValor = valor.toFloat();
  return constrain(nuevoValor, minimo, maximo);
}

int calcularCapacidadCilindricaLitros(int diametroCm, int alturaCm, int capacidadFallbackLitros) {
  if (diametroCm <= 0 || alturaCm <= 0) return capacidadFallbackLitros;
  float radioCm = diametroCm / 2.0f;
  float volumenCm3 = 3.14159265f * radioCm * radioCm * alturaCm;
  int litros = (int)(volumenCm3 / 1000.0f + 0.5f);
  return (litros <= 0) ? capacidadFallbackLitros : litros;
}

// NUEVA FUNCIÓN: Traduce el porcentaje deseado a distancia física en cm basándose en la altura total
int calcularDistanciaDesdePorcentaje(int porcentaje, int alturaTotalCm) {
  if (alturaTotalCm <= 0) return 0;
  float factorVacío = (100.0f - porcentaje) / 100.0f;
  return (int)(alturaTotalCm * factorVacío + 0.5f);
}

// Nueva lectura limpia del porcentaje según la lectura actual del sensor y la física del contenedor
int calcularPorcentajeDesdeMedicion(int distanciaCm, int alturaTotalCm) {
  if (alturaTotalCm <= 0) return 0;
  int distanciaAjustada = constrain(distanciaCm, 0, alturaTotalCm);
  float pct = ((float)(alturaTotalCm - distanciaAjustada) / alturaTotalCm) * 100.0f;
  return constrain((int)(pct + 0.5f), 0, 100);
}

void reiniciarPromedioSensores() {
  sumaMuestrasCisterna = 0;
  sumaMuestrasTanque = 0;
  indiceMuestra = 0;
  cantidadMuestras = 0;
}

unsigned long obtenerCooldownAnticicloMs() {
  int minutos = constrain(tiempoEsperaEntreArranquesMin, TIEMPO_ESPERA_ENTRE_ARRANQUES_MIN_MIN, TIEMPO_ESPERA_ENTRE_ARRANQUES_MAX_MIN);
  return (unsigned long)minutos * 60UL * 1000UL;
}

unsigned long obtenerCooldownRestanteMs() {
  if (proximoArranquePermitidoMs == 0) return 0;
  unsigned long ahora = millis();
  if (ahora >= proximoArranquePermitidoMs) return 0;
  return proximoArranquePermitidoMs - ahora;
}

String obtenerTextoEstadoBomba() {
  if (bloqueoSeguridad) return "BLOQUEO DE SEGURIDAD";
  unsigned long cooldownRestanteMs = obtenerCooldownRestanteMs();
  if (cooldownRestanteMs > 0 && !motorEncendido && modoManual == 0 && !releManualDirecto) {
    return "EN COOLDOWN - restan " + String((cooldownRestanteMs / 1000UL) / 60) + " min " + String((cooldownRestanteMs / 1000UL) % 60) + " seg";
  }
  if (motorEncendido) return "BOMBEANDO AGUA...";
  return "MOTOR APAGADO";
}

bool guardarConfiguracionDesdeRuta(const String& ruta, String& error) {
  int nuevoDiametroTanqueCm = leerParametroEntero(ruta, "diam_tanque", diametroTanqueCm, 0, 500);
  int nuevaAlturaTanqueCm = leerParametroEntero(ruta, "alto_tanque", alturaTanqueCm, 1, 500);
  int nuevoDiametroCisternaCm = leerParametroEntero(ruta, "diam_cisterna", diametroCisternaCm, 0, 500);
  int nuevaAlturaCisternaCm = leerParametroEntero(ruta, "alto_cisterna", alturaCisternaCm, 1, 500);
  float nuevoCaudalBombaLpm = leerParametroFloat(ruta, "caudal_bomba", caudalBombaLpm, 0.1f, 9999.0f);
  
  int nuevoTanqueArranquePorcentaje = leerParametroEntero(ruta, "tanque_arranque_pct", tanqueArranquePorcentaje, 0, 100);
  int nuevoTanqueCortePorcentaje = leerParametroEntero(ruta, "tanque_corte_pct", tanqueCortePorcentaje, 0, 100);
  int nuevaCisternaReservaPct = leerParametroEntero(ruta, "cisterna_reserva_pct", cisternaReservaPorcentaje, 0, 100);
  int nuevaCisternaMaxPct = leerParametroEntero(ruta, "cisterna_max_pct", cisternaMaxPorcentaje, 0, 100);
  
  int nuevasCapturasPromedio = leerParametroEntero(ruta, "capturas_promedio", capturasPromedio, 1, CAPTURAS_PROMEDIO_MAX);
  int nuevoCooldownAnticicloMin = leerParametroEntero(ruta, "cooldown_anticiclo_min", tiempoEsperaEntreArranquesMin, TIEMPO_ESPERA_ENTRE_ARRANQUES_MIN_MIN, TIEMPO_ESPERA_ENTRE_ARRANQUES_MAX_MIN);

  if (nuevoTanqueArranquePorcentaje >= nuevoTanqueCortePorcentaje) { error = "El % de arranque del tanque debe ser menor que el % de corte."; return false; }
  if (nuevaCisternaReservaPct >= nuevaCisternaMaxPct) { error = "La reserva de la cisterna debe ser menor al % máximo de la misma."; return false; }

  diametroTanqueCm = nuevoDiametroTanqueCm; alturaTanqueCm = nuevaAlturaTanqueCm;
  diametroCisternaCm = nuevoDiametroCisternaCm; alturaCisternaCm = nuevaAlturaCisternaCm;
  caudalBombaLpm = nuevoCaudalBombaLpm; 
  tanqueArranquePorcentaje = nuevoTanqueArranquePorcentaje; tanqueCortePorcentaje = nuevoTanqueCortePorcentaje;
  cisternaReservaPorcentaje = nuevaCisternaReservaPct; cisternaMaxPorcentaje = nuevaCisternaMaxPct;
  tiempoEsperaEntreArranquesMin = nuevoCooldownAnticicloMin;
  
  if (capturasPromedio != nuevasCapturasPromedio) { capturasPromedio = nuevasCapturasPromedio; reiniciarPromedioSensores(); }

  preferences.begin("tanquecfg", false);
  preferences.putInt("diam_tanque", diametroTanqueCm);
  preferences.putInt("alto_tanque", alturaTanqueCm);
  preferences.putInt("diam_cisterna", diametroCisternaCm);
  preferences.putInt("alto_cisterna", alturaCisternaCm);
  preferences.putFloat("caudal_bomba", caudalBombaLpm);
  preferences.putInt("tanque_arranque_pct", tanqueArranquePorcentaje);
  preferences.putInt("tanque_corte_pct", tanqueCortePorcentaje);
  preferences.putInt("cisterna_reserva_pct", cisternaReservaPorcentaje);
  preferences.putInt("cisterna_max_pct", cisternaMaxPorcentaje);
  preferences.putInt("cooldown_anticiclo_min", tiempoEsperaEntreArranquesMin);
  preferences.putInt("capt_prom", capturasPromedio);
  preferences.end();
  error = "";
  return true;
}

void cargarConfiguracion() {
  preferences.begin("tanquecfg", true);
  diametroTanqueCm = preferences.getInt("diam_tanque", DIAMETRO_TANQUE_DEF_CM);
  alturaTanqueCm = preferences.getInt("alto_tanque", ALTURA_TANQUE_DEF_CM); 
  diametroCisternaCm = preferences.getInt("diam_cisterna", DIAMETRO_CISTERNA_DEF_CM); 
  alturaCisternaCm = preferences.getInt("alto_cisterna", ALTURA_CISTERNA_DEF_CM);
  caudalBombaLpm = preferences.getFloat("caudal_bomba", CAUDAL_BOMBA_DEF_LPM); 
  tanqueArranquePorcentaje = preferences.getInt("tanque_arranque_pct", TANQUE_ARRANQUE_DEF_PORCENTAJE); 
  tanqueCortePorcentaje = preferences.getInt("tanque_corte_pct", TANQUE_CORTE_DEF_PORCENTAJE); 
  cisternaReservaPorcentaje = preferences.getInt("cisterna_reserva_pct", CISTERNA_RESERVA_DEF_PORCENTAJE); 
  cisternaMaxPorcentaje = preferences.getInt("cisterna_max_pct", CISTERNA_MAX_DEF_PORCENTAJE);
  tiempoEsperaEntreArranquesMin = constrain(preferences.getInt("cooldown_anticiclo_min", TIEMPO_ESPERA_ENTRE_ARRANQUES_DEF_MIN), TIEMPO_ESPERA_ENTRE_ARRANQUES_MIN_MIN, TIEMPO_ESPERA_ENTRE_ARRANQUES_MAX_MIN);
  capturasPromedio = constrain(preferences.getInt("capt_prom", CAPTURAS_PROMEDIO_DEF), 1, CAPTURAS_PROMEDIO_MAX);
  preferences.end();
}

void restaurarValoresPorDefecto() {
  diametroCisternaCm = DIAMETRO_CISTERNA_DEF_CM; alturaCisternaCm = ALTURA_CISTERNA_DEF_CM;
  diametroTanqueCm = DIAMETRO_TANQUE_DEF_CM; alturaTanqueCm = ALTURA_TANQUE_DEF_CM;
  caudalBombaLpm = CAUDAL_BOMBA_DEF_LPM; 
  tanqueArranquePorcentaje = TANQUE_ARRANQUE_DEF_PORCENTAJE; tanqueCortePorcentaje = TANQUE_CORTE_DEF_PORCENTAJE;
  cisternaReservaPorcentaje = CISTERNA_RESERVA_DEF_PORCENTAJE; cisternaMaxPorcentaje = CISTERNA_MAX_DEF_PORCENTAJE;
  tiempoEsperaEntreArranquesMin = TIEMPO_ESPERA_ENTRE_ARRANQUES_DEF_MIN;
  capturasPromedio = CAPTURAS_PROMEDIO_DEF;
  reiniciarPromedioSensores(); proximoArranquePermitidoMs = 0; pedidoArranqueAutomatico = false;
}

void borrarTodoPersistido() {
  preferences.begin("tanquecfg", false); preferences.clear(); preferences.end();
  preferences.begin("consumo", false); preferences.clear(); preferences.end();
  restaurarValoresPorDefecto(); borrarHistorialConsumo();
  logInicio = 0; logCantidad = 0; mensajeConfig = "Todo fue restablecido a valores por defecto.";
}

unsigned long calcularTiempoMaximoBombaMs(int litrosUtiles) {
  if (caudalBombaLpm <= 0.0f || litrosUtiles <= 0) return TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
  unsigned long estimadoMs = (unsigned long)((litrosUtiles / caudalBombaLpm) * 60000.0f * FACTOR_SEGURIDAD_BOMBA);
  return max(estimadoMs, TIEMPO_SEGURIDAD_MINIMO_CALCULADO_MS);
}

int distCisterna = 0;
int distTanque = 0;
WiFiServer server(80);

int medirDistancia(int pinTrig, int pinEcho) {
  digitalWrite(pinTrig, LOW); delayMicroseconds(2);
  digitalWrite(pinTrig, HIGH); delayMicroseconds(10);
  digitalWrite(pinTrig, LOW);
  long duracion = pulseIn(pinEcho, HIGH, 30000);
  if (duracion == 0) return 999;
  return duracion * 0.034 / 2;
}

void agregarMuestrasSensores(int lecturaCisterna, int lecturaTanque) {
  if (cantidadMuestras >= capturasPromedio) {
    sumaMuestrasCisterna -= muestrasCisterna[indiceMuestra];
    sumaMuestrasTanque -= muestrasTanque[indiceMuestra];
  } else {
    cantidadMuestras++;
  }
  muestrasCisterna[indiceMuestra] = lecturaCisterna;
  muestrasTanque[indiceMuestra] = lecturaTanque;
  sumaMuestrasCisterna += lecturaCisterna;
  sumaMuestrasTanque += lecturaTanque;
  indiceMuestra = (indiceMuestra + 1) % capturasPromedio;

  distCisterna = (int)(sumaMuestrasCisterna / cantidadMuestras);
  distTanque = (int)(sumaMuestrasTanque / cantidadMuestras);
}

void setup() {
  Serial.begin(115200);
  cargarConfiguracion();
  cargarHistorialConsumo();

  pinMode(PIN_TRIG_CISTERNA, OUTPUT); pinMode(PIN_ECHO_CISTERNA, INPUT);
  pinMode(PIN_TRIG_TANQUE, OUTPUT); pinMode(PIN_ECHO_TANQUE, INPUT);
  pinMode(PIN_MOTOR, OUTPUT); digitalWrite(PIN_MOTOR, LOW);

  wifiManager.setShowStaticFields(true);
  wifiManager.setShowDnsFields(true);
  
  Serial.println("Iniciando AutoConnect...");
  if (!wifiManager.autoConnect("Tanque_Cisterna")) {
    Serial.println("Error al conectar. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("\n¡Wi-Fi Conectado exitosamente!");
  accesoWebIp = "http://" + WiFi.localIP().toString() + "/";
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  registrarEvento("BOOT_OK", WiFi.localIP().toString());
  
  server.begin();
  MDNS.begin("tanque");
}

void loop() {
  int lecturaCisterna = medirDistancia(PIN_TRIG_CISTERNA, PIN_ECHO_CISTERNA);
  int lecturaTanque = medirDistancia(PIN_TRIG_TANQUE, PIN_ECHO_TANQUE);
  agregarMuestrasSensores(lecturaCisterna, lecturaTanque);

  int capacidadCisternaLitros = calcularCapacidadCilindricaLitros(diametroCisternaCm, alturaCisternaCm, CAPACIDAD_CISTERNA_FALLBACK_LITROS);
  int capacidadTanqueLitros = calcularCapacidadCilindricaLitros(diametroTanqueCm, alturaTanqueCm, CAPACIDAD_TANQUE_FALLBACK_LITROS);

  // Mapeos dinámicos basados exclusivamente en la altura y lectura de los sensores
  int pctCisterna = calcularPorcentajeDesdeMedicion(distCisterna, alturaCisternaCm);
  int pctTanque = calcularPorcentajeDesdeMedicion(distTanque, alturaTanqueCm);

  int litrosCisterna = (int)((pctCisterna / 100.0f) * capacidadCisternaLitros);
  int litrosTanque = (int)((pctTanque / 100.0f) * capacidadTanqueLitros);

  int reservaLitrosCisterna = (int)(capacidadCisternaLitros * (cisternaReservaPorcentaje / 100.0f));
  int litrosDisponiblesCisterna = max(litrosCisterna - reservaLitrosCisterna, 0);
  int litrosCorteTanque = (int)(capacidadTanqueLitros * (tanqueCortePorcentaje / 100.0f));
  int litrosParaObjetivoTanque = max(litrosCorteTanque - litrosTanque, 0);
  
  actualizarHistogramaConsumo(litrosTanque);

  if (millis() - ultimaSalidaSerialSensores >= INTERVALO_SERIAL_SENSORES_MS) {
    ultimaSalidaSerialSensores = millis();
    Serial.print("Tanque: "); Serial.print(pctTanque); Serial.print("% | Cisterna: "); Serial.print(pctCisterna); Serial.println("%");
  }

  // --- LÓGICA AUTOMÁTICA MEJORADA UNIFICADA ---
  bool promedioListo = (cantidadMuestras >= capturasPromedio);
  bool cisternaTieneAgua = promedioListo && (pctCisterna > cisternaReservaPorcentaje);
  bool tanqueNecesitaCarga = promedioListo && (pctTanque <= tanqueArranquePorcentaje);
  bool tanqueAlcanzoObjetivo = (pctTanque >= tanqueCortePorcentaje); // Único criterio inequívoco
  bool enEsperaAnticiclo = (proximoArranquePermitidoMs > 0 && millis() < proximoArranquePermitidoMs);
  bool anticicloActivo = enEsperaAnticiclo;
  bool motorDeberiaEncender = motorEncendido;

  if (pctTanque > tanqueArranquePorcentaje && pctTanque < tanqueCortePorcentaje && !motorEncendido) {
    bloqueoSeguridad = false;
  }

  if (!cisternaTieneAgua || tanqueAlcanzoObjetivo || enEsperaAnticiclo) {
    motorDeberiaEncender = false; 
  } else if (tanqueNecesitaCarga) {
    motorDeberiaEncender = true;
  }

  if (releManualDirecto) {
    motorEncendido = true;
  } else if (bloqueoSeguridad) {
    motorEncendido = false;
  } else if (modoManual == 1) {
    motorEncendido = cisternaTieneAgua;
  } else if (modoManual == 2) {
    motorEncendido = false;
  } else {
    if (motorEncendido && !motorDeberiaEncender) motorEncendido = false;
    else if (!motorEncendido && motorDeberiaEncender && !enEsperaAnticiclo) pedidoArranqueAutomatico = true;
    
    if (pedidoArranqueAutomatico && !enEsperaAnticiclo) {
      motorEncendido = true;
      pedidoArranqueAutomatico = false;
    }
  }

  if (anticicloActivo) {
    motorEncendido = false; releManualDirecto = false; modoManual = 0; pedidoArranqueAutomatico = false;
  }

  if (motorEncendido) {
    if (tiempoInicioBomba == 0) {
      tiempoInicioBomba = millis();
      int litrosUtilesCalculo = max(litrosDisponiblesCisterna, 1);
      if (!releManualDirecto && modoManual == 0) {
        litrosUtilesCalculo = min(litrosDisponiblesCisterna, max(litrosParaObjetivoTanque, 1));
      }
      tiempoMaximoBombaMs = calcularTiempoMaximoBombaMs(litrosUtilesCalculo);
    }
    tiempoFuncionando = millis() - tiempoInicioBomba;
    if (tiempoFuncionando >= tiempoMaximoBombaMs) {
      bloqueoSeguridad = true; motorEncendido = false; modoManual = 0; releManualDirecto = false;
    }
  } else {
    bool estabaEncendida = (tiempoInicioBomba != 0);
    tiempoInicioBomba = 0; tiempoFuncionando = 0; tiempoMaximoBombaMs = TIEMPO_SEGURIDAD_MINIMO_BOMBA_MS;
    if (estabaEncendida && !releManualDirecto && modoManual == 0) {
      proximoArranquePermitidoMs = millis() + obtenerCooldownAnticicloMs();
    }
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
            bool enPaginaConfig = ruta.startsWith("config");
            bool enPaginaLog = ruta.startsWith("log");
            bool configGuardada = false;

            if (ruta.startsWith("ENCENDER")) {
              if (obtenerCooldownRestanteMs() == 0) {
                releManualDirecto = true; modoManual = 1; bloqueoSeguridad = false; pedidoArranqueAutomatico = false;
                registrarEvento("ENCENDER_MANUAL", ipCliente);
              } else { mensajeConfig = "No se permite encender durante el cooldown."; }
            }
            if (ruta.startsWith("APAGAR")) {
              releManualDirecto = false; modoManual = 2; pedidoArranqueAutomatico = false;
              registrarEvento("APAGAR", ipCliente);
            }
            if (ruta.startsWith("AUTO")) {
              releManualDirecto = false; modoManual = 0; bloqueoSeguridad = false; proximoArranquePermitidoMs = 0; pedidoArranqueAutomatico = false;
              registrarEvento("AUTO_RESET", ipCliente);
              client.println("HTTP/1.1 302 Found\r\nLocation: /\r\nConnection: close\r\n\r\n");
              break;
            }
            if (ruta.startsWith("config/cambiar_wifi")) {
              reiniciarParaConfigWiFi = true; wifiManager.resetSettings(); registrarEvento("CAMBIAR_WIFI", ipCliente);
            }
            if (ruta.startsWith("config/borrar_consumo")) {
              borrarPersistenciaConsumo(); configGuardada = true; registrarEvento("BORRAR_CONSUMO", ipCliente);
            }
            if (ruta.startsWith("config/guardar")) {
              String errorGuardar = "";
              if (guardarConfiguracionDesdeRuta(ruta, errorGuardar)) { configGuardada = true; registrarEvento("GUARDAR_CONFIG", ipCliente); }
              else { mensajeConfig = errorGuardar; }
            }
            if (ruta.startsWith("config/borrar_datos")) {
              borrarTodoPersistido(); configGuardada = true; registrarEvento("BORRAR_DATOS", ipCliente);
            }

            if (ruta == "api/estado") {
              String modoApi = releManualDirecto ? "MANUAL DIRECTO" : ((modoManual == 0) ? "AUTOMATICO" : ((modoManual == 1) ? "MANUAL ENCENDIDO" : "MANUAL APAGADO"));
              client.println("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
              client.print("{\"distTanque\":"); client.print(distTanque);
              client.print(",\"pctTanque\":"); client.print(pctTanque);
              client.print(",\"litrosTanque\":"); client.print(litrosTanque);
              client.print(",\"distCisterna\":"); client.print(distCisterna);
              client.print(",\"pctCisterna\":"); client.print(pctCisterna);
              client.print(",\"litrosCisterna\":"); client.print(litrosCisterna);
              client.print(",\"motorEncendido\":"); client.print(motorEncendido ? "true" : "false");
              client.print(",\"bloqueo\":"); client.print(bloqueoSeguridad ? "true" : "false");
              client.print(",\"tiempoActivoSeg\":"); client.print(tiempoFuncionando / 1000UL);
              client.print(",\"limiteEstimadoSeg\":"); client.print(tiempoMaximoBombaMs / 1000UL);
              client.print(",\"cooldownMin\":"); client.print(tiempoEsperaEntreArranquesMin);
              client.print(",\"cooldownRestanteSeg\":"); client.print(obtenerCooldownRestanteMs() / 1000UL);
              client.print(",\"estadoBomba\":\""); client.print(obtenerTextoEstadoBomba());
              client.print("\",\"modo\":\""); client.print(modoApi);
              client.print("\",\"uptime\":"); client.print(millis() / 1000UL);
              client.println("}");
              break;
            }

            client.println("HTTP/1.1 200 OK\r\nContent-type: text/html; charset=UTF-8\r\nConnection: close\r\n\r\n");
            client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Monitor de Agua ESP32</title>");
            client.println("<style>body{font-family:Arial,sans-serif;text-align:center;background:#f4f4f9;padding:10px;} .card{background:white;padding:15px;border-radius:15px;margin:12px auto;max-width:400px;box-shadow:0 4px 8px rgba(0,0,0,0.1);} h1{color:#0288d1;} .status{font-weight:bold;padding:10px;border-radius:5px;margin-top:10px;} .on{background:#c8e6c9;color:#256029;} .off{background:#ffcdd2;color:#c62828;} .alert{background:#ffe0b2;color:#e65100;} .val{font-size:24px;color:#0288d1;font-weight:bold;} .btn{display:inline-block;width:125px;box-sizing:border-box;text-align:center;padding:10px 5px;margin:5px;font-size:14px;font-weight:bold;color:white;text-decoration:none;border-radius:5px;box-shadow:0 2px 4px rgba(0,0,0,0.2);} .btn-full{width:100%; display:block; margin: 10px 0;} .btn-on{background:#4caf50;} .btn-off{background:#f44336;} .btn-auto{background:#2196f3;} .btn-warn{background:#d32f2f;} .btn-dark{background:#455a64;} .modal{display:none;position:fixed;z-index:999;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.55);} .modal-content{background:#fff;border-radius:12px;max-width:360px;margin:15% auto;padding:16px;box-shadow:0 8px 20px rgba(0,0,0,0.25);} .modal-title{margin:0 0 8px 0;color:#b71c1c;font-size:18px;} .modal-text{margin:0 0 12px 0;color:#333;font-size:14px;line-height:1.35;}</style>");
            client.println("<script>var accionModal='manual';function openConfirmModal(tipo){accionModal=tipo||'manual';var t=document.getElementById('confirmModalTitle');var m=document.getElementById('confirmModalText');var a=document.getElementById('confirmModalAccept');if(accionModal==='reset'){if(t)t.textContent='⚠️ Confirmar reset';if(m)m.textContent='Vas a ejecutar AUTO / RESET. Esto limpia el modo manual y el cooldown anti-ciclo para volver a probar.';if(a)a.textContent='RESET';}else{if(t)t.textContent='⚠️ Confirmar arranque manual';if(m)m.textContent='Vas a intentar encender la bomba en modo manual. Verifica niveles de agua y estado general antes de continuar.';if(a)a.textContent='ACEPTAR';}document.getElementById('confirmModal').style.display='block';}function closeConfirmModal(){document.getElementById('confirmModal').style.display='none';}function confirmarAccionModal(){if(accionModal==='reset'){window.location.assign('/AUTO');}else{window.location.assign('/ENCENDER');}}function poner(id,v){var e=document.getElementById(id);if(e)e.textContent=v;}function formatoTiempo(s){s=Number(s)||0;return Math.floor(s/60)+' min '+(s%60)+' seg';}function actualizarEstado(){fetch('/api/estado',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){poner('pctTanque',d.pctTanque+' %');poner('distTanque',d.distTanque+' cm');poner('litrosTanque',d.litrosTanque+' Litros');poner('pctCisterna',d.pctCisterna+' %');poner('distCisterna',d.distCisterna+' cm');poner('litrosCisterna',d.litrosCisterna+' Litros');poner('modoActual','Modo actual: '+d.modo);poner('tiempoActivo',formatoTiempo(d.tiempoActivoSeg));poner('limiteEstimado',formatoTiempo(d.limiteEstimadoSeg));var estado=document.getElementById('estadoBomba');if(estado){estado.textContent=d.estadoBomba;estado.className='status '+(d.bloqueo?'off alert':(d.motorEncendido?'on':'off'));}var tiempos=document.getElementById('tiemposBomba');if(tiempos)tiempos.style.display=d.motorEncendido?'block':'none';});}document.addEventListener('DOMContentLoaded',function(){actualizarEstado();setInterval(actualizarEstado,1000);});</script>");
            client.println("</head><body>");
            client.println("<h1>💧 Control de Agua 💧</h1>");

            if (!enPaginaConfig && !enPaginaLog) {
              // --- VISTA HOME (/) ---
              client.println("<div class='card'><h2>Tanque Superior (" + String(capacidadTanqueLitros) + "L)</h2><p id='pctTanque' class='val'>" + String(pctTanque) + " %</p><p>Distancia actual: <strong id='distTanque'>" + String(distTanque) + " cm</strong></p><p>Aprox: <strong id='litrosTanque'>" + String(litrosTanque) + " Litros</strong></p></div>");
              client.println("<div class='card'><h2>Cisterna (" + String(capacidadCisternaLitros) + "L)</h2><p id='pctCisterna' class='val'>" + String(pctCisterna) + " %</p><p>Distancia actual: <strong id='distCisterna'>" + String(distCisterna) + " cm</strong></p><p>Aprox: <strong id='litrosCisterna'>" + String(litrosCisterna) + " Litros</strong></p></div>");
              
              client.println("<div class='card'><h2>Estado de la Bomba</h2>");
              String estadoBombaTexto = obtenerTextoEstadoBomba();
              bool estadoBombaBloqueo = estadoBombaTexto.startsWith("BLOQUEO DE SEGURIDAD");
              String claseEstadoBomba = estadoBombaBloqueo ? "status off alert" : (motorEncendido ? "status on" : "status off");
              client.println("<div id='estadoBomba' class='" + claseEstadoBomba + "'>" + estadoBombaTexto + "</div>");
              client.println("<div id='tiemposBomba' style='display:" + String(motorEncendido ? "block" : "none") + ";'>");
              client.println("<p style='margin-top:10px;'>Tiempo activo: <strong id='tiempoActivo'>" + String((tiempoFuncionando / 1000UL) / 60) + " min " + String((tiempoFuncionando / 1000UL) % 60) + " seg</strong></p>");
              client.println("<p style='margin-top:6px;'>Límite estimado: <strong id='limiteEstimado'>" + String((tiempoMaximoBombaMs / 1000UL) / 60) + " min " + String((tiempoMaximoBombaMs / 1000UL) % 60) + " seg</strong></p>");
              client.println("</div>");
              String modoTexto = releManualDirecto ? "MANUAL DIRECTO" : ((modoManual == 0) ? "AUTOMÁTICO" : ((modoManual == 1) ? "MANUAL (ENCENDIDO)" : "MANUAL (APAGADO)"));
              client.println("<p id='modoActual' style='font-size:12px; color:#666;'>Modo actual: " + modoTexto + "</p></div>");

              client.println("<div class='card'><h2>Controles Manuales</h2>");
              client.println("<a href='#' onclick=\"openConfirmModal('manual');return false;\" class='btn btn-on'>ARRANQUE</a>");
              client.println("<a href='/APAGAR' class='btn btn-off'>PARADA</a>");
              client.println("<a href='#' onclick=\"openConfirmModal('reset');return false;\" class='btn btn-auto'>AUTO / RESET</a></div>");
              client.println("<div class='card'><a href='/config' class='btn btn-dark btn-full'>IR A CONFIGURACIÓN</a></div>");
            }

            if (enPaginaConfig) {
              // --- VISTA CONFIGURACIÓN (/config) ---
              client.println("<div class='card'><h2>Datos de Conexión</h2>");
              client.println("<p style='margin:6px 0; text-align:left;'>IP local: <strong>" + accesoWebIp + "</strong></p>");
              client.println("<p style='margin:6px 0; text-align:left;'>mDNS: <strong>" + accesoWebMdns + "</strong></p></div>");

              client.println("<div class='card'><h2>Configuración Actual</h2>");
              if (configGuardada) client.println("<p style='color:#1b5e20; font-weight:bold;'>Configuración guardada.</p>");
              if (mensajeConfig.length() > 0) { client.println("<p style='color:#b71c1c; font-weight:bold;'>" + mensajeConfig + "</p>"); mensajeConfig = ""; }
              
              client.println("<p style='text-align:left;'>Capacidad tanque: <strong>" + String(capacidadTanqueLitros) + " L</strong></p>");
              client.println("<p style='text-align:left;'>Capacidad cisterna: <strong>" + String(capacidadCisternaLitros) + " L</strong></p>");
              client.println("<p style='text-align:left;'>Arranque tanque: <strong>" + String(tanqueArranquePorcentaje) + "%</strong></p>");
              client.println("<p style='text-align:left;'>Corte tanque: <strong>" + String(tanqueCortePorcentaje) + "%</strong></p>");
              client.println("<p style='text-align:left;'>Reserva cisterna: <strong>" + String(cisternaReservaPorcentaje) + "%</strong></p>");
              client.println("<p style='text-align:left;'>Máximo cisterna: <strong>" + String(cisternaMaxPorcentaje) + "%</strong></p>");
              client.println("<br><a href='/' class='btn btn-dark'>VOLVER AL HOME</a>");
              client.println("<a href='/log' class='btn btn-auto'>VER LOG</a>");
              client.println("<a href='/config/cambiar_wifi' class='btn btn-warn'>CAMBIAR WIFI</a>");
              client.println("<a href='/config/borrar_consumo' class='btn btn-warn' onclick=\"return confirm('¿Borrar historial?');\">BORRAR CONSUMO</a></div>");

              // Formulario limpio y simplificado (Cero distancias en cm)
              client.println("<div class='card'><h2>Modificar Parámetros</h2><form action='/config/guardar' method='GET'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Diámetro tanque (cm)</p><input type='number' name='diam_tanque' value='" + String(diametroTanqueCm) + "' min='0' max='500' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Altura tanque (cm)</p><input type='number' name='alto_tanque' value='" + String(alturaTanqueCm) + "' min='1' max='500' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Diámetro cisterna (cm)</p><input type='number' name='diam_cisterna' value='" + String(diametroCisternaCm) + "' min='0' max='500' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Altura cisterna (cm)</p><input type='number' name='alto_cisterna' value='" + String(alturaCisternaCm) + "' min='1' max='500' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Caudal bomba (L/min)</p><input type='number' step='0.1' name='caudal_bomba' value='" + String(caudalBombaLpm, 1) + "' min='0.1' max='9999' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Capturas promedio</p><input type='number' name='capturas_promedio' value='" + String(capturasPromedio) + "' min='1' max='" + String(CAPTURAS_PROMEDIO_MAX) + "' style='width:100%; padding:8px; box-sizing:border-box;'>");
              
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Arranque tanque (%)</p><input type='number' name='tanque_arranque_pct' value='" + String(tanqueArranquePorcentaje) + "' min='0' max='100' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Corte tanque (%)</p><input type='number' name='tanque_corte_pct' value='" + String(tanqueCortePorcentaje) + "' min='0' max='100' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Reserva cisterna (%)</p><input type='number' name='cisterna_reserva_pct' value='" + String(cisternaReservaPorcentaje) + "' min='0' max='100' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Máximo cisterna (%)</p><input type='number' name='cisterna_max_pct' value='" + String(cisternaMaxPorcentaje) + "' min='0' max='100' style='width:100%; padding:8px; box-sizing:border-box;'>");
              
              client.println("<p style='text-align:left; margin:8px 0 4px 0;'>Cooldown anti-ciclo (min)</p><input type='number' name='cooldown_anticiclo_min' value='" + String(tiempoEsperaEntreArranquesMin) + "' min='" + String(TIEMPO_ESPERA_ENTRE_ARRANQUES_MIN_MIN) + "' max='" + String(TIEMPO_ESPERA_ENTRE_ARRANQUES_MAX_MIN) + "' style='width:100%; padding:8px; box-sizing:border-box;'>");
              client.println("<div style='margin-top:12px;'><button type='submit' class='btn btn-auto' style='width:100%; border:none; cursor:pointer;'>GUARDAR CONFIGURACIÓN</button></div></form>");
              client.println("<br><a href='/config/borrar_datos' class='btn btn-warn btn-full' onclick=\"return confirm('¿Restablecer todo de fábrica?');\">RESETEAR TODO</a></div>");
            }

            if (enPaginaLog) {
              // --- VISTA LOGS (/log) ---
              client.println("<div class='card'><h2>Consumo de Agua (Últimas 24hs)</h2>");
              client.println("<div style='display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; font-size: 12px; text-align: left;'>");
              for (int i = 0; i < 24; i++) {
                if (histConsumoLitros[i] > 0) {
                  client.println("<div style='background:#f0f4f8; padding:4px; border-radius:4px;'><strong>" + String(i) + "hs:</strong> " + String(histConsumoLitros[i], 1) + "L</div>");
                }
              }
              client.println("</div>");
              float totalConsumo = 0;
              for (int i = 0; i < 24; i++) totalConsumo += histConsumoLitros[i];
              if (totalConsumo == 0) client.println("<p style='font-size:12px; color:#999;'>Sin consumos registrados en las últimas 24hs.</p>");
              client.println("</div>");

              client.println("<div class='card'><h2>Bitácora de Eventos</h2><div style='text-align:left; font-size:12px; max-height:200px; overflow:auto; border:1px solid #eee; padding:8px;'>");
              if (logCantidad == 0) client.println("<p>Sin eventos.</p>");
              else {
                for (int i = 0; i < logCantidad; i++) {
                  int idx = (logInicio + i) % MAX_LOGS;
                  client.println("<p style='margin:3px 0;'><strong>" + logs[idx].momento + "</strong> | " + logs[idx].accion + " (" + logs[idx].ip + ")</p>");
                }
              }
              client.println("</div><br><a href='/config' class='btn btn-dark'>VOLVER A CONFIG</a></div>");
            }

            // Modal de confirmación global
            client.println("<div id='confirmModal' class='modal'><div class='modal-content'><h3 id='confirmModalTitle' class='modal-title'>⚠️ Confirmar</h3>");
            client.println("<p id='confirmModalText' class='modal-text'></p>");
            client.println("<a id='confirmModalAccept' href='#' onclick='confirmarAccionModal();return false;' class='btn btn-warn'>ACEPTAR</a>");
            client.println("<a href='#' onclick='closeConfirmModal();return false;' class='btn btn-auto'>CANCELAR</a></div></div>");
            
            client.println("</body></html>");
            break;
          } else { currentLine = ""; }
        } else if (c != '\r') { currentLine += c; }
      }
    }
    client.stop();
  }

  if (reiniciarParaConfigWiFi) { delay(1000); ESP.restart(); }
  delay(10);
}