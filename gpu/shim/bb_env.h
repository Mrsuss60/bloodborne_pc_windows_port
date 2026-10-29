#ifndef BB_ENV_H
#define BB_ENV_H

#include <stdlib.h>

static inline const char* bb_env(const char* name) {
    return getenv(name);
}

static inline int bb_env_on(const char* name) {
    const char* value = getenv(name);
    return value && value[0] == '1';
}

static inline int bb_env_off(const char* name) {
    const char* value = getenv(name);
    return value && value[0] == '0';
}

static inline long long bb_env_int(const char* name, long long fallback) {
    const char* value = getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }
    const int hex = value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    return strtoll(value, NULL, hex ? 16 : 10);
}

static inline double bb_env_float(const char* name, double fallback) {
    const char* value = getenv(name);
    return value && value[0] ? strtod(value, NULL) : fallback;
}

#endif
