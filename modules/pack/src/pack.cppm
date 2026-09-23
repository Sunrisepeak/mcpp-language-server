// The package's lib root: every module of mcppls.pack, for a consumer that wants all of them.
// Importing the one module a file needs stays the norm; this exists because mcpp expects a
// root per library package.
export module mcppls.pack;

export import mcppls.pack.archive;
export import mcppls.pack.clangd;
export import mcppls.pack.fetch;
export import mcppls.pack.kit;
export import mcppls.pack.lock;
export import mcppls.pack.payload;
export import mcppls.pack.release;
export import mcppls.pack.targets;
