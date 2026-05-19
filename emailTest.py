import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart

# EMAIL CONFIG
FROM_EMAIL = "Sumedhamukherjee2004@gmail.com"
TO_EMAIL = "rakeshsoni48128@gmail.com"

# REMOVE SPACES FROM APP PASSWORD
APP_PASSWORD = "zgrqyyyhcxkdnbce"

# CREATE EMAIL
msg = MIMEMultipart()

msg["From"] = FROM_EMAIL
msg["To"] = TO_EMAIL
msg["Subject"] = "ESP32 Drone Test Email"

body = """
Hello Rakesh, I'm your Pengu,

This is a test email from your ESP32 Rescue Drone project 🚁

If you received this email,
your SMTP setup is working correctly.

- AI Rescue Drone System
"""

msg.attach(MIMEText(body, "plain"))

try:
    # CONNECT TO GMAIL SMTP SERVER
    server = smtplib.SMTP("smtp.gmail.com", 587)

    server.starttls()

    # LOGIN
    server.login(FROM_EMAIL, APP_PASSWORD)

    # SEND EMAIL
    server.sendmail(FROM_EMAIL, TO_EMAIL, msg.as_string())

    print("✅ Email sent successfully!")

    # CLOSE CONNECTION
    server.quit()

except Exception as e:
    print("❌ Error:", e)