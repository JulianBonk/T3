#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "HX711.h"

// --- Pins für HX711 ---
#define HX711_DT 10
#define HX711_SCK 11

// --- Pins für Taster ---
#define BALANCE_PIN 12
#define MODE_PIN 13

// --- Pin für ADC der Batterie ---
#define BAT_PIN 4

// --- Display Setup ---
#define GFX_DEV_DEVICE LILYGO_T_DISPLAY_S3_AMOLED
Arduino_DataBus *bus = new Arduino_ESP32QSPI(6, 47, 18, 7, 48, 5);
Arduino_GFX *gfx = new Arduino_RM67162(bus, 17, 3);  // Rotation 1 = Querformat
Arduino_GFX *gfx2;

HX711 scale;

const int storageSize = 536;        // As many pixels we have
const int displayHeight = 240;
const int displayWidth = 536;

long values[storageSize];
int currentIndex = 0;
float maxValue = 0.002; /// NOch ein define bauen!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!


float calibratedOffset = 2638;
float calibratedScalingFactor = 0.00000045855;
float blancedOffset = 0;
int modeNbr = 1;

// --- Farben und Design ---
#define BACKGROUND_COLOR BLACK
#define TEXT_COLOR GREEN
#define HIGHLIGHT_COLOR RED

#define LOADCELL_REFERENCE_VOLTAGE 2.74
#define AMPLIFIER_GAIN 64.0
#define FULL_SCALE_INPUT_MV 2
#define BOOT_PIN 0

unsigned long lastSampleTime = 0;
const unsigned long sampleInterval = 1000 / 80;  // 80 SPS ≈ 12 ms
unsigned long lastDrawTime = 0;
const unsigned long drawInterval = 200;          // Display alle 200 ms

void setBrightness(uint8_t value) {
  bus->beginWrite();
  bus->writeC8D8(0x51, value);
  bus->endWrite();
}

float calculateMvPerV(long raw) {
  return raw * calibratedScalingFactor;
}

double calculateTrimR(float avMvPerV) {
  double uSoll = 0.5 - (avMvPerV / 1000);
  double Rtotal = (uSoll * 350) / (1 - uSoll);
  double rTrim = 1.0 / ((1.0 / Rtotal) - (1.0 / 350.0));
  return rTrim / 1000;
}

float calculatePromil(double TrimR) {
  const float promil[19][2] = {
    {1740000, 0.2}, {874065, 0.4}, {582923, 0.6}, {437150, 0.8},
    {349650, 1.0}, {291316, 1.2}, {218000, 1.6}, {194094, 1.8},
    {145483, 2.4}, {109025, 3.2}, {96872, 3.6}, {72566, 4.8},
    {54337, 6.4}, {48261, 7.2}, {36108, 9.6}, {26993, 12.8},
    {23995, 14.4}, {17879, 19.2}, {11803, 28.8},
  };
  long minDiff = abs((TrimR * 1000) - promil[0][0]);
  float closest = promil[0][1];
  for (int i = 1; i < 19; i++) {
    long diff = abs((TrimR * 1000) - promil[i][0]);
    if (diff < minDiff) {
      minDiff = diff;
      closest = promil[i][1];
    }
  }
  return closest;
}

float calculateBat() {
  int rawBat = analogRead(BAT_PIN);
  return (rawBat * 1.61) / 1000;
}

float calculateTemp(float avMvPerV) {
  if (avMvPerV > 0) {
    double UPt100 = 100.0 / 22100.0;
    double zaehler = (UPt100 + (avMvPerV / 1000.0)) * 22000.0;
    double nenner = 1.0 - UPt100 + (avMvPerV / 1000.0);
    double resistancePt100 = zaehler / nenner;
    double temp = (resistancePt100 - 100) / (100 * 0.00391);
    return temp;
  }
  return 0;
}




void balance(float avMvPerV) {
  blancedOffset = avMvPerV;
  maxValue = 0.0001;   //

  gfx2->fillScreen(BLACK);

  gfx2->setTextColor(ORANGE);
  gfx2->setTextSize(8, 8);
  gfx2->setCursor(50, 80);
  gfx2->println("BALANCED");
  gfx2->flush();

  while (digitalRead(BALANCE_PIN) == LOW) {
    delay(50);
  }
}

void drawGraph(float mvPerV, float avMvPerV, float promil, float batVoltage) {
  int graphHeight = 100;               // Höhe des Graphen
  int centerY = displayHeight / 2;     // Zentrale Y-Position (Null-Linie)
  int lastY = centerY;                 // Startwert für die vorherige Y-Position

  gfx2->fillScreen(BACKGROUND_COLOR);  // Hintergrund löschen

  // Überschrift "ch1 Graph" zeichnen
  gfx2->setTextColor(HIGHLIGHT_COLOR);
  gfx2->setTextSize(3);                // Textgröße anpassen
  gfx2->setCursor(2, 2);               // Position
  gfx2->print("ch1 RollingGraph");

  // Nulllinie (Mittelachse) zeichnen
  gfx2->drawLine(0, centerY, displayWidth - 1, centerY, LIGHTGREY);

  // 50% Ausschlag-Linien zeichnen (positive und negative Linie)
  int fiftyPercentOffset = (int)(graphHeight * 0.25); // 50% vom maximalen Ausschlag
  int y50p = centerY - fiftyPercentOffset;
  int yMinus50p = centerY + fiftyPercentOffset;

  gfx2->drawLine(0, y50p, displayWidth - 1, y50p, YELLOW);  // Positive 50%-Linie
  gfx2->drawLine(0, yMinus50p, displayWidth - 1, yMinus50p, YELLOW);  // Negative 50%-Linie

  // Rechteck um den Graphen zeichnen (Ränder)
  int graphMargin = 10;
  gfx2->drawRect(graphMargin, centerY - graphHeight / 2 - graphMargin, displayWidth - graphMargin * 2, graphHeight + graphMargin * 2, WHITE);

  // Start bei der letzten Position im Array
  int x = storageSize - 1;
  while (x >= 0) {
    int pos = (currentIndex - (storageSize - 1 - x) + storageSize) % storageSize;

    // Rohwert aus dem Array holen
    int raw = values[pos];
    float mvPerV = calculateMvPerV(raw - calibratedOffset - blancedOffset);  // Berechne den Wert

    // Um die Mitte (Null-Linie) skalieren, gleiche Skalierung wie im Bargraph
    int yOffset = (int)(mvPerV / maxValue * graphHeight);  // Umrechnung des mvPerV in Y-Offset
    int y = centerY - yOffset;  // Y-Wert anpassen, sodass die Null-Linie in der Mitte bleibt
    y = constrain(y, centerY - graphHeight / 2, centerY + graphHeight / 2);  // Begrenzung des Y-Werts innerhalb des Graphenbereichs

    // Nur Linien zwischen den Punkten zeichnen
    if (x != storageSize - 1) {
      gfx2->drawLine(x + 1, lastY, x, y, HIGHLIGHT_COLOR);  // Zeige Linie vom letzten Punkt zur aktuellen Position
    }

    lastY = y;  // Aktuellen Y-Wert speichern
    x--;  // Zur nächsten X-Position bewegen
  }

  gfx2->flush();  // Alles an den Bildschirm senden
}

void drawOneValue(float mvPerV, float avMvPerV, float promil, float batVoltage) {


  char buf[20];
  snprintf(buf, sizeof(buf), "%+.6f", avMvPerV);  // Vorzeichen und 6 Dezimalstellen

  // Den Wert in zwei Teile aufteilen
  String fullValue = String(buf);                                     // Der gesamte formatierte Wert als String
  String firstPart = fullValue.substring(0, fullValue.length() - 3);  // Alle bis auf die letzten 3 Ziffern
  String lastPart = fullValue.substring(fullValue.length() - 3);      // Die letzten 3 Ziffern

  gfx2->fillScreen(BACKGROUND_COLOR);

  gfx2->setTextColor(HIGHLIGHT_COLOR);
  gfx2->setTextSize(4, 4);
  gfx2->setCursor(420, 60);  //  gfx2->setCursor(10, 20);
  gfx2->println("mV/V");

  gfx2->setTextColor(TEXT_COLOR);
  gfx2->setTextSize(7, 7);
  gfx2->setTextColor(GREEN);
  gfx2->setCursor(20, 40);
  gfx2->print(firstPart);

  // Den letzten Teil in GrünGrau ausgeben
  uint16_t green = RGB565(0, 160, 0);
  gfx2->setTextColor(green);
  gfx2->print(lastPart);


  int barWidth = 436;
  int barHeight = 25;
  int barX = 50;
  int barY = 5;  //200

  int filled = (int)(mvPerV / maxValue * barWidth);
  if (filled > barWidth) filled = barWidth;

  uint16_t barColor = GREEN;
  if (abs(mvPerV) > maxValue / 2 * 0.8) barColor = RED;
  else if (abs(mvPerV) > maxValue / 2 * 0.6) barColor = YELLOW;

  gfx2->drawRect(barX - 1, barY - 1, barWidth + 2, barHeight + 2, WHITE);
  gfx2->fillRect(barX + barWidth / 2, barY, filled, barHeight, barColor);


  double TrimR = calculateTrimR(avMvPerV);

  gfx2->setTextSize(2, 2);
  gfx2->setTextColor(WHITE);
  gfx2->setCursor(21, 107);
  gfx2->print("Required Trim Resistor");



  gfx2->setTextSize(3, 3);
  gfx2->setTextColor(GREEN);

  gfx2->setCursor(287, 100);


  if (abs(TrimR) > 99999) {
    double TrimRMega = TrimR / 1000;                        //MegaOhm
    snprintf(buf, sizeof(buf), "%+*.*f", 6, 0, TrimRMega);  // Vorzeichen und 6 Dezimalstellen [kOhm]
    gfx2->print(buf);

    gfx2->setCursor(420, 100);
    gfx2->setTextColor(RED);
    gfx2->print("MOhm ");
  }

  else {

    snprintf(buf, sizeof(buf), "%+*.*f", 6, 0, TrimR);  // Vorzeichen und 6 Dezimalstellen [kOhm]
    gfx2->print(buf);

    gfx2->setCursor(420, 100);
    gfx2->setTextColor(RED);
    gfx2->print("kOhm ");
  }

  //Promille Ausgabe
  gfx2->setTextSize(2, 2);
  gfx2->setTextColor(WHITE);
  gfx2->setCursor(215, 140);
  gfx2->print("Promil");

  uint16_t promilColor = RGB565(250, 0, 250);
  gfx2->setTextSize(3, 3);
  gfx2->setTextColor(promilColor);
  gfx2->setCursor(310, 136);
  snprintf(buf, sizeof(buf), "%*.*f", 3, 1, promil);
  gfx2->print(buf);
  
  gfx2->setCursor(420, 136);
  gfx2->setTextColor(promilColor);
  gfx2->print("% ");

  gfx2->setCursor(430, 136);
  gfx2->setTextColor(promilColor);
  gfx2->print(". ");
  
  //Batteriespannung ausgabe 
  gfx2->setTextSize(2, 2);
  gfx2->setTextColor(WHITE);
  gfx2->setCursor(93, 180);
  gfx2->print("BatVoltage");

    //Farbe in Abhängigkeit der Spannung ändern 
  uint16_t batColor = RGB565(250, 250, 0);
  if (batVoltage < 3.6) batColor = RED;
  else if (batVoltage > 3.65) batColor = GREEN;
  
  gfx2->setTextSize(3, 3);
  gfx2->setCursor(310, 176);
  gfx2->setTextColor(batColor);
  snprintf(buf, sizeof(buf), "%*.*f", 2, 2, batVoltage);
  gfx2->print(buf);

  gfx2->setCursor(420, 176);
  gfx2->setTextColor(batColor);
  gfx2->print("V ");
  

  


  gfx2->flush();
}


float getFilteredAverage() {
  int count = 5;
  int recentValues[5];

  // Letzte 5 Werte holen (mit Ringpuffer-Logik)
  for (int i = 0; i < count; i++) {
    int index = (currentIndex - 1 - i + storageSize) % storageSize;
    recentValues[i] = values[index];
  }

  // Min und Max finden
  int minVal = recentValues[0];
  int maxVal = recentValues[0];
  int sum = recentValues[0];

  for (int i = 1; i < count; i++) {
    if (recentValues[i] < minVal) minVal = recentValues[i];
    if (recentValues[i] > maxVal) maxVal = recentValues[i];
    sum += recentValues[i];
  }

  // Summe minus min und max
  int filteredSum = sum - minVal - maxVal;

  // Mittelwert aus 3 Werten berechnen
  float average = filteredSum / 3.0;

  return average;
}

void mode() {
  modeNbr++;

  gfx2->fillScreen(BLACK);

  gfx2->setTextColor(YELLOW);
  gfx2->setTextSize(8, 8);
  gfx2->setCursor(100, 80);
  gfx2->print("MODE ");
  gfx2->println(modeNbr);
  gfx2->flush();

  int i = 0;
  while (digitalRead(MODE_PIN) == LOW) {
    delay(25);
    i++;
    if(i > 30){
      modeNbr = 1;
     gfx2->fillScreen(BLACK);
    gfx2->setTextColor(GREEN);
    gfx2->setTextSize(8, 8);
    gfx2->setCursor(30, 80);
    gfx2->print("MODE RESET");
    gfx2->flush();
    i = 0;
    }
    
  }
}



void setup() {
  pinMode(BAT_PIN, INPUT);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  pinMode(BOOT_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  for (int i = 0; i < storageSize; i++) values[i] = 0;

  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx2 = new Arduino_Canvas(displayWidth, displayHeight, gfx, 0, 0);
  gfx2->begin(GFX_SKIP_OUTPUT_BEGIN);
  gfx2->fillScreen(BACKGROUND_COLOR);
  gfx2->flush();
  setBrightness(150);

  scale.begin(HX711_DT, HX711_SCK);
  scale.set_gain(128);
  Serial.println(scale.is_ready() ? "HX711 bereit." : "HX711 nicht bereit!");

  pinMode(BALANCE_PIN, INPUT_PULLUP);
  pinMode(MODE_PIN, INPUT_PULLUP);
}

void loop() {
  unsigned long now = millis();

  if (now - lastSampleTime >= sampleInterval && scale.is_ready()) {
    lastSampleTime = now;
    long raw = scale.read();
    values[currentIndex] = raw;
    currentIndex = (currentIndex + 1) % storageSize;
  }

  if (now - lastDrawTime >= drawInterval) {
    lastDrawTime = now;

    long avRaw = getFilteredAverage();
    float mvPerV = calculateMvPerV(values[(currentIndex - 1 + storageSize) % storageSize] - calibratedOffset - blancedOffset);
    float avMvPerV = calculateMvPerV(avRaw - calibratedOffset - blancedOffset);
    double TrimR = calculateTrimR(avMvPerV);
    float promil = calculatePromil(abs(TrimR));
    float batVoltage = calculateBat();

    if (!digitalRead(BALANCE_PIN)) balance(avRaw - calibratedOffset);
    if (!digitalRead(MODE_PIN)) mode();

    if (maxValue / 2 < abs(avMvPerV)) {
      maxValue = abs(avMvPerV) * 2;
      Serial.println("updateMaxVal");
    }

    switch (modeNbr) {
      case 1:
        drawOneValue(mvPerV, avMvPerV, promil, batVoltage);
        break;
      case 2:
        drawGraph(mvPerV, avMvPerV, promil, batVoltage);
        break;
      default:
        drawOneValue(mvPerV, avMvPerV, promil, batVoltage);
        break;
    }
  }
}
