#include <WiFi.h>

// ⚠️ CAMBIA ESTO CON LOS DATOS DE TU CASA ⚠️
const char* ssid = "EL_NOMBRE_DE_TU_WIFI";
const char* password = "TU_CONTRASEÑA_DEL_WIFI";

// Pines Cisterna (Abajo)
const int PIN_TRIG_CISTERNA = 5;
const int PIN_ECHO_CISTERNA = 18;

// Pines Tanque (Arriba)
const int PIN_TRIG_TANQUE = 23;
const int PIN_ECHO_TANQUE = 22;

// Pin del Motor (El que va al relé o LED)
const int PIN_MOTOR = 2;

// Configuraciones de tus tanques reales (en centímetros)
// Ajusta esto midiendo la altura desde el sensor hasta el fondo de tus tanques
const int CISTERNA_MINIMA_AGUA = 80;  // Si mide más de 80cm, está vacía
const int TANQUE_PIDE_AGUA = 70;      // Si baja de 70cm, pide agua
const int TANQUE_LLENO = 20;          // A los 20cm del sensor, se llena

bool motorEncendido = false;
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
  
  Serial.print("Conectando a: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n¡Wi-Fi Conectado!");
  Serial.print("DIRECCIÓN IP PARA EL CELULAR: http://");
  Serial.println(WiFi.localIP()); // Esta IP es la que vas a poner en el buscador del celu
  
  server.begin();
}

void loop() {
  distCisterna = medirDistancia(PIN_TRIG_CISTERNA, PIN_ECHO_CISTERNA);
  distTanque = medirDistancia(PIN_TRIG_TANQUE, PIN_ECHO_TANQUE);
  
  bool cisternaTieneAgua = (distCisterna < CISTERNA_MINIMA_AGUA);
  if (!cisternaTieneAgua) {
    motorEncendido = false;
  } else if (distTanque > TANQUE_PIDE_AGUA) {
    motorEncendido = true;
  } else if (distTanque <= TANQUE_LLENO) {
    motorEncendido = false;
  }
  digitalWrite(PIN_MOTOR, motorEncendido ? HIGH : LOW);

  WiFiClient client = server.available();
  if (client) {
    String currentLine = "";
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') {
          if (currentLine.length() == 0) {
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();
            
            // Web App
            client.println("<!DOCTYPE html><html><head><meta charset='UTF-8'>");
            client.println("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
            client.println("<title>Monitor Tanques</title>");
            client.println("<style>body{font-family:Arial;text-align:center;background:#f4f4f9;padding:20px;} .card{background:white;padding:20px;border-radius:15px;margin:15px auto;max-width:400px;box-shadow:0 4px 8px rgba(0,0,0,0.1);} h1{color:#0288d1;} .status{font-weight:bold;padding:10px;border-radius:5px;margin-top:10px;} .on{background:#c8e6c9;color:#256029;} .off{background:#ffcdd2;color:#c62828;}</style>");
            client.println("<script>setInterval(function(){location.reload();},3000);</script>");
            client.println("</head><body>");
            client.println("<h1>💧 Monitor de Agua Real 💧</h1>");
            
            client.println("<div class='card'><h2>Tanque Superior</h2><p>Distancia: <strong>" + String(distTanque) + " cm</strong></p></div>");
            client.println("<div class='card'><h2>Cisterna (Abajo)</h2><p>Distancia: <strong>" + String(distCisterna) + " cm</strong></p></div>");
            
            client.println("<div class='card'><h2>Estado de la Bomba</h2>");
            if (motorEncendido) client.println("<div class='status on'>BOMBEANDO AGUA...</div>");
            else client.println("<div class='status off'>MOTOR APAGADO</div>");
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