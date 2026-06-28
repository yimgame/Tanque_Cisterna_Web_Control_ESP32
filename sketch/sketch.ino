#include <WiFi.h>
#include <ESPmDNS.h>

// ⚠️ CAMBIA ESTO CON LOS DATOS DE TU CASA ⚠️
//const char* ssid = "EL_NOMBRE_DE_TU_WIFI";
//const char* password = "TU_CONTRASEÑA_DEL_WIFI";

const char* ssid = "Tu Wifi";
const char* password = "Tu Password";

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
unsigned long tiempoInicioBomba = 0;
unsigned long tiempoFuncionando = 0;

// Control manual por Web (0 = Automático, 1 = Forzar Encendido, 2 = Forzar Apagado)
int modoManual = 0; 

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
  
  WiFi.setHostname("tanque");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n¡Wi-Fi Conectado!");
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

  // --- LÓGICA AUTOMÁTICA DE LA BOMBA ---
  bool cisternaTieneAgua = (distCisterna < CISTERNA_MINIMA_AGUA);
  bool motorDeberiaEncender = motorEncendido;

  // Si el tanque se vacía por debajo del nivel de arranque, quitamos el bloqueo temporal para permitir reintento
  if (distTanque > TANQUE_PIDE_AGUA && !motorEncendido) {
    bloqueoSeguridad = false; 
  }

  if (!cisternaTieneAgua) {
    motorDeberiaEncender = false; // Bloqueo físico: cisterna vacía
  } else if (distTanque > TANQUE_PIDE_AGUA) {
    motorDeberiaEncender = true;  // Falta agua arriba
  } else if (distTanque <= TANQUE_LLENO) {
    motorDeberiaEncender = false; // Se llenó arriba
  }

  // --- APLICAR MODOS MANUALES / SEGURIDAD ---
  if (bloqueoSeguridad) {
    motorEncendido = false;
  } else if (modoManual == 1) {
    motorEncendido = cisternaTieneAgua; // Forzar encendido (pero cuida que haya agua abajo)
  } else if (modoManual == 2) {
    motorEncendido = false;             // Forzar apagado total
  } else {
    motorEncendido = motorDeberiaEncender; // Modo automático normal
  }

  // --- CONTROL DE TIEMPO Y TEMPORIZADOR ---
  if (motorEncendido) {
    if (tiempoInicioBomba == 0) {
      tiempoInicioBomba = millis(); // Guarda el momento exacto en que arrancó
    }
    tiempoFuncionando = millis() - tiempoInicioBomba;

    // Si supera los 10 minutos, se apaga por seguridad
    if (tiempoFuncionando >= MAX_TIEMPO_BOMBA) {
      bloqueoSeguridad = true;
      motorEncendido = false;
      modoManual = 0; // Regresa a auto para el próximo ciclo
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
            
            // --- FILTRO DE SEGURIDAD PARA BOTONES MANUALES ---
            if (request.indexOf("GET /ENCENDER") != -1) { 
              // Solo permite cambiar a encendido manual si el tanque NO está lleno Y la cisterna SÍ tiene agua
              if (distTanque > TANQUE_LLENO && cisternaTieneAgua) {
                modoManual = 1; 
                bloqueoSeguridad = false; 
              }
            }
            if (request.indexOf("GET /APAGAR") != -1) { 
              modoManual = 2; 
            }
            if (request.indexOf("GET /AUTO") != -1) { 
              modoManual = 0; 
              bloqueoSeguridad = false; 
            }

            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();
            
            client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
            client.println("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
            client.println("<title>Monitor de Agua Pro</title>");
            client.println("<style>body{font-family:Arial,sans-serif;text-align:center;background:#f4f4f9;padding:10px;} .card{background:white;padding:15px;border-radius:15px;margin:12px auto;max-width:400px;box-shadow:0 4px 8px rgba(0,0,0,0.1);} h1{color:#0288d1;} .status{font-weight:bold;padding:10px;border-radius:5px;margin-top:10px;} .on{background:#c8e6c9;color:#256029;} .off{background:#ffcdd2;color:#c62828;} .alert{background:#ffe0b2;color:#e65100;} .val{font-size:24px;color:#0288d1;font-weight:bold;} .btn{display:inline-block;width:125px;box-sizing:border-box;text-align:center;padding:10px 5px;margin:5px;font-size:14px;font-weight:bold;color:white;text-decoration:none;border-radius:5px;box-shadow:0 2px 4px rgba(0,0,0,0.2);} .btn-on{background:#4caf50;} .btn-off{background:#f44336;} .btn-auto{background:#2196f3;}</style>");
            
            client.println("<script>setInterval(function(){ if(!window.location.search) { location.reload(); } }, 3000);</script>");
            client.println("</head><body>");
            client.println("<h1>💧 Control de Agua Inteligente 💧</h1>");
            
            client.println("<div class='card'><h2>Tanque Superior (1000L)</h2><p class='val'>" + String(pctTanque) + " %</p><p>Aprox: <strong>" + String(litrosTanque) + " Litros</strong></p></div>");
            client.println("<div class='card'><h2>Cisterna (500L)</h2><p class='val'>" + String(pctCisterna) + " %</p><p>Aprox: <strong>" + String(litrosCisterna) + " Litros</strong></p></div>");
            
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
            
            String modoTexto = (modoManual == 0) ? "AUTOMÁTICO" : ((modoManual == 1) ? "MANUAL (ENCENDIDO)" : "MANUAL (APAGADO)");
            client.println("<p style='font-size:12px; color:#666;'>Modo actual: " + modoTexto + "</p>");
            client.println("</div>");

            client.println("<div class='card'><h2>Controles Manuales</h2>");
            client.println("<a href='/ENCENDER' class='btn btn-on'>ARRANQUE</a>");
            client.println("<a href='/APAGAR' class='btn btn-off'>PARADA</a>");
            client.println("<a href='/AUTO' class='btn btn-auto'>AUTO / RESET</a>");
            client.println("</div></body></html>");
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
  delay(10);
}