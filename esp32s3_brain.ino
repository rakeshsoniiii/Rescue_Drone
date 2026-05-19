/*
  ╔══════════════════════════════════════════════════════════╗
  ║  Warehouse Bot — ESP32-S3 N16R8  "BRAIN"                 ║
  ║  Email + Telegram + GPS                                   ║
  ║  Fetches JPEG from ESP32-CAM then sends alerts           ║
  ╚══════════════════════════════════════════════════════════╝

  Board: ESP32-S3 Dev Module  (select in Arduino IDE)
  Flash: 16MB  PSRAM: 8MB OPI

  Libraries (Arduino Library Manager):
    - ESP Mail Client  by Mobizt
    - TinyGPSPlus      by Mikal Hart

  GPS Wiring (NEO-6M):
    GPS TX  → GPIO 17  (UART1 RX on S3)
    GPS RX  → GPIO 18  (UART1 TX — optional)
    GPS VCC → 3.3V
    GPS GND → GND

  Setup:
    1. Flash ESP32-CAM with camra/camra.ino (eye firmware)
    2. Boot ESP32-CAM, note its IP from Serial Monitor
    3. Paste that IP into ESP32_CAM_IP below
    4. Flash this file onto ESP32-S3
    5. Boot S3, note its IP from Serial Monitor
    6. Paste S3 IP into yolo_detection.py
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP_Mail_Client.h>
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>
#include "esp_http_server.h"

// ════════════════════════════════════════════════════════════════════
//  USER CONFIG
// ════════════════════════════════════════════════════════════════════
const char* WIFI_SSID    = "Rak";
const char* WIFI_PASS    = "@1212@#1";

// ── ESP32-CAM IP (get from camra.ino Serial Monitor after boot) ───
const char* ESP32_CAM_IP = "192.168.1.100";  // ← change this!

// ── Email ─────────────────────────────────────────────────────────
const char* FROM_EMAIL   = "Sumedhamukherjee2004@gmail.com";
const char* FROM_NAME    = "Warehouse Bot";
const char* TO_EMAIL     = "rakeshsoni48128@gmail.com";
const char* APP_PASSWORD = "zgrqyyyhcxkdnbce";

// ── Telegram ─────────────────────────────────────────────────────
const char* TG_BOT_TOKEN = "8853836062:AAE28xC3qrGy2ymCiLXY8vf4Dh54HqDspeI";   // from @BotFather
const char* TG_CHAT_ID   = "7999773900";     // from @userinfobot

// ── GPS ───────────────────────────────────────────────────────────
#define GPS_RX_PIN  17
#define GPS_TX_PIN  18
#define GPS_BAUD    9600

// ════════════════════════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════════════════════════
HardwareSerial  gpsSerial(1);   // UART1 on S3
TinyGPSPlus     gps;
SMTPSession     smtp;
httpd_handle_t  brainHttpd = NULL;

double gpsLat = 0, gpsLng = 0, gpsAlt = 0;
bool   gpsValid = false;
int    gpsSats  = 0;

// Image buffer — S3 has 8MB PSRAM, so 200KB is fine
#define IMG_BUF_SIZE (200 * 1024)
static uint8_t imgBuf[IMG_BUF_SIZE];
static size_t  imgLen = 0;
static bool    alertBusy = false;

// ════════════════════════════════════════════════════════════════════
//  FETCH JPEG FROM ESP32-CAM
// ════════════════════════════════════════════════════════════════════
bool fetchImageFromCam() {
  String url = "http://" + String(ESP32_CAM_IP) + "/capture";
  Serial.println("[fetch] GET " + url);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();

  if (code != 200) {
    Serial.println("[fetch] Failed HTTP " + String(code));
    http.end();
    return false;
  }

  imgLen = http.getSize();
  if (imgLen == 0 || imgLen > IMG_BUF_SIZE) {
    Serial.println("[fetch] Bad size: " + String(imgLen));
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t read = 0;
  while (http.connected() && read < imgLen) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t chunk = min(avail, IMG_BUF_SIZE - read);
      stream->readBytes(imgBuf + read, chunk);
      read += chunk;
    } else {
      delay(1);
    }
  }
  http.end();

  imgLen = read;
  Serial.printf("[fetch] Got %u bytes\n", imgLen);
  return imgLen > 0;
}

// ════════════════════════════════════════════════════════════════════
//  EMAIL
// ════════════════════════════════════════════════════════════════════
void sendEmail() {
  Serial.println("[email] Sending...");

  ESP_Mail_Session session;
  session.server.host_name  = "smtp.gmail.com";
  session.server.port       = 465;
  session.login.email       = FROM_EMAIL;
  session.login.password    = APP_PASSWORD;
  session.login.user_domain = "";

  SMTP_Message msg;
  msg.sender.name  = FROM_NAME;
  msg.sender.email = FROM_EMAIL;
  msg.subject      = "Warehouse Bot - Person Detected";
  msg.addRecipient("Rakesh", TO_EMAIL);

  String body = "Person detected by Warehouse Bot!\n\n";
  if (gpsValid) {
    body += "GPS Location:\n";
    body += "  Latitude : " + String(gpsLat, 6) + "\n";
    body += "  Longitude: " + String(gpsLng, 6) + "\n";
    body += "  Altitude : " + String(gpsAlt, 1) + " m\n";
    body += "  Maps: https://maps.google.com/?q=" +
            String(gpsLat, 6) + "," + String(gpsLng, 6) + "\n";
  } else {
    body += "GPS: Not connected.\n";
  }
  msg.text.content  = body.c_str();
  msg.text.charSet  = "utf-8";
  msg.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  // Attach JPEG from buffer
  if (imgLen > 0) {
    SMTP_Attachment att;
    att.descr.filename = "detection.jpg";
    att.descr.mime     = "image/jpeg";
    att.blob.data      = imgBuf;
    att.blob.size      = imgLen;
    att.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
    msg.addAttachment(att);
  }

  if (!smtp.connect(&session)) {
    Serial.println("[email] Connect FAILED: " + smtp.errorReason());
    return;
  }
  if (!MailClient.sendMail(&smtp, &msg)) {
    Serial.println("[email] Send FAILED: " + smtp.errorReason());
  } else {
    Serial.println("[email] Sent OK!");
  }
  smtp.closeSession();
}

// ════════════════════════════════════════════════════════════════════
//  TELEGRAM
// ════════════════════════════════════════════════════════════════════
void sendTelegram() {
  Serial.println("[telegram] Sending...");

  String text = "*Warehouse Bot Alert!*\nPerson detected.\n\n";
  if (gpsValid) {
    text += "Lat: " + String(gpsLat, 6) + "\n";
    text += "Lng: " + String(gpsLng, 6) + "\n";
    text += "[Open Map](https://maps.google.com/?q=" +
            String(gpsLat, 6) + "," + String(gpsLng, 6) + ")";
  } else {
    text += "GPS: Not connected.";
  }

  WiFiClientSecure tgClient;
  tgClient.setInsecure();
  HTTPClient https;

  // Text message
  String url = "https://api.telegram.org/bot" + String(TG_BOT_TOKEN) + "/sendMessage";
  https.begin(tgClient, url);
  https.addHeader("Content-Type", "application/json");
  String body = "{\"chat_id\":\"" + String(TG_CHAT_ID) +
                "\",\"text\":\"" + text +
                "\",\"parse_mode\":\"Markdown\"}";
  int code = https.POST(body);
  Serial.println("[telegram] Text HTTP " + String(code));
  https.end();

  // Photo
  if (imgLen > 0) {
    url = "https://api.telegram.org/bot" + String(TG_BOT_TOKEN) + "/sendPhoto";
    String boundary = "S3Boundary";
    String head  = "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    head += String(TG_CHAT_ID) + "\r\n";
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"photo\"; filename=\"detect.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    WiFiClientSecure photoClient;
    photoClient.setInsecure();
    if (photoClient.connect("api.telegram.org", 443)) {
      size_t total = head.length() + imgLen + tail.length();
      photoClient.println("POST /bot" + String(TG_BOT_TOKEN) + "/sendPhoto HTTP/1.1");
      photoClient.println("Host: api.telegram.org");
      photoClient.println("Content-Type: multipart/form-data; boundary=" + boundary);
      photoClient.println("Content-Length: " + String(total));
      photoClient.println("Connection: close");
      photoClient.println();
      photoClient.print(head);
      photoClient.write(imgBuf, imgLen);
      photoClient.print(tail);
      delay(1000);
      Serial.println("[telegram] Photo sent");
      photoClient.stop();
    }
  }
}

// ════════════════════════════════════════════════════════════════════
//  ALERT TASK (runs with large stack)
// ════════════════════════════════════════════════════════════════════
void alertTaskFn(void* arg) {
  Serial.println("[alert] Task started on core " + String(xPortGetCoreID()));

  bool ok = fetchImageFromCam();
  if (!ok) {
    Serial.println("[alert] Could not fetch image — sending text-only alert");
    imgLen = 0;
  }

  sendEmail();
  sendTelegram();

  alertBusy = false;
  Serial.println("[alert] Task done");
  vTaskDelete(NULL);
}

// ════════════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════

// /send-alert  (called by Python YOLO or HTML button)
static esp_err_t alertHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");

  if (alertBusy) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"Already sending\"}");
  }

  alertBusy = true;
  // 20KB stack — plenty for SMTP + Telegram SSL
  xTaskCreate(alertTaskFn, "alertTask", 20480, NULL, 1, NULL);

  return httpd_resp_sendstr(req, "{\"sent\":true,\"msg\":\"Alert triggered\"}");
}

// /status
static esp_err_t statusHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  String j = "{\"cam\":\"" + String(ESP32_CAM_IP) + "\""
           + ",\"gps_valid\":" + (gpsValid ? "true" : "false")
           + ",\"lat\":" + String(gpsLat, 6)
           + ",\"lng\":" + String(gpsLng, 6)
           + ",\"sats\":" + String(gpsSats)
           + ",\"busy\":" + (alertBusy ? "true" : "false") + "}";
  return httpd_resp_sendstr(req, j.c_str());
}

// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] ESP32-S3 Brain starting...");

  // GPS on UART1
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[gps] UART1 on GPIO17(RX)/GPIO18(TX)");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\n[wifi] Connected! IP: " + WiFi.localIP().toString());
  Serial.println("[wifi] Paste this IP into yolo_detection.py as ESP32_S3_IP");
  Serial.println("[info] ESP32-CAM IP set to: " + String(ESP32_CAM_IP));

  // Start HTTP server
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port    = 80;
  cfg.stack_size     = 8192;

  httpd_uri_t uris[] = {
    {"/send-alert", HTTP_GET, alertHandler,  NULL},
    {"/status",     HTTP_GET, statusHandler, NULL},
  };

  if (httpd_start(&brainHttpd, &cfg) == ESP_OK) {
    for (auto& u : uris) httpd_register_uri_handler(brainHttpd, &u);
    Serial.println("[server] Brain HTTP server on :80");
    Serial.println("[server] /send-alert  — triggers email + telegram");
    Serial.println("[server] /status      — GPS + system status");
  }
}

// ════════════════════════════════════════════════════════════════════
//  LOOP — poll GPS
// ════════════════════════════════════════════════════════════════════
void loop() {
  while (gpsSerial.available() > 0) {
    if (gps.encode(gpsSerial.read())) {
      if (gps.location.isValid()) {
        gpsValid = true;
        gpsLat   = gps.location.lat();
        gpsLng   = gps.location.lng();
      }
      if (gps.altitude.isValid()) gpsAlt = gps.altitude.meters();
      gpsSats = gps.satellites.isValid() ? gps.satellites.value() : 0;
    }
  }
  delay(10);
}
