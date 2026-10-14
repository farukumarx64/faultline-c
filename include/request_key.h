#ifndef FAULTLINE_REQUEST_KEY_H
#define FAULTLINE_REQUEST_KEY_H

#include <stddef.h>
#include <stdint.h>

/* Opaque bytes. All zero means an unkeyed legacy submission, never a wire key. */
#define FAULTLINE_REQUEST_KEY_SIZE 16u

static inline int faultline_request_key_present(const uint8_t *key)
{
    for (size_t i = 0; i < FAULTLINE_REQUEST_KEY_SIZE; ++i) {
        if (key[i] != 0) { return 1; }
    }
    return 0;
}

#endif
