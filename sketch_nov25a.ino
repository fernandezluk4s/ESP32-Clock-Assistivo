#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// ======================= CONFIGURAÇÕES DO WIFI ========================
const char* ssid = "NOME_WIFI";
const char* password = "SENHA_WIFI";

// ======================= CONFIGURAÇÕES DO TELEGRAM ====================
// Obtenha o token com @BotFather no Telegram
// Obtenha o CHAT_ID com @userinfobot
#define BOT_TOKEN "CODIGO_DO_BOT"
#define CHAT_ID "SEU_ID_PESSOAL"

String telegramUrl = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";

// ======================== CONFIGURAÇÕES DO TFT =========================
#define TFT_CS 5
#define TFT_DC 21
#define TFT_RST 19
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);

// ======================== CONFIGURAÇÕES DO BUZZER ======================
#define BUZZER_PIN 15

// ======================== CONFIGURAÇÕES NTP ============================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -10800, 60000);

// ======================== CONFIGURAÇÕES MQTT ============================
const char* mqtt_server = "broker.hivemq.com";
#define TOPIC_TIME    "esp32/relogio/hora"
#define TOPIC_STATUS  "esp32/relogio/status"
#define TOPIC_EVENT   "esp32/relogio/evento"
#define TOPIC_COMMAND "esp32/relogio/comando"

WiFiClient espClient;
PubSubClient client(espClient);

// ===================== CONTROLE DE ESTADO E EVENTOS ===================
enum EstadoTela { TELA_RELOGIO, TELA_EVENTO };
EstadoTela estadoAtual = TELA_RELOGIO;

bool eventoAtivo = false;
unsigned long eventoInicioMillis = 0;
const unsigned long EVENTO_DURACAO = 5000;
String eventoTexto = "";

bool buzzerTocando = false;
int buzzerContador = 0;
unsigned long buzzerUltimoBeep = 0;
const int BUZZER_TOTAL_BEEPS = 3;
const unsigned long BUZZER_INTERVALO = 250;

long lastMsg = 0;
int ultimoMinutoVerificado = -1;

// ===================== CONTROLE DE TEMPO ===================
// Variável para controlar o atraso do Telegram na inicialização
unsigned long telegramStartupTime = 0; 
const unsigned long TELEGRAM_STARTUP_DELAY = 10000; // 10 segundos

// =============== FILA DE MENSAGENS TELEGRAM (NÃO-BLOQUEANTE) ===============
struct TelegramMessage {
  String texto;
  unsigned long timestamp;
};

#define MAX_TELEGRAM_QUEUE 5
TelegramMessage telegramQueue[MAX_TELEGRAM_QUEUE];
int telegramQueueCount = 0;
unsigned long lastTelegramSent = 0;
const unsigned long TELEGRAM_INTERVAL = 2000; // Envia a cada 2 segundos

// =============== ADICIONAR MENSAGEM À FILA ===============
void adicionarTelegramNaFila(String mensagem) {
  if (telegramQueueCount < MAX_TELEGRAM_QUEUE) {
    telegramQueue[telegramQueueCount].texto = mensagem;
    telegramQueue[telegramQueueCount].timestamp = millis();
    telegramQueueCount++;
    Serial.println("✓ Mensagem adicionada à fila do Telegram");
  } else {
    Serial.println("✗ Fila do Telegram cheia!");
  }
}

// =============== PROCESSAR FILA DO TELEGRAM (NÃO-BLOQUEANTE) ===============
void processarFilaTelegram() {
  if (telegramQueueCount == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  
  unsigned long agora = millis();
  if (agora - lastTelegramSent < TELEGRAM_INTERVAL) return;
  
  // Envia a primeira mensagem da fila
  HTTPClient http;
  http.begin(telegramUrl);
  http.addHeader("Content-Type", "application/json");
  
  // Monta o JSON da mensagem
  String mensagemCompleta = "⚠️ *ALERTA DO RELÓGIO* ⚠️\\n\\n";
  
  // Se for a mensagem de startup, mude o emoji e título
  if (telegramQueue[0].texto.startsWith("Sistema inicializado!")) {
    mensagemCompleta = "✅ *STATUS DO RELÓGIO* ✅\\n\\n";
  }
  
  mensagemCompleta += telegramQueue[0].texto;
  
  // Adiciona timestamp
  time_t timestamp = timeClient.getEpochTime();
  struct tm* t = localtime(&timestamp);
  char timeStr[20];
  snprintf(timeStr, sizeof(timeStr), "\\n\\n🕐 %02d:%02d:%02d", 
           t->tm_hour, t->tm_min, t->tm_sec);
  mensagemCompleta += timeStr;
  
  String payload = "{\"chat_id\":\"" + String(CHAT_ID) + 
                   "\",\"text\":\"" + mensagemCompleta + 
                   "\",\"parse_mode\":\"Markdown\"}";
  
  int httpCode = http.POST(payload);
  
  if (httpCode > 0) {
    if (httpCode == 200) {
      Serial.println("✓ Telegram enviado com sucesso!");
    } else {
      Serial.printf("⚠ Telegram retornou código %d\n", httpCode);
    }
  } else {
    Serial.printf("✗ Erro ao enviar Telegram: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
  lastTelegramSent = agora;
  
  // Remove a mensagem da fila
  for (int i = 0; i < telegramQueueCount - 1; i++) {
    telegramQueue[i] = telegramQueue[i + 1];
  }
  telegramQueueCount--;
}

// =============== FUNÇÕES DE BUZZER NÃO-BLOQUEANTE ===============
void iniciarBuzzer() {
  buzzerTocando = true;
  buzzerContador = 0;
  buzzerUltimoBeep = millis();
  tone(BUZZER_PIN, 2000, 200);
}

void atualizarBuzzer() {
  if (!buzzerTocando) return;
  
  unsigned long agoraMillis = millis();
  if (agoraMillis - buzzerUltimoBeep >= BUZZER_INTERVALO) {
    buzzerContador++;
    if (buzzerContador < BUZZER_TOTAL_BEEPS) {
      tone(BUZZER_PIN, 2000, 200);
      buzzerUltimoBeep = agoraMillis;
    } else {
      buzzerTocando = false;
    }
  }
}

// ========== PUBLICAR EVENTO MQTT ==========
void publicarEventoMQTT(String texto) {
  if (client.connected()) {
    client.publish(TOPIC_EVENT, texto.c_str());
  }
}

// ======================= INICIAR EVENTO (NÃO-BLOQUEANTE) ======================
void iniciarEvento(String texto) {
  eventoTexto = texto;
  estadoAtual = TELA_EVENTO;
  eventoInicioMillis = millis();
  eventoAtivo = true;
  
  iniciarBuzzer();
  publicarEventoMQTT(texto);
  
  // ADICIONA À FILA DO TELEGRAM (não bloqueia!)
  adicionarTelegramNaFila(texto);
  
  // Desenha a tela imediatamente
  desenharTelaEvento();
}

// ======================= DESENHAR TELA DE EVENTO ======================
void desenharTelaEvento() {
  tft.fillScreen(ST7735_YELLOW);
  tft.setTextColor(ST7735_BLACK);

  String titulo = "AVISO!";
  tft.setTextSize(2);

  int16_t x1, y1;
  uint16_t w1, h1;
  tft.getTextBounds(titulo, 0, 0, &x1, &y1, &w1, &h1);

  tft.setCursor((tft.width() - w1) / 2, tft.height() / 2 - h1 - 5);
  tft.println(titulo);

  tft.setTextSize(1);

  int16_t x2, y2;
  uint16_t w2, h2;
  tft.getTextBounds(eventoTexto, 0, 0, &x2, &y2, &w2, &h2);

  tft.setCursor((tft.width() - w2) / 2, 80);
  tft.println(eventoTexto);
}

// ======================= VERIFICAR SE EVENTO TERMINOU ======================
void verificarFimEvento() {
  if (estadoAtual == TELA_EVENTO) {
    unsigned long agoraMillis = millis();
    if (agoraMillis - eventoInicioMillis >= EVENTO_DURACAO) {
      estadoAtual = TELA_RELOGIO;
      eventoAtivo = false;
      // Força a redesenhar o relógio imediatamente ao sair do evento
      // (Isso será feito no loop principal)
    }
  }
}

// ======================= EVENTOS AUTOMÁTICOS ==========================
void verificarEventos(int h, int m) {
  if (ultimoMinutoVerificado == m) return;
  
  ultimoMinutoVerificado = m;

  // ALERTAS
  if (h == 12 && m == 0) { iniciarEvento("Hora do Almoco!"); return; }
  if (h == 14 && m == 0) { iniciarEvento("Hora do Lanche!"); return; }
  if (h == 17 && m == 0) { iniciarEvento("Tomar Remedios"); return; }
  if (h == 19 && m == 30) { iniciarEvento("Hora do Banho"); return; }
  if (h == 21 && m == 0) { iniciarEvento("Hora da Janta!"); return; }
  if (h == 6  && m == 0) { iniciarEvento("Bom dia!"); return; }
  if (h == 22 && m == 0) { iniciarEvento("Boa noite!"); return; }
}

// ======================= CALLBACK MQTT ================================
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.printf("MQTT Command Received [%s]: %s\n", topic, message.c_str());

  if (String(topic) == TOPIC_COMMAND) {
    if (message == "RESTART") {
      iniciarEvento("Reiniciando...");
      delay(2000);
      ESP.restart();
    }
    else if (message.startsWith("EVENTO:")) {
      String textoPersonalizado = message.substring(7);
      iniciarEvento(textoPersonalizado);
    }
  }
}

// ======================= RECONNECT MQTT =================
void reconnect() {
  int tentativas = 0;
  while (!client.connected() && tentativas < 3) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
   
    if (client.connect(clientId.c_str(), TOPIC_STATUS, 1, true, "Offline")) {
      Serial.println("connected");
      client.subscribe(TOPIC_COMMAND);
      client.publish(TOPIC_STATUS, "Online");
      return;
    } else {
      Serial.printf("failed, rc=%d. Retrying...\n", client.state());
      tentativas++;
      delay(1000);
    }
  }
}

// ======================= WIFI ================================
bool connectWifi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  for (int i = 0; i < 20; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("CONNECTED");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println("FAILED");
  return false;
}

// ======================= DESENHAR RELÓGIO ======================
void desenharRelogio(int hours, int minutes, int seconds, String dayOfWeek, int day, int month, int year) {
  tft.fillScreen(ST7735_BLACK);

  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(2);

  char horaStr[9];
  snprintf(horaStr, sizeof(horaStr), "%02d:%02d:%02d", hours, minutes, seconds);

  int16_t x, y; 
  uint16_t w, h;
  tft.getTextBounds(horaStr, 0, 0, &x, &y, &w, &h);
  tft.setCursor((tft.width() - w) / 2, tft.height() / 2 - h - 5);
  tft.print(horaStr);

  tft.setTextSize(1);
  tft.setTextColor(ST7735_YELLOW);

  String dateStr = dayOfWeek + ", " + String(day) + "/" + String(month) + "/" + String(year);

  int16_t x2, y2; 
  uint16_t w2, h2;
  tft.getTextBounds(dateStr, 0, 0, &x2, &y2, &w2, &h2);
  tft.setCursor((tft.width() - w2) / 2, 80);
  tft.print(dateStr);
}

// =============================== SETUP =================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(2);
  tft.setCursor(10, 10);

  if (connectWifi()) {
    tft.println("WiFi OK");
    
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
    timeClient.begin();
    
    // Registra o momento em que a inicialização de rede e MQTT foi concluída
    telegramStartupTime = millis();
    
  } else {
    tft.println("Erro WiFi");
  }
}

// ================================ LOOP ==================================
void loop() {
  // Verifica WiFi
  if (WiFi.status() != WL_CONNECTED) {
    iniciarEvento("WiFi PERDIDO!");
    delay(2000);
    ESP.restart();
  }

  // --- NOVO: Verifica se o atraso de 10 segundos para o Telegram já passou ---
  if (telegramStartupTime != 0 && millis() - telegramStartupTime >= TELEGRAM_STARTUP_DELAY) {
    adicionarTelegramNaFila("Sistema inicializado! ✅");
    // Reseta a variável para não entrar mais neste bloco
    telegramStartupTime = 0; 
  }

  // MQTT
  if (!client.connected()) reconnect();
  client.loop();

  // PROCESSA FILA DO TELEGRAM (não bloqueia!)
  processarFilaTelegram();

  // Atualiza buzzer se estiver tocando
  atualizarBuzzer();

  // Atualiza tempo
  timeClient.update();

  time_t agora = timeClient.getEpochTime();
  struct tm* t = localtime(&agora);

  int hours = t->tm_hour;
  int minutes = t->tm_min;
  int seconds = t->tm_sec;

  int day = t->tm_mday;
  int month = t->tm_mon + 1;
  int year = t->tm_year + 1900;

  String daysOfWeek[] = {"Domingo","Segunda","Terca","Quarta","Quinta","Sexta","Sabado"};
  String dayOfWeek = daysOfWeek[t->tm_wday];

  // Verifica eventos automáticos
  verificarEventos(hours, minutes);

  // Verifica se evento terminou
  verificarFimEvento();

  // MQTT Heartbeat a cada 1 segundo
  if (millis() - lastMsg >= 1000) {
    lastMsg = millis();

    char bufferHora[9];
    snprintf(bufferHora, sizeof(bufferHora), "%02d:%02d:%02d", hours, minutes, seconds);
   
    Serial.printf("Heartbeat: %s\n", bufferHora);
    client.publish(TOPIC_TIME, bufferHora);
    client.publish(TOPIC_STATUS, "Online");
  }

  // Desenha a tela apropriada
  if (estadoAtual == TELA_RELOGIO) {
    desenharRelogio(hours, minutes, seconds, dayOfWeek, day, month, year);
  }

  delay(100);
}
