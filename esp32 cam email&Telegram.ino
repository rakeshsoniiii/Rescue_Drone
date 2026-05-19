#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ESP_Mail_Client.h>

// ================= WIFI =================

const char* ssid = "Rak";
const char* password = "@1212@#1";

// ================= TELEGRAM =================

#define BOT_TOKEN "8853836062:AAE28xC3qrGy2ymCiLXY8vf4Dh54HqDspeI"
#define CHAT_ID "7999773900"

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// ================= EMAIL =================

#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 465

#define AUTHOR_EMAIL "Sumedhamukherjee2004@gmail.com"
#define AUTHOR_PASSWORD "zgrqyyyhcxkdnbce"

#define RECIPIENT_EMAIL "rakeshsoni48128@gmail.com"

SMTPSession smtp;

// ==================================================

void sendTelegram() {

  Serial.println("Sending Telegram Message...");

  bool sent = bot.sendMessage(
    CHAT_ID,
    "🚁 Rescue Drone Online!\n📡 ESP32 Connected Successfully.",
    ""
  );

  if (sent) {
    Serial.println("✅ Telegram Message Sent");
  } else {
    Serial.println("❌ Telegram Failed");
  }
}

// ==================================================

void sendEmail() {

  Serial.println("Sending Email...");

  ESP_Mail_Session session;

  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;

  session.login.email = AUTHOR_EMAIL;
  session.login.password = AUTHOR_PASSWORD;

  session.login.user_domain = "";

  SMTP_Message message;

  message.sender.name = "ESP32 Rescue Drone";
  message.sender.email = AUTHOR_EMAIL;

  message.subject = "🚁 Rescue Drone Alert";

  message.addRecipient("Rakesh", RECIPIENT_EMAIL);

  String textMsg =
    "Hello Rakesh!\n\n"
    "ESP32 Rescue Drone is now ONLINE.\n\n"
    "WiFi Connected Successfully.\n"
    "Telegram + Email systems operational.";

  message.text.content = textMsg.c_str();

  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  if (!smtp.connect(&session)) {

    Serial.println("❌ SMTP Connection Failed");
    return;
  }

  if (!MailClient.sendMail(&smtp, &message)) {

    Serial.println("❌ Email Send Failed");
    Serial.println(smtp.errorReason());

  } else {

    Serial.println("✅ Email Sent Successfully!");
  }

  smtp.closeSession();
}

// ==================================================

void setup() {

  Serial.begin(115200);

  Serial.println("\n🚁 RESCUE DRONE STARTING...");

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {

    Serial.print(".");
    delay(500);
  }

  Serial.println("\n✅ WiFi Connected");

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setInsecure();

  // SEND TELEGRAM
  sendTelegram();

  delay(3000);

  // SEND EMAIL
  sendEmail();

  Serial.println("\n✅ SYSTEM READY");
}

// ==================================================

void loop() {

  delay(1000);
}