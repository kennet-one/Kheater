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
  HE3
} HEAT = HE0;


void receivedCallback( uint32_t from, String &msg ) {

  String str1 = msg.c_str();
  String str2 = "he0";
  String str3 = "he1";
  String str4 = "he2";
  String str5 = "he3";

  if (str1.equals(str2)) { // просто кулер

  }

  if (str1.equals(str3)) { // кулер + нагрев L

  }
  
  if (str1.equals(str4)) { // кулер + нагрев H

  }
  
  if (str1.equals(str5)) { // кулер + нагрев L + H

  }
}


void setup() {
  Serial.begin(115200);

  pinMode(13, INPUT); //реле кулера
  pinMode(12, INPUT); //реле обороту корпуса
  pinMode(14, INPUT); //реле H
  pinMode(27, INPUT); //реле L

  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);

}

void loop() {

  mesh.update();
}
