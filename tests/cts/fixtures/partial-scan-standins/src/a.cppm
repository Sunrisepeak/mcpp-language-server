module;
#include <nothere/a.h>

export module standin.a;

import std;

export namespace standin {
  int value_a() { return 1; }
}
