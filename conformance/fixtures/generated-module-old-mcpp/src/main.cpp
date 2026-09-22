import app.consumer;
import xpkg.lua_stdlib;

int main() {
    return consumerAnswer() + xpkg::lua_stdlib_answer();
}
