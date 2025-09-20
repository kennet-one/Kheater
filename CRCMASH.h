#include <stdlib.h>  // strtol
#include <stdio.h>   // snprintf

painlessMesh mesh;

// ===== CRC-8 Dallas (poly 0x31, ref 0x8C) =====
uint8_t crc8_dallas(const uint8_t* data, size_t len){
  uint8_t crc = 0;
  while (len--){
    uint8_t inbyte = *data++;
    for (uint8_t i = 8; i; --i){
      uint8_t mix = (crc ^ inbyte) & 0x01;
      crc >>= 1;
      if (mix) crc ^= 0x8C;
      inbyte >>= 1;
    }
  }
  return crc;
}

String addCrcStar(const String& s){
  uint8_t c = crc8_dallas((const uint8_t*)s.c_str(), s.length());
  char buf[4]; snprintf(buf, sizeof(buf), "%02X", c);
  return s + "*" + String(buf);
}

bool stripAndVerifyCrcStar(const String& in, String& out){
  int p = in.lastIndexOf('*');
  if (p < 0 || p+3 != (int)in.length()) return false; // CRC немає або не в кінці
  String hex = in.substring(p+1);
  char* endp = nullptr;
  long v = strtol(hex.c_str(), &endp, 16);
  if (!hex.length() || endp==nullptr || *endp!='\0' || v<0 || v>255) return false;
  String body = in.substring(0,p);
  uint8_t calc = crc8_dallas((const uint8_t*)body.c_str(), body.length());
  if (calc != (uint8_t)v) return false;
  out = body;
  return true;
}

// ===== Broadcast-обгортка: завжди додає CRC =====
inline void sendB(const String& s){ mesh.sendBroadcast(addCrcStar(s)); }

inline void sendS(uint32_t to, const String& s){ mesh.sendSingle(to, addCrcStar(s)); }

// ===== Невелика черга для дефер-обробки повідомлень =====
// (щоб не шити нічого з callback і уникнути реентрантності)
constexpr uint8_t QN = 8;   // кількість елементів
constexpr uint8_t QL = 64;  // макс довжина рядка (включно з '\0')

char inQ[QN][QL];
volatile uint8_t qh = 0, qt = 0;

inline uint8_t qInc(uint8_t i){ return (uint8_t)((i+1) % QN); }
inline bool qEmpty(){ return qh == qt; }

void qPush(const String& s){
  uint8_t n = qInc(qh);
  if (n == qt) qt = qInc(qt);      // перезапис найстарішого, якщо переповнилось
  size_t len = s.length(); if (len > QL-1) len = QL-1;
  memcpy(inQ[qh], s.c_str(), len);
  inQ[qh][len] = 0;
  qh = n;
}

bool qPop(String& out){
  if (qEmpty()) return false;
  out = String(inQ[qt]);
  qt = qInc(qt);
  return true;
}


// ===== Друга черга: (from, body) для точних адресних повідомлень =====
constexpr uint8_t Q2N = 8;   // елементів
// Використаємо той самий QL для довжини тіла
uint32_t inQ2From[Q2N];
char     inQ2Body[Q2N][QL];
volatile uint8_t q2h = 0, q2t = 0;

inline uint8_t q2Inc(uint8_t i){ return (uint8_t)((i+1) % Q2N); }
inline bool q2Empty(){ return q2h == q2t; }

void qPush2(uint32_t from, const String& s){
  uint8_t n = q2Inc(q2h);
  if (n == q2t) q2t = q2Inc(q2t);   // перезапис найстарішого
  size_t len = s.length(); if (len > QL-1) len = QL-1;
  memcpy(inQ2Body[q2h], s.c_str(), len);
  inQ2Body[q2h][len] = 0;
  inQ2From[q2h] = from;
  q2h = n;
}

bool qPop2(uint32_t& from, String& out){
  if (q2Empty()) return false;
  from = inQ2From[q2t];
  out = String(inQ2Body[q2t]);
  q2t = q2Inc(q2t);
  return true;
}

// ===== Callback: лише перевіряємо CRC і кладемо в чергу =====
void receivedCallback(uint32_t from, String &msg){
  // не ловимо власні бродкасти (запобігає ехо-циклам)
  if (from == mesh.getNodeId()) return;

  String m = msg; m.trim();
  if (!m.length()) return;

  String body;
  if (!stripAndVerifyCrcStar(m, body)) return;  // строгий CRC: некоректне — дроп

  qPush(body);  // НІЯКИХ sendB тут — тільки в чергу!
  qPush2(from, body);  // і в (from,body) чергу для адресних обробок
}