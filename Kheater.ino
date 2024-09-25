//************************************************************
// nodeId = 
//
//************************************************************
#include "painlessMesh.h"

#define   MESH_PREFIX     "kennet"
#define   MESH_PASSWORD   "kennet123"
#define   MESH_PORT       5555

Scheduler userScheduler; 
painlessMesh  mesh;

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
      digitalWrite(13, LOW);
      digitalWrite(27, HIGH); //викл реле L
      break;
  }
}

void receivedCallback( uint32_t from, String &msg ) {

  String str1 = msg.c_str();
  String str2 = "he0";
  String str3 = "he1";
  String str4 = "he2";
  String str5 = "he3";
  String str6 = "he4";

  if (str1.equals(str2)) { // просто кулер
    heat = HE0;
  }

  if (str1.equals(str3)) { // кулер + нагрев L
    heat = HE1;
  }
  
  if (str1.equals(str4)) { // кулер + нагрев H
    heat = HE2;
  }
  
  if (str1.equals(str5)) { // кулер + нагрев L + H
    heat = HE3;
  }

  if (str1.equals(str6)) { // виключано
    heat = HE4;
  }
}


void setup() {
  Serial.begin(115200);

  pinMode(13, OUTPUT); //реле кулера
  pinMode(12, OUTPUT); //реле обороту корпуса
  pinMode(14, OUTPUT); //реле H
  pinMode(27, OUTPUT); //реле L

  digitalWrite(27, HIGH);
  digitalWrite(12, HIGH);
  digitalWrite(14, HIGH);

  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);

}

void loop() {

  heatcore ();

  mesh.update();
}
