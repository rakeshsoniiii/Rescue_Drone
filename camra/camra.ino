/*
  ╔══════════════════════════════════════════════════════════╗
  ║  Warehouse Bot Camera — ESP32-CAM (AI Thinker)           ║
  ║  WiFi Station  |  MJPEG Stream :81  |  Controls :80      ║
  ║  GPS NEO-6M    |  Email alert with JPEG                   ║
  ╚══════════════════════════════════════════════════════════╝

  Libraries required (install via Arduino Library Manager):
    - ESP32 board package (espressif/arduino-esp32)
    - TinyGPSPlus  by Mikal Hart
    - ESP Mail Client  by Mobizt  (search "ESP Mail Client")

  GPS Wiring (NEO-6M / NEO-8M):
    GPS VCC  → 3.3 V  on ESP32-CAM
    GPS GND  → GND
    GPS TX   → GPIO 14  (UART2 RX on ESP32-CAM)
    GPS RX   → GPIO 15  (UART2 TX — optional, only needed for commands)
*/

// ── Core ──────────────────────────────────────────────────────────────────────
#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp32-hal-ledc.h"
#include "esp_heap_caps.h"

// ── GPS ───────────────────────────────────────────────────────────────────────
#include <HardwareSerial.h>
#include <TinyGPSPlus.h>

// ── Email ─────────────────────────────────────────────────────────────────────
#include <ESP_Mail_Client.h>

// ── Telegram ──────────────────────────────────────────────────────────────────
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ════════════════════════════════════════════════════════════════════════════
//  USER CONFIG
// ════════════════════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "Rak";
const char* WIFI_PASS     = "@1212@#1";

const char* FROM_EMAIL    = "Sumedhamukherjee2004@gmail.com";
const char* FROM_NAME     = "Warehouse Bot";
const char* TO_EMAIL      = "rakeshsoni48128@gmail.com";
const char* APP_PASSWORD  = "zgrqyyyhcxkdnbce";  // no spaces

// ── Telegram Config ──────────────────────────────────────────────────────────
// Get bot token from @BotFather, chat ID from @userinfobot on Telegram
const char* TG_BOT_TOKEN = "8853836062:AAE28xC3qrGy2ymCiLXY8vf4Dh54HqDspeI";   // e.g. 7312345678:AAFxxx
const char* TG_CHAT_ID   = "7999773900";     // e.g. 123456789

// ════════════════════════════════════════════════════════════════════════════
//  CAMERA PINS — AI Thinker
// ════════════════════════════════════════════════════════════════════════════
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22
#define LED_GPIO_NUM     4

// ════════════════════════════════════════════════════════════════════════════
//  GPS PINS — UART2
// ════════════════════════════════════════════════════════════════════════════
#define GPS_RX_PIN  14   // ← connect GPS module TX here
#define GPS_TX_PIN  15   // → connect GPS module RX here (optional)
#define GPS_BAUD    9600

// ════════════════════════════════════════════════════════════════════════════
//  LED LEDC
// ════════════════════════════════════════════════════════════════════════════
#define LED_LEDC_CH  LEDC_CHANNEL_2
int ledDuty = 0;

void setLED(int duty) {
  ledDuty = constrain(duty, 0, 255);
  ledcWrite(LED_LEDC_CH, ledDuty);
}

// ════════════════════════════════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════════════════════════════════
httpd_handle_t camHttpd    = NULL;
httpd_handle_t streamHttpd = NULL;

HardwareSerial gpsSerial(2);
TinyGPSPlus   gps;
SMTPSession    smtp;

double gpsLat = 0, gpsLng = 0, gpsAlt = 0;
bool   gpsValid = false;
int    gpsSats  = 0;
bool   flashOn  = false;

// ── Email/Telegram task buffer (PSRAM, not internal DRAM) ─────────────────────
// Static 80 KB in .bss overflowed dram0 by ~11 KB on ESP32-CAM.
#define ALERT_BUF_SIZE (48 * 1024)  // VGA JPEG is usually 15–35 KB
static uint8_t* alertBuf     = nullptr;
static size_t   alertBufCap  = 0;
static size_t   alertBufLen  = 0;
static bool     alertBusy    = false;

bool initAlertBuf() {
  if (alertBuf) return true;
  size_t want = ALERT_BUF_SIZE;
  alertBuf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
  if (!alertBuf) {
    want = 24 * 1024;
    alertBuf = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!alertBuf) {
    Serial.println("[alert] JPEG buffer alloc FAILED");
    return false;
  }
  alertBufCap = want;
  Serial.printf("[alert] Buffer %u KB in %s\n",
                (unsigned)(alertBufCap / 1024),
                esp_ptr_external_ram(alertBuf) ? "PSRAM" : "DRAM");
  return true;
}

size_t alertBufCapacity() {
  return alertBufCap;
}

// Stream
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CT   = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BND  = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ════════════════════════════════════════════════════════════════════════════
//  EMAIL
// ════════════════════════════════════════════════════════════════════════════
void sendPhotoEmail(camera_fb_t* fb) {
  Serial.println("[email] Sending...");

  // Use ESP_Mail_Session — matches the working esp32 cam email.ino exactly
  ESP_Mail_Session session;
  session.server.host_name = "smtp.gmail.com";
  session.server.port      = 465;          // port 465 SSL — same as working .ino
  session.login.email      = FROM_EMAIL;
  session.login.password   = APP_PASSWORD; // no spaces: zgrqyyyhcxkdnbce
  session.login.user_domain = "";

  SMTP_Message msg;
  msg.sender.name  = FROM_NAME;
  msg.sender.email = FROM_EMAIL;
  msg.subject      = "Warehouse Bot - Person Detected";
  msg.addRecipient("Rakesh", TO_EMAIL);

  // Body always sends — GPS added only if available
  String body = "Person detected by Warehouse Bot!\n\n";
  if (gpsValid) {
    body += "GPS Location:\n";
    body += "  Latitude : " + String(gpsLat, 6) + "\n";
    body += "  Longitude: " + String(gpsLng, 6) + "\n";
    body += "  Altitude : " + String(gpsAlt, 1) + " m\n";
    body += "  Satellites: " + String(gpsSats) + "\n";
    body += "  Maps: https://maps.google.com/?q=" +
            String(gpsLat, 6) + "," + String(gpsLng, 6) + "\n";
  } else {
    body += "GPS: Not connected — image still attached below.\n";
  }
  msg.text.content = body.c_str();
  msg.text.charSet = "utf-8";
  msg.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  // Attach JPEG image
  SMTP_Attachment att;
  att.descr.filename = "detection.jpg";
  att.descr.mime     = "image/jpeg";
  att.blob.data      = fb->buf;
  att.blob.size      = fb->len;
  att.descr.transfer_encoding = Content_Transfer_Encoding::enc_base64;
  msg.addAttachment(att);

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

// ════════════════════════════════════════════════════════════════════════════
//  TELEGRAM
// ════════════════════════════════════════════════════════════════════════════
void sendTelegramAlert(camera_fb_t* fb) {
  Serial.println("[telegram] Sending message...");

  // Build message text
  String msg = "*Warehouse Bot Alert!*\n";
  msg += "Person detected by camera.\n\n";
  if (gpsValid) {
    msg += "*GPS Location:*\n";
    msg += "Lat: " + String(gpsLat, 6) + "\n";
    msg += "Lng: " + String(gpsLng, 6) + "\n";
    msg += "Alt: " + String(gpsAlt, 1) + " m\n";
    msg += "Sats: " + String(gpsSats) + "\n";
    msg += "[Open in Maps](https://maps.google.com/?q=" +
           String(gpsLat, 6) + "," + String(gpsLng, 6) + ")";
  } else {
    msg += "GPS: Not connected.";
  }

  // ── Step 1: Send text message ──────────────────────────────────────────────
  WiFiClientSecure tgClient;
  tgClient.setInsecure();  // skip SSL cert verify (fine for Telegram)

  HTTPClient https;
  String url = "https://api.telegram.org/bot" + String(TG_BOT_TOKEN) + "/sendMessage";
  https.begin(tgClient, url);
  https.addHeader("Content-Type", "application/json");

  // Escape quotes in msg for JSON
  String body = "{\"chat_id\":\"" + String(TG_CHAT_ID) +
                "\",\"text\":\"" + msg +
                "\",\"parse_mode\":\"Markdown\"}";
  int code = https.POST(body);
  Serial.println("[telegram] Text HTTP " + String(code));
  https.end();

  // ── Step 2: Send photo ─────────────────────────────────────────────────────
  if (fb) {
    url = "https://api.telegram.org/bot" + String(TG_BOT_TOKEN) + "/sendPhoto";

    // Build multipart boundary
    String boundary = "ESP32CAMBoundary";
    String head = "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
    head += String(TG_CHAT_ID) + "\r\n";
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"photo\"; filename=\"capture.jpg\"\r\n";
    head += "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    // Connect manually (HTTPClient multipart is unreliable for binary)
    WiFiClientSecure photoClient;
    photoClient.setInsecure();
    if (photoClient.connect("api.telegram.org", 443)) {
      size_t totalLen = head.length() + fb->len + tail.length();
      photoClient.println("POST /bot" + String(TG_BOT_TOKEN) + "/sendPhoto HTTP/1.1");
      photoClient.println("Host: api.telegram.org");
      photoClient.println("Content-Type: multipart/form-data; boundary=" + boundary);
      photoClient.println("Content-Length: " + String(totalLen));
      photoClient.println("Connection: close");
      photoClient.println();
      photoClient.print(head);
      photoClient.write(fb->buf, fb->len);
      photoClient.print(tail);
      delay(500);
      Serial.println("[telegram] Photo sent");
      photoClient.stop();
    } else {
      Serial.println("[telegram] Photo connect failed");
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  HTTP HANDLERS
// ════════════════════════════════════════════════════════════════════════════

// /stream
static esp_err_t streamHandler(httpd_req_t* req) {
  camera_fb_t* fb  = NULL;
  esp_err_t    res = ESP_OK;
  char         part[64];

  httpd_resp_set_type(req, STREAM_CT);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }
    size_t hlen = snprintf(part, sizeof(part), STREAM_PART, fb->len);
    res  = httpd_resp_send_chunk(req, STREAM_BND, strlen(STREAM_BND));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

// /capture
static esp_err_t captureHandler(httpd_req_t* req) {
  // Flash on for capture — use ledcWrite (NOT digitalWrite)
  bool wasOn = flashOn;
  if (!wasOn) ledcWrite(LED_LEDC_CH, 200);
  delay(150);

  camera_fb_t* fb = esp_camera_fb_get();

  if (!wasOn) ledcWrite(LED_LEDC_CH, 0);

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ── FreeRTOS task: Email (runs with 16KB stack — same as standalone .ino) ────
void emailTaskFn(void* arg) {
  Serial.println("[email-task] Starting on core " + String(xPortGetCoreID()));

  // Build a fake fb pointing at our static buffer
  camera_fb_t fakeFb;
  fakeFb.buf    = alertBuf;
  fakeFb.len    = alertBufLen;
  fakeFb.width  = 640;
  fakeFb.height = 480;
  fakeFb.format = PIXFORMAT_JPEG;

  sendPhotoEmail(&fakeFb);
  alertBusy = false;
  Serial.println("[email-task] Done");
  vTaskDelete(NULL);
}

// /send-email — copies JPEG, returns immediately, email sends in background
static esp_err_t emailHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");

  if (!initAlertBuf()) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"No memory\"}");
  }
  if (alertBusy) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"Already sending\"}");
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }

  size_t cap = alertBufCapacity();
  if (alertBuf && fb->len <= cap) {
    memcpy(alertBuf, fb->buf, fb->len);
    alertBufLen = fb->len;
    alertBusy   = true;
  }
  esp_camera_fb_return(fb);  // release camera for streaming

  if (!alertBusy) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"Image too large or no buffer\"}");
  }

  xTaskCreate(emailTaskFn, "emailTask", 12288, NULL, 1, NULL);

  return httpd_resp_sendstr(req, "{\"sent\":true,\"msg\":\"Sending...\"}\n");
}

// ── FreeRTOS task: Telegram (16KB stack) ─────────────────────────────────────
void telegramTaskFn(void* arg) {
  Serial.println("[telegram-task] Starting");
  camera_fb_t fakeFb;
  fakeFb.buf    = alertBuf;
  fakeFb.len    = alertBufLen;
  fakeFb.width  = 640;
  fakeFb.height = 480;
  fakeFb.format = PIXFORMAT_JPEG;
  sendTelegramAlert(&fakeFb);
  alertBusy = false;
  Serial.println("[telegram-task] Done");
  vTaskDelete(NULL);
}

// /send-telegram
static esp_err_t telegramHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");

  if (!initAlertBuf()) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"No memory\"}");
  }
  if (alertBusy) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"Already sending\"}");
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }

  size_t cap = alertBufCapacity();
  if (alertBuf && fb->len <= cap) {
    memcpy(alertBuf, fb->buf, fb->len);
    alertBufLen = fb->len;
    alertBusy   = true;
  }
  esp_camera_fb_return(fb);

  if (!alertBusy) {
    return httpd_resp_sendstr(req, "{\"sent\":false,\"msg\":\"Image too large or no buffer\"}");
  }

  xTaskCreate(telegramTaskFn, "tgTask", 12288, NULL, 1, NULL);
  return httpd_resp_sendstr(req, "{\"sent\":true,\"msg\":\"Sending...\"}");
}

// /flash
static esp_err_t flashHandler(httpd_req_t* req) {
  flashOn = !flashOn;
  // Use ledcWrite — digitalWrite conflicts with ledcAttachPin
  ledcWrite(LED_LEDC_CH, flashOn ? 255 : 0);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  String j = flashOn ? "{\"flash\":true}" : "{\"flash\":false}";
  return httpd_resp_sendstr(req, j.c_str());
}

// /status
static esp_err_t statusHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  String j = "{\"flash\":" + String(flashOn ? "true" : "false") +
             ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  return httpd_resp_sendstr(req, j.c_str());
}

// /gps
static esp_err_t gpsHandler(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_type(req, "application/json");
  String j = "{\"valid\":" + String(gpsValid ? "true" : "false") +
             ",\"lat\":"  + String(gpsLat, 6) +
             ",\"lng\":"  + String(gpsLng, 6) +
             ",\"alt\":"  + String(gpsAlt, 1) +
             ",\"sats\":" + String(gpsSats) + "}";
  return httpd_resp_sendstr(req, j.c_str());
}

// /control?var=xxx&val=yyy
static esp_err_t controlHandler(httpd_req_t* req) {
  char buf[128];
  int  len = httpd_req_get_url_query_len(req) + 1;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (len > 1 && len <= (int)sizeof(buf) &&
      httpd_req_get_url_query_str(req, buf, len) == ESP_OK) {

    char var[32], val[16];
    if (httpd_query_key_value(buf, "var", var, sizeof(var)) == ESP_OK &&
        httpd_query_key_value(buf, "val", val, sizeof(val)) == ESP_OK) {

      int v = atoi(val);
      sensor_t* s = esp_camera_sensor_get();
      int r = 0;

      if      (!strcmp(var,"framesize"))  r = s->set_framesize(s,(framesize_t)v);
      else if (!strcmp(var,"quality"))    r = s->set_quality(s,v);
      else if (!strcmp(var,"brightness")) r = s->set_brightness(s,v);
      else if (!strcmp(var,"denoise"))    r = s->set_denoise(s,v);
      else if (!strcmp(var,"night_mode")) r = s->set_raw_gma(s,v);
      else if (!strcmp(var,"hmirror"))    r = s->set_hmirror(s,v);
      else if (!strcmp(var,"vflip"))      r = s->set_vflip(s,v);
      else if (!strcmp(var,"led"))        { setLED(v); r = 0; }

      if (r == 0) return httpd_resp_sendstr(req, "OK");
    }
  }
  httpd_resp_send_500(req);
  return ESP_FAIL;
}

// ════════════════════════════════════════════════════════════════════════════
//  SERVER START
// ════════════════════════════════════════════════════════════════════════════
void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;

  httpd_uri_t uris80[] = {
    {"/capture",        HTTP_GET, captureHandler,  NULL},
    {"/flash",          HTTP_GET, flashHandler,    NULL},
    {"/status",         HTTP_GET, statusHandler,   NULL},
    {"/gps",            HTTP_GET, gpsHandler,      NULL},
    {"/control",        HTTP_GET, controlHandler,  NULL},
    {"/send-email",     HTTP_GET, emailHandler,    NULL},
    {"/send-telegram",  HTTP_GET, telegramHandler, NULL},
  };
  if (httpd_start(&camHttpd, &cfg) == ESP_OK)
    for (auto& u : uris80) httpd_register_uri_handler(camHttpd, &u);

  cfg.server_port = 81;
  cfg.ctrl_port   = 32769;
  httpd_uri_t streamUri = {"/stream", HTTP_GET, streamHandler, NULL};
  if (httpd_start(&streamHttpd, &cfg) == ESP_OK)
    httpd_register_uri_handler(streamHttpd, &streamUri);

  Serial.println("[server] :80 control ready");
  Serial.println("[server] :81/stream ready");
}

// ════════════════════════════════════════════════════════════════════════════
//  CAMERA INIT
// ════════════════════════════════════════════════════════════════════════════
void startCamera() {
  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_pclk  = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM; cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM;  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {
    cfg.frame_size   = FRAMESIZE_VGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    // No PSRAM: keep frames small — SVGA in DRAM will not link or will crash
    cfg.frame_size   = FRAMESIZE_CIF;
    cfg.jpeg_quality = 15;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
  }

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("[camera] Init FAILED"); return;
  }
  Serial.println("[camera] Ready");
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  delay(100);

  // LED — ONLY use ledcSetup+ledcAttachPin+ledcWrite
  // Never call pinMode/digitalWrite after ledcAttachPin — it overrides LEDC!
  ledcSetup(LED_LEDC_CH, 5000, 8);
  ledcAttachPin(LED_GPIO_NUM, LED_LEDC_CH);
  ledcWrite(LED_LEDC_CH, 0);

  // GPS UART2
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[gps] UART2 started on GPIO14(RX)/GPIO15(TX)");

  // Camera + alert JPEG buffer (PSRAM)
  startCamera();
  initAlertBuf();

  // WiFi Station
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] Connecting to " + String(WIFI_SSID));
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[wifi] Connected! IP: " + WiFi.localIP().toString());
    Serial.println("[wifi] Stream: http://" + WiFi.localIP().toString() + ":81/stream");
    Serial.println("[wifi] Open:   http://" + WiFi.localIP().toString());
  } else {
    Serial.println("\n[wifi] FAILED — check SSID/password");
  }

  startServers();
}

// ════════════════════════════════════════════════════════════════════════════
//  LOOP — poll GPS
// ════════════════════════════════════════════════════════════════════════════
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
