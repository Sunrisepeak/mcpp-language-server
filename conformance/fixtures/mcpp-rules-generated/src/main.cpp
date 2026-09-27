#include "ui_form.h"

import std;
import hello.greet;

int main(int argc, char* argv[]) {
    Ui::Form form;
    std::println("{} {}", hello::greet("mcpp"), form.rows);
    return 0;
}
