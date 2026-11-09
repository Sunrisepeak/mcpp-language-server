module;
#include "wb0.h"
#include <nothere/b.h>

export module standin.b;

import std;

export namespace standin {
  int value_b() { return 1; }
}
