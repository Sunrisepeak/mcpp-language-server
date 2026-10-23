export module target.real;

#if defined(_M_IX86) || defined(__i386__)
export constexpr int register_bits = 32;
#else
export constexpr int register_bits = 64;
#endif

static_assert(register_bits == sizeof(void*) * 8);
