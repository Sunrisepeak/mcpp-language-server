// The package's lib root: every module of mcppls.base, for a consumer that wants all of them.
// Importing the one module a file needs stays the norm; this exists because mcpp expects a
// root per library package.
export module mcppls.base;

export import mcppls.base.error;
export import mcppls.base.glob;
export import mcppls.base.log;
export import mcppls.base.path;
export import mcppls.base.sha256;
export import mcppls.base.text;
export import mcppls.base.uri;
export import mcppls.base.version;
