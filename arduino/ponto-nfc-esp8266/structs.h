#ifndef STRUCTS_H
#define STRUCTS_H

// Data structures
struct User {
  byte uid[4];
  char name[16 + 1];
  bool lastState;  // true = in, false = out
};

struct Record {
  byte uid[4];
  unsigned long timestamp;
  bool isIn;  // true = punch in, false = punch out
};

#endif