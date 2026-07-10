#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>          // La librería mágica
#include <time.h>

// Pines Cisterna (Abajo)
const int PIN_TRIG_CISTERNA = 5;
const int PIN_ECHO_CISTERNA = 18;

// Pines Tanque (Arriba)
const int PIN_TRIG_TANQUE = 23;
const int PIN_ECHO_TANQUE = 22;

// Pin del Motor
const int PIN_MOTOR = 2;

// --- CONFIGURACIÓN DE MEDIDAS (En centímetros) ---
const int CISTERNA_ALTO = 80;
const int CISTERNA_MINIMA_AGUA = 75;

const int TANQUE_ALTO = 110;
const int TANQUE_PIDE_AGUA = 90;
const int TANQUE_LLENO = 20;

// --- TIEMPOS DE SEGURIDAD ---
const unsigned long MAX_TIEMPO_BOMBA = 10 * 60 * 1000; // 10 minutos en milisegundos

// --- VARIABLES DE ESTADO ---
bool motorEncendido = false;
bool bloqueoSeguridad = false;
bool reiniciarParaConfigWiFi = false;
bool releManualDirecto = false;
unsigned long tiempoInicioBomba = 0;
unsigned long tiempoFuncionando = 0;
String accesoWebIp = "";
String accesoWebMdns = "http://tanque.local/";

// Control manual por Web (0 = Automático, 1 = Forzar Encendido, 2 = Forzar Apagado)
int modoManual = 0;

WiFiManager wifiManager;

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
  }

  ultimoLitrosTanqueMuestreo = litrosTanque;
  ultimoMuestreoConsumo = ahora;
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

  if (distCisterna > CISTERNA_ALTO) distCisterna = CISTERNA_ALTO;
  if (distTanque > TANQUE_ALTO) distTanque = TANQUE_ALTO;

  // Porcentajes y Litros
  int pctCisterna = map(distCisterna, CISTERNA_ALTO, 0, 0, 100);
  if (pctCisterna < 0) pctCisterna = 0; if (pctCisterna > 100) pctCisterna = 100;
  int litrosCisterna = map(pctCisterna, 0, 100, 0, 500);

  int pctTanque = map(distTanque, TANQUE_ALTO, 0, 0, 100);
  if (pctTanque < 0) pctTanque = 0; if (pctTanque > 100) pctTanque = 100;
  int litrosTanque = map(pctTanque, 0, 100, 0, 1000);
  actualizarHistogramaConsumo(litrosTanque);

  // --- LÓGICA AUTOMÁTICA DE LA BOMBA ---
  bool cisternaTieneAgua = (distCisterna < CISTERNA_MINIMA_AGUA);
  bool motorDeberiaEncender = motorEncendido;

  if (distTanque > TANQUE_PIDE_AGUA && !motorEncendido) {
    bloqueoSeguridad = false;
  }

  if (!cisternaTieneAgua) {
    motorDeberiaEncender = false; 
  } else if (distTanque > TANQUE_PIDE_AGUA) {
    motorDeberiaEncender = true;  
  } else if (distTanque <= TANQUE_LLENO) {
    motorDeberiaEncender = false; 
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
    }
    tiempoFuncionando = millis() - tiempoInicioBomba;

    if (tiempoFuncionando >= MAX_TIEMPO_BOMBA) {
      bloqueoSeguridad = true;
      motorEncendido = false;
      modoManual = 0;
      releManualDirecto = false;
    }
  } else {
    tiempoInicioBomba = 0;
    tiempoFuncionando = 0;
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
            bool enPaginaConfig = (request.indexOf("GET /config") != -1);
            bool enPaginaLog = (request.indexOf("GET /log") != -1);

            // --- FILTRO DE SEGURIDAD PARA BOTONES MANUALES ---
            if (request.indexOf("GET /ENCENDER") != -1) {
              releManualDirecto = true;
              modoManual = 1;
              bloqueoSeguridad = false;
              registrarEvento("ENCENDER_MANUAL", ipCliente);
            }
            if (request.indexOf("GET /APAGAR") != -1) {
              releManualDirecto = false;
              modoManual = 2;
              registrarEvento("APAGAR", ipCliente);
            }
            if (request.indexOf("GET /AUTO") != -1) {
              releManualDirecto = false;
              modoManual = 0;
              bloqueoSeguridad = false;
              registrarEvento("AUTO_RESET", ipCliente);
            }
            if (request.indexOf("GET /config/cambiar_wifi") != -1) {
              releManualDirecto = false;
              reiniciarParaConfigWiFi = true;
              wifiManager.resetSettings();
              registrarEvento("CAMBIAR_WIFI", ipCliente);
            }

            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();

            client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
            client.println("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
            client.println("<title>Monitor de Agua Pro</title>");
            client.println("<style>body{font-family:Arial,sans-serif;text-align:center;background:#f4f4f9;padding:10px;} .card{background:white;padding:15px;border-radius:15px;margin:12px auto;max-width:400px;box-shadow:0 4px 8px rgba(0,0,0,0.1);} h1{color:#0288d1;} .status{font-weight:bold;padding:10px;border-radius:5px;margin-top:10px;} .on{background:#c8e6c9;color:#256029;} .off{background:#ffcdd2;color:#c62828;} .alert{background:#ffe0b2;color:#e65100;} .val{font-size:24px;color:#0288d1;font-weight:bold;} .btn{display:inline-block;width:125px;box-sizing:border-box;text-align:center;padding:10px 5px;margin:5px;font-size:14px;font-weight:bold;color:white;text-decoration:none;border-radius:5px;box-shadow:0 2px 4px rgba(0,0,0,0.2);} .btn-on{background:#4caf50;} .btn-off{background:#f44336;} .btn-auto{background:#2196f3;} .btn-warn{background:#d32f2f;} .modal{display:none;position:fixed;z-index:999;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.55);} .modal-content{background:#fff;border-radius:12px;max-width:360px;margin:15% auto;padding:16px;box-shadow:0 8px 20px rgba(0,0,0,0.25);} .modal-title{margin:0 0 8px 0;color:#b71c1c;font-size:18px;} .modal-text{margin:0 0 12px 0;color:#333;font-size:14px;line-height:1.35;}</style>");

            client.println("<script>setInterval(function(){ if(!window.location.search) { location.reload(); } }, 3000);function openConfirmModal(){document.getElementById('confirmModal').style.display='block';document.body.style.overflow='hidden';}function closeConfirmModal(){document.getElementById('confirmModal').style.display='none';document.body.style.overflow='';}function confirmarArranqueManual(){window.location.href='/ENCENDER';}function bindModalEvents(){var m=document.getElementById('confirmModal');if(!m){return;}window.addEventListener('click',function(e){if(e.target===m){closeConfirmModal();}});document.addEventListener('keydown',function(e){if(e.key==='Escape'||e.key==='Esc'){closeConfirmModal();}});}document.addEventListener('DOMContentLoaded',bindModalEvents);</script>");
            client.println("</head><body>");
            client.println("<h1>💧 Control de Agua 💧</h1>");

            client.println("<div class='card'><h2>Tanque Superior (1000L)</h2><p class='val'>" + String(pctTanque) + " %</p><p>Aprox: <strong>" + String(litrosTanque) + " Litros</strong></p></div>");
            client.println("<div class='card'><h2>Cisterna (500L)</h2><p class='val'>" + String(pctCisterna) + " %</p><p>Aprox: <strong>" + String(litrosCisterna) + " Litros</strong></p></div>");

            client.println("<div class='card'><h2>Acceso Rápido</h2>");
            client.println("<p style='margin:6px 0;'>IP local: <strong>" + accesoWebIp + "</strong></p>");
            client.println("<p style='margin:6px 0;'>mDNS: <strong>" + accesoWebMdns + "</strong></p>");
            if (WiFi.status() == WL_CONNECTED) {
              // client.println("<p style='margin:10px 0 0 0; font-size:13px; color:#555;'>Ya estás en el panel de control.</p>");
            }
            client.println("</div>");

            client.println("<div class='card'><h2>Estado de la Bomba</h2>");
            if (bloqueoSeguridad) {
              client.println("<div class='status off alert'>🚨 BLOQUEO DE SEGURIDAD (Excedió 10 min)</div>");
            } else if (motorEncendido) {
              unsigned long seg = (tiempoFuncionando / 1000) % 60;
              unsigned long min = (tiempoFuncionando / 1000) / 60;
              client.println("<div class='status on'>BOMBEANDO AGUA...</div>");
              client.println("<p style='margin-top:10px;'>Tiempo activo: <strong>" + String(min) + " min " + String(seg) + " seg</strong></p>");
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
              client.println("<div class='card'><h2>Configuración Avanzada</h2>");
              client.println("<p style='margin:0 0 10px 0; color:#666;'>Opciones ocultas de mantenimiento.</p>");
              client.println("<a href='/log' class='btn btn-auto'>VER LOG</a>");
              client.println("<a href='/config/cambiar_wifi' class='btn btn-auto'>CAMBIAR WIFI</a>");
              client.println("<a href='/' class='btn btn-off'>VOLVER</a>");
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