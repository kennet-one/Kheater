//************************************************************
// nodeId = 1812998333
//
//************************************************************
// нада ше добавити
//
// фітбек на змінення окремих опцій
// фітбек на всьо разом
// зовнішній контроль температури
// внутрішній контроль температури
// дисплей
// фізичні кнопки не сенсорні, нахуй сенсорні кнопки
#include "painlessMesh.h"

#define   MESH_PREFIX     "kennet"
#define   MESH_PASSWORD   "kennet123"
#define   MESH_PORT       5555

Scheduler userScheduler; 
painlessMesh  mesh;

unsigned long he4t = 0;
bool he4timer = false;

bool rotatos = LOW;

float extemp = 27.10;

enum HEAT {
  HE0,
  HE1,
  HE2,
  HE3,
  HE4
} heat = HE4;

void heatcore () {
  switch (heat) {
    case HE0:
      digitalWrite(13, HIGH);  // вкл кулер

      digitalWrite(27, HIGH);   //викл реле L
      break;

    case HE1:
      digitalWrite(13, HIGH);  // вкл кулер
      digitalWrite(27, LOW); //вкл реле L
      break;

    case HE2:
      
      break;
    case HE3:
      
      break;
    case HE4:
      digitalWrite(13, LOW); // викл кулер
      digitalWrite(27, HIGH); //викл реле L
      
      digitalWrite(26, LOW); // викл оборотне реле
      break;
  }
}

void heatfeedback () {
  switch (heat) {
    case HE0:
      mesh.sendSingle(624409705,"250");
      break;
    case HE1:
      mesh.sendSingle(624409705,"251");
      break;
    case HE2:
      mesh.sendSingle(624409705,"252");
      break;
    case HE3:
      mesh.sendSingle(624409705,"253");
      break;
    case HE4:
      mesh.sendSingle(624409705,"254");
      break;
  }
}

void safetimer () {
  if (he4timer) {
    if (millis() - he4t >= 30000) { // 30 секунд
      heat = HE4;
      he4timer = false; 
    }
  }
}

void rotaation () {
  rotatos = !rotatos;
  digitalWrite(26, rotatos);
  mesh.sendSingle(624409705, "09" + (rotatos ? String("1") : String("0")));
}

void receivedCallback( uint32_t from, String &msg ) {
  String str1 = msg.c_str();
  String str2 = "he0";
  String str3 = "he1";
  String str4 = "he2";
  String str5 = "he3";
  String str6 = "he4";
  String str8 = "hero";

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
  else if (str1.equals(str6)) { // виключено
    heat = HE0;
    he4t = millis();
    he4timer = true;
    heatfeedback();
  }
  else if (str1.equals(str8)) { // просто кулер
    rotaation();
  }
  else if (str1.startsWith("05")) {

    String tempString = str1.substring(2); // Отримуємо підрядок після перших двох символів
    float temperature = tempString.toFloat();

    if (tempString.length() > 0) {
      //Serial.println(temperature);

      if (temperature < extemp) {
        heat = HE1;; // меньше заданої температури
      } else {
        heat = HE0;; // більше
      }
    }
  }
  else if (str1.startsWith("W5")) {

    String tempString = str1.substring(2); // Отримуємо підрядок після перших двох символів
    if (tempString.length() > 0) {
      mesh.sendSingle(624409705, "R5" + tempString);
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

}

void loop() {

  safetimer ();
  heatcore ();

  mesh.update();
}
