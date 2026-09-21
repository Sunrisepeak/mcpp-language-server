module hello.greet:counter;

namespace hello::detail {
int& calls() {
    static int value = 0;
    return value;
}

void bump() {
    ++calls();
}

int value() {
    return calls();
}
}
