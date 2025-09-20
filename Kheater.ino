//************************************************************
// nodeId = 1812998333
//
//************************************************************
// нада ше добавити
// нада шось придумати з флагом авто в ручному режимі управленія
// внутрішній контроль температури
// фізичні кнопки не сенсорні, нахуй сенсорні кнопки
// відображення активного режиму
// при потері конекта з зовнішнім датчиком температури шоб переходив на встрояний
// КНОПКА ВКЛ ВИКЛ
// таймер последбнього ауто пакету шоб виключав нагрев

#include "painlessMesh.h" // фай фай меш
#include "CRCMASH.h"
#include <U8g2lib.h> // бібліотека дистплея
#include <Wire.h>  // I2C
#include "mash_parameter.h"
#include "IMG.h"

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE); // який і як підключаний дисплей

Scheduler userScheduler;
unsigned long he4t = 0;      // часті таймера для безпечного іимкнення
bool he4timer = false;       //

bool rotatos = LOW;          // оборот корпуча по умолчанію виключаний

float extemp = 26.7;        // зовнішня не з внутрішньго датчика температура яку нада підтримувати
bool extempflag = true;      // по умолчанію AUTO mod включано

enum HEAT {    // часть heatcore
  HE0,
  HE1,
  HE2,
  HE3,
  HE4
} heat = HE4;     // по умолчанію виключанно НО AUTO mod має більший пріоритет

void heatcore () {     // главний робочий цикл обогревателя
  switch (heat) {
    case HE0:
      digitalWrite(13, HIGH);   // вкл кулер
      digitalWrite(14, HIGH);   // викл реле H
      digitalWrite(27, HIGH);   // викл реле L
      break;

    case HE1:
      digitalWrite(13, HIGH);  // вкл кулер
      digitalWrite(27, LOW);   // вкл реле L
      digitalWrite(14, HIGH);  // викл реле H
      break;

    case HE2:
      digitalWrite(13, HIGH);  // вкл кулер
      digitalWrite(14, LOW);   // вкл реле H
      digitalWrite(27, HIGH);  // викл реле L
      break;
    case HE3:
      
      break;
    case HE4:
      digitalWrite(13, LOW); // викл кулер
      digitalWrite(27, HIGH); //викл реле L
      digitalWrite(14, HIGH); //викл реле H
      
      digitalWrite(26, LOW); // викл оборотне реле
      break;
  }
}

void heatfeedback () {    // обратна связь главного цикла
  switch (heat) {
    case HE0:
      if (extempflag) {
        sendB("A5");
      } else {
        sendB("250");
      }
      break;
    case HE1:
      if (extempflag) {
        sendB("A5");
      } else {
        sendB("251");
      }
      break;
    case HE2:
      if (extempflag) {
        sendB("A5");
      } else {
        sendB("252");
      }
      break;
    case HE3:
        sendB("253");
      break;
    case HE4:
        sendB("254");
      break;
  }
}

void safetimer () { // цей таймер запускаєця коли нада охладити нагреватель перед виключенням
  if (he4timer) {
    if (millis() - he4t >= 30000) { // 30 секунд
      heat = HE4;
      he4timer = false; 
    }
  }
}

void rotaation () { // обороти корпуса
  rotatos = !rotatos;
  digitalWrite(26, rotatos);
  sendB("09" + (rotatos ? String("1") : String("0")));
}



// === Deferred handler: original callback body moved here (works on verified 'body') ===
void handleBodyFrom(uint32_t from, const String& body){
          // прийомка MASH сеті
  String str1 = body;
  String str2 = "he0";
  String str3 = "he1";
  String str4 = "he2";
  String str5 = "he3";
  String str6 = "he4";
  String str7 = "he5";
  String str8 = "hero";
  String str9 = "heho";

  if (str1.equals(str2)) { // просто кулер
    heat = HE0;
    heatfeedback();
  }
  else if (str1.equals(str3)) { // кулер + нагрів L
    heat = HE1;
    heatfeedback();
  }
  else if (str1.equals(str4)) { // кулер + нагрів H
    heat = HE2;
    heatfeedback();
  }
  else if (str1.equals(str5)) { // кулер + нагрів L + H
    heat = HE3;
    heatfeedback();
  }
  else if (str1.equals(str7)) { // вкл ауто мод
    extempflag = true;
  }
  else if (str1.equals(str6)) { // виключено
    heat = HE0;
    he4t = millis();
    he4timer = true;
    heatfeedback();
    extempflag = false;
  }
  else if (str1.equals(str8)) { // оборот корпуса
    rotaation();
  }
  else if (str1.equals(str9)) { // eho
    heatfeedback();
    sendB("09" + (rotatos ? String("1") : String("0")));     // обратна связь оборота корпуса
    sendB("R5" + String(extemp));                            // обратна связь усттановляної тесператури для підтримування
  }
  else if (str1.startsWith("05")) {  // AUTO мод

    String tempString = str1.substring(2); // Отримуємо підрядок після перших двох символів
    float temperature = tempString.toFloat();

    if (tempString.length() > 0) {
      sendB("A5");      // обратна связь активації AUTO moda

      if (extempflag) {
        float diff = extemp - temperature;
        if (diff > 0) {

          if (diff > 0.5) { // Якщо різниця більша за 0.5 градуса
            heat = HE2; 
          } else {
            heat = HE1;
          }
        } else { // Якщо реальна температура >= заданої
          heat = HE0;
        }
      }
    }
  }
  else if (str1.startsWith("W5")) { // тут устанавлюем яку температуру буде підтримувати AUTO мод

    String tempString = str1.substring(2); // Отримуємо підрядок після перших двох символів
    if (tempString.length() > 0) {
      sendB("R5" + tempString); // тут наверно нада поміняти на екстемп
      extemp = tempString.toFloat();    // устанавлюем підтримувану температуру
    }
  }

}




void setup() {
  Serial.begin(115200);

  pinMode(13, OUTPUT); //реле кулера
  pinMode(26, OUTPUT); //реле обороту корпуса
  pinMode(14, OUTPUT); //реле H
  pinMode(27, OUTPUT); //реле L

  digitalWrite(27, HIGH);
  digitalWrite(26, LOW);
  digitalWrite(14, HIGH);

  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);

  u8g2.begin();                                           // ініт дисплея
  u8g2.drawBitmap(32, 0, 8, 64, logo);               // прінт лого
  u8g2.sendBuffer();                                      // висилка буфера)
  delay(3000);                                            // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  u8g2.enableUTF8Print();  
  u8g2.setFont(u8g2_font_cu12_t_cyrillic);                // підтримка шрифтом українського текста 
  u8g2.clearBuffer();
  u8g2.sendBuffer();
}

void loop() {
  // --- deferred CRC queue processing (addressed) ---
  for (uint8_t __i=0; __i<3; ++__i){
    uint32_t __from; String __body;
    if (!qPop2(__from, __body)) break;
    handleBodyFrom(__from, __body);
  }

  safetimer ();
  heatcore ();

  mesh.update();
}
