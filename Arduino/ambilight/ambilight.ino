#include <FastLED.h>
#define NUM_LEDS 112
#define DATA_PIN 13
#define SERIAL_RATE 115200

CRGB leds[NUM_LEDS];
CRGB back;

void setup() { 
	FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);

  back = CRGB(0,0,0);

	for (int i = 0; i < NUM_LEDS; i ++){
		leds[i] = CRGB(0,0,0);
	}
	
	FastLED.show();
  	Serial.begin(SERIAL_RATE);
}

void loop() {
  for (int i = 0; i < NUM_LEDS; i ++){
    leds[i] = back;
  }
  byte c1 = 0, c2 = 0;
  
  while (Serial.available() <= 0) ;
  c1 = Serial.read();

  while (Serial.available() <= 0);
  c2 = Serial.read();

  if (c1 == 'B' && c2 == 'k') {
    byte r, g, b;

    while (Serial.available() <= 0);
    r = Serial.read();
    
    while (Serial.available() <= 0);
    g = Serial.read();
    
    while (Serial.available() <= 0);
    b = Serial.read();

    back = CRGB(r, g, b);
    Serial.write("Do");
    fill_solid(leds, NUM_LEDS, back);

    FastLED.show();
  }

  if (c1 != 'F' && c2 != 'u') return;
  
	// Read the transmission data and set LED values
	for (uint8_t i = 0; i < NUM_LEDS; i++) {
		byte r, g, b;

    while (Serial.available() <= 0);
		r = Serial.read();
    
    while (Serial.available() <= 0);
		g = Serial.read();
    
    while (Serial.available() <= 0);
		b = Serial.read();

		leds[i].r = r;
		leds[i].g = g;
		leds[i].b = b;
	}
 
	FastLED.show();
}