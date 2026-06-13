#include <WiFi.h>
#include <ESP32Servo.h>
#include <Firebase_ESP_Client.h>
#include <addons/RTDBHelper.h>

// ---------- Credenciais ----------
#define WIFI_SSID       "REDE"
#define WIFI_PASSWORD   "SENHA"
#define DATABASE_URL    "URL FIREBASE"
#define DATABASE_SECRET "SECRET FIREBASE"

// ---------- Pinos ----------
const int PINO_SERVO = 14;
const int PINO_TRIG  = 13;
const int PINO_ECHO  = 12;

// ---------- Parametros mecanicos e de dosagem ----------
const int   ANGULO_ABERTO   = 124;
const int   ANGULO_FECHADO  = 70;
const float DIST_POTE_VAZIO = 16.0;
const float DIST_POTE_CHEIO = 7.0;
const float FATOR_S_POR_G   = 0.1;   // tempo de abertura por grama
const int   TOLERANCIA_NIVEL = 2;    // % minimo de queda de racao
const int   NIVEL_MINIMO     = 15;   // alarme de reposicao de racao

// ---------- NTP (fuso de Brasilia, GMT-3) ----------
const long GMT_OFFSET_SEC  = -3 * 3600;
const int  DAYLIGHT_OFFSET = 0;

// ---------- Estados da maquina de dosagem ----------
enum EstadoDosagem {
  SERVO_IDLE,
  SERVO_ABRINDO,
  SERVO_ABERTO,
  SERVO_FECHANDO,
  SERVO_FECHADO
};
volatile EstadoDosagem estado = SERVO_IDLE;

// ---------- Parametros vindos do Firebase ----------
volatile int gramasManual = 20;
volatile int gramasDaVez  = 20;
String horariosAgendados  = "08:00:20,18:00:20";
int ultimoMinutoDisparo   = -1;

// ---------- Balanca simulada ----------
volatile float pesoAtual      = 0.0;
volatile float pesoAlvo       = 0.0;
volatile bool  comportaAberta = false;

// ---------- Objetos e primitivas FreeRTOS ----------
Servo meuServo;
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
QueueHandle_t filaComandos;
SemaphoreHandle_t mutexPeso;
SemaphoreHandle_t mutexFirebase;

// ============================================================
//  Funcoes auxiliares
// ============================================================
float lerDistanciaCm() {
  digitalWrite(PINO_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PINO_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PINO_TRIG, LOW);
  long d = pulseIn(PINO_ECHO, HIGH, 30000);
  if (d == 0) return -1;
  return d * 0.0343 / 2.0;
}

int calcularNivelPercentual(float dist) {
  if (dist < 0) return -1;
  float n = (DIST_POTE_VAZIO - dist) / (DIST_POTE_VAZIO - DIST_POTE_CHEIO) * 100.0;
  if (n < 0)   n = 0;
  if (n > 100) n = 100;
  return (int)n;
}

void fecharServoLento() {
  meuServo.write(90);
  for (int a = 90; a >= ANGULO_FECHADO; a--) {
    meuServo.write(a);
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// Verifica se a hora atual bate com um horario agendado ("HH:MM:gramas").
// Se bater, guarda a gramagem daquele horario em gramasDaVez.
bool horarioDeAlimentar() {
  struct tm t;
  if (!getLocalTime(&t)) return false;
  int minutoAtual = t.tm_hour * 60 + t.tm_min;
  if (minutoAtual == ultimoMinutoDisparo) return false;

  String lista = horariosAgendados;
  lista.replace(";", ",");
  lista.replace(" ", "");

  while (lista.length() > 0) {
    int virg = lista.indexOf(',');
    String item = (virg == -1) ? lista : lista.substring(0, virg);

    int dp1 = item.indexOf(':');
    int dp2 = item.indexOf(':', dp1 + 1);
    if (dp1 > 0 && dp2 > dp1) {
      int h = item.substring(0, dp1).toInt();
      int m = item.substring(dp1 + 1, dp2).toInt();
      int g = item.substring(dp2 + 1).toInt();
      if (h == t.tm_hour && m == t.tm_min) {
        ultimoMinutoDisparo = minutoAtual;
        gramasDaVez = g;
        return true;
      }
    }
    if (virg == -1) break;
    lista = lista.substring(virg + 1);
  }
  return false;
}

// ============================================================
//  TASK 1 - Comunicacao (WiFi + NTP + Firebase)
// ============================================================
void Task_Comunicacao(void *pv) {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(300));
  Serial.print("WiFi OK! IP: "); Serial.println(WiFi.localIP());

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, "pool.ntp.org", "time.nist.gov");
  struct tm t;
  bool ntpOk = false;
  for (int i = 0; i < 5 && !ntpOk; i++) {
    if (getLocalTime(&t, 5000)) {
      ntpOk = true;
      Serial.println("NTP sincronizado.");
    } else {
      Serial.printf("NTP tentativa %d falhou, tentando de novo...\n", i + 1);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
  if (!ntpOk) Serial.println("NTP nao sincronizou (so manual vai funcionar).");

  config.database_url = DATABASE_URL;
  config.signer.tokens.legacy_token = DATABASE_SECRET;
  Firebase.reconnectNetwork(true);
  fbdo.setBSSLBufferSize(4096, 1024);
  Firebase.begin(&config, &auth);
  Serial.println("Firebase iniciado.");

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ============================================================
//  TASK 2 - Gestor de Eventos
// ============================================================
void Task_GestorEventos(void *pv) {
  for (;;) {
    if (xSemaphoreTake(mutexFirebase, portMAX_DELAY)) {
      if (Firebase.ready()) {
        // UC2 - configuracao remota
        if (Firebase.RTDB.getInt(&fbdo, "/config/gramasManual"))
          gramasManual = fbdo.intData();
        if (Firebase.RTDB.getString(&fbdo, "/config/horarios"))
          horariosAgendados = fbdo.stringData();

        // UC3 - gatilho manual (reseta a flag na hora, evita disparo duplo)
        if (Firebase.RTDB.getBool(&fbdo, "/comando/abrirPorta")) {
          if (fbdo.boolData() == true && estado == SERVO_IDLE) {
            Firebase.RTDB.setBool(&fbdo, "/comando/abrirPorta", false);
            gramasDaVez = gramasManual;
            int cmd = 1; xQueueSend(filaComandos, &cmd, 0);
            Serial.println(">> Gatilho MANUAL");
          }
        }

        // UC1 - gatilho programado por horario (NTP)
        if (horarioDeAlimentar() && estado == SERVO_IDLE) {
          int cmd = 1; xQueueSend(filaComandos, &cmd, 0);
          Serial.println(">> Gatilho PROGRAMADO (horario)");
        }
      }
      xSemaphoreGive(mutexFirebase);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ============================================================
//  TASK 3 - Balanca
// ============================================================
void Task_Balanca(void *pv) {
  for (;;) {
    if (xSemaphoreTake(mutexPeso, portMAX_DELAY)) {
      if (comportaAberta) pesoAtual += 5.0;
      xSemaphoreGive(mutexPeso);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  TASK 4 - Controle de Dosagem
// ============================================================
void Task_ControleDosagem(void *pv) {
  int cmd;
  int nivelAntes = 0;
  unsigned long inicioDosagem = 0, inicioFechamento = 0, inicioEstab = 0;

  for (;;) {
    switch (estado) {

      case SERVO_IDLE:
        if (xQueueReceive(filaComandos, &cmd, pdMS_TO_TICKS(100))) {
          pesoAlvo = gramasDaVez;
          estado = SERVO_ABRINDO;
        }
        break;

      case SERVO_ABRINDO:
        nivelAntes = calcularNivelPercentual(lerDistanciaCm());
        Serial.printf(">> Nivel antes: %d%%\n", nivelAntes);
        if (xSemaphoreTake(mutexPeso, portMAX_DELAY)) {
          pesoAtual = 0.0; comportaAberta = true;
          xSemaphoreGive(mutexPeso);
        }
        meuServo.write(ANGULO_ABERTO);
        if (xSemaphoreTake(mutexFirebase, portMAX_DELAY)) {
          if (Firebase.ready()) Firebase.RTDB.setString(&fbdo, "/status", "ABERTO");
          xSemaphoreGive(mutexFirebase);
        }
        inicioDosagem = millis();
        Serial.printf(">> [ABRINDO] dosando %dg por %.1fs\n", gramasDaVez, gramasDaVez * FATOR_S_POR_G);
        estado = SERVO_ABERTO;
        break;

      case SERVO_ABERTO:
        if (millis() - inicioDosagem >= (unsigned long)(gramasDaVez * FATOR_S_POR_G * 1000)
            || millis() - inicioDosagem >= 15000) {
          if (xSemaphoreTake(mutexPeso, portMAX_DELAY)) {
            comportaAberta = false;
            xSemaphoreGive(mutexPeso);
          }
          fecharServoLento();
          inicioFechamento = millis();
          Serial.println(">> [ABERTO] tempo de dosagem concluido");
          estado = SERVO_FECHANDO;
        }
        break;

      case SERVO_FECHANDO:
        if (millis() - inicioFechamento >= 1000) {
          inicioEstab = millis();
          estado = SERVO_FECHADO;
        }
        break;

      case SERVO_FECHADO:
        if (millis() - inicioEstab >= 500) {
          int nivelDepois = calcularNivelPercentual(lerDistanciaCm());
          int queda = nivelAntes - nivelDepois;
          bool caiuArroz = (queda >= TOLERANCIA_NIVEL);
          Serial.printf(">> Nivel depois: %d%% | queda: %d%%\n", nivelDepois, queda);

          if (xSemaphoreTake(mutexFirebase, portMAX_DELAY)) {
            if (Firebase.ready()) {
              Firebase.RTDB.setInt(&fbdo, "/ultimaPorcaoGramas", gramasDaVez);
              Firebase.RTDB.setInt(&fbdo, "/nivelPercentual", nivelDepois);
              Firebase.RTDB.setString(&fbdo, "/status", "IDLE");
              struct tm t;
              char horaStr[6] = "--:--";
              if (getLocalTime(&t)) sprintf(horaStr, "%02d:%02d", t.tm_hour, t.tm_min);
              Firebase.RTDB.setString(&fbdo, "/ultimaPorcaoHora", horaStr);
              Firebase.RTDB.setString(&fbdo, "/alerta", caiuArroz ? "ok" : "sem racao");
            }
            xSemaphoreGive(mutexFirebase);
          }
          Serial.println(caiuArroz ? ">> [FECHADO] racao caiu OK"
                                   : ">> [FECHADO] ALERTA: nao detectou queda de nivel!");
          estado = SERVO_IDLE;
        }
        break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================
//  TASK 5 - Telemetria e Nivel (UC4)
// ============================================================
void Task_Telemetria(void *pv) {
  for (;;) {
    float dist = lerDistanciaCm();
    int nivel = calcularNivelPercentual(dist);

    if (xSemaphoreTake(mutexFirebase, portMAX_DELAY)) {
      if (Firebase.ready() && nivel >= 0) {
        Firebase.RTDB.setInt(&fbdo, "/nivelPercentual", nivel);
        Firebase.RTDB.setString(&fbdo, "/alertaNivel", nivel < NIVEL_MINIMO ? "baixo" : "ok");
      }
      xSemaphoreGive(mutexFirebase);
    }

    if (nivel < 0) Serial.println("Nivel reservatorio: leitura invalida");
    else           Serial.printf("Nivel reservatorio: %d%% (dist %.1f cm)\n", nivel, dist);

    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  WiFi.setSleep(false);
  Serial.begin(115200);

  meuServo.attach(PINO_SERVO);
  meuServo.write(ANGULO_FECHADO);
  pinMode(PINO_TRIG, OUTPUT);
  pinMode(PINO_ECHO, INPUT);

  filaComandos  = xQueueCreate(5, sizeof(int));
  mutexPeso     = xSemaphoreCreateMutex();
  mutexFirebase = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(Task_Comunicacao,     "Comunicacao", 8192,  NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_GestorEventos,   "Gestor",      8192,  NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_Balanca,         "Balanca",     2048,  NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_ControleDosagem, "Dosagem",     12288, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_Telemetria,      "Telemetria",  8192,  NULL, 1, NULL, 1);

  Serial.println("PetFeeder FreeRTOS - 5 tasks criadas.");
}

void loop() {
  vTaskDelete(NULL);
}