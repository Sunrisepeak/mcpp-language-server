export module target.platform;

export int architecture_bits() { return sizeof(void*) * 8; }
