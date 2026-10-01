module;
#include "wc0.h"
#include <nothere/c.h>

export module standin.c;

import std;

export namespace standin {
  int value_c() { return 1; }
}
