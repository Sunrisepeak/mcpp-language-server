// The package's lib root: every module of mcppls.platform, for a consumer that wants all of them.
// Importing the one module a file needs stays the norm; this exists because mcpp expects a
// root per library package.
export module mcppls.platform;

export import mcppls.platform.dirs;
export import mcppls.platform.env;
export import mcppls.platform.fs;
export import mcppls.platform.net;
export import mcppls.platform.preopen;
export import mcppls.platform.process;
export import mcppls.platform.stdio;
export import mcppls.platform.task;
export import mcppls.platform.toolenv;
export import mcppls.platform.toolrun;
