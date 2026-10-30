/* cppcc freestanding <string.h> for Novus OS.
 *
 * The functions are written in the supported C++ subset so that a single
 * translation unit gets working implementations without a runtime library.
 */
#ifndef _CPPCC_STRING_H
#define _CPPCC_STRING_H

#include <stddef.h>

static inline void* memcpy(void* destination, const void* source, size_t count) {
    unsigned char* to = (unsigned char*)destination;
    const unsigned char* from = (const unsigned char*)source;
    for (size_t index = 0; index < count; index++) {
        to[index] = from[index];
    }
    return destination;
}

static inline void* memmove(void* destination, const void* source, size_t count) {
    unsigned char* to = (unsigned char*)destination;
    const unsigned char* from = (const unsigned char*)source;
    if (to < from) {
        for (size_t index = 0; index < count; index++) {
            to[index] = from[index];
        }
    } else {
        size_t index = count;
        while (index > 0) {
            index--;
            to[index] = from[index];
        }
    }
    return destination;
}

static inline void* memset(void* destination, int value, size_t count) {
    unsigned char* to = (unsigned char*)destination;
    for (size_t index = 0; index < count; index++) {
        to[index] = (unsigned char)value;
    }
    return destination;
}

static inline int memcmp(const void* left, const void* right, size_t count) {
    const unsigned char* first = (const unsigned char*)left;
    const unsigned char* second = (const unsigned char*)right;
    for (size_t index = 0; index < count; index++) {
        if (first[index] != second[index]) {
            return (int)first[index] - (int)second[index];
        }
    }
    return 0;
}

static inline size_t strlen(const char* text) {
    size_t length = 0;
    while (text[length] != 0) {
        length++;
    }
    return length;
}

static inline int strcmp(const char* left, const char* right) {
    while (*left != 0 && *left == *right) {
        left++;
        right++;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

static inline int strncmp(const char* left, const char* right, size_t count) {
    size_t index = 0;
    while (index < count && left[index] != 0 && left[index] == right[index]) {
        index++;
    }
    if (index == count) {
        return 0;
    }
    return (int)(unsigned char)left[index] - (int)(unsigned char)right[index];
}

static inline char* strcpy(char* destination, const char* source) {
    size_t index = 0;
    while (source[index] != 0) {
        destination[index] = source[index];
        index++;
    }
    destination[index] = 0;
    return destination;
}

static inline char* strncpy(char* destination, const char* source, size_t count) {
    size_t index = 0;
    while (index < count && source[index] != 0) {
        destination[index] = source[index];
        index++;
    }
    while (index < count) {
        destination[index] = 0;
        index++;
    }
    return destination;
}

static inline char* strcat(char* destination, const char* source) {
    size_t offset = strlen(destination);
    strcpy(destination + offset, source);
    return destination;
}

static inline char* strchr(const char* text, int character) {
    while (*text != 0) {
        if (*text == (char)character) {
            return (char*)text;
        }
        text++;
    }
    return (char*)0;
}

static inline char* strrchr(const char* text, int character) {
    const char* last = (const char*)0;
    while (*text != 0) {
        if (*text == (char)character) {
            last = text;
        }
        text++;
    }
    return (char*)last;
}

#endif
