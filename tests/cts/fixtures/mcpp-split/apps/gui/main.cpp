// Not part of the build: the entry point of a feature the build does not enable, importing a module nothing provides.
import std;
import hello.greet;
import hello.gui;

int main() {
    std::println("{}", hello::greet("gui"));
    return hello::gui::run();
}
