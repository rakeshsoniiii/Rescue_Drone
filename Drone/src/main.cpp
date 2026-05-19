#include <Arduino.h>

// Define the LED pin (GPIO 2 on ESP32 S3 - built-in LED)
// You can change this to any GPIO pin you're using
#define LED_PIN 2

void setup() {
  // Initialize serial communication for debugging
  Serial.begin(115200);
  delay(1000);
  
  // Set LED pin as output
  pinMode(LED_PIN, OUTPUT);
  
  Serial.println("ESP32 S3 LED Blink Starting!");
  Serial.print("LED Pin: ");
  Serial.println(LED_PIN);
}

void loop() {
  // Turn LED ON
  digitalWrite(LED_PIN, HIGH);
  Serial.println("LED ON");
  delay(1000);  // Wait 1 second
  
  // Turn LED OFF
  digitalWrite(LED_PIN, LOW);
  Serial.println("LED OFF");
  delay(1000);  // Wait 1 second
}
