#line 1 "src/app.cppm"
export module app;

export int answer() { return 42; }
export struct Point {
    int x;
    int y;
    int sum() const {
        if (x > 0) {
            return x + y;
        }
        return y;
    }
};
