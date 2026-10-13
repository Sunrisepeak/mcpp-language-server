export module base;

// Compiles on its own: the error is only in what an importer instantiates.
export template <class T>
int twice(T value) { return value.no_such_member() * 2; }
