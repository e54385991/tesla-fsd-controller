#pragma once

#include <stdint.h>
#include <string.h>

constexpr size_t kClientConfigCapacity = 1025;

struct ClientConfigState {
    uint32_t revision = 1;
    uint32_t applied = 0;
    char current[kClientConfigCapacity] = "{}";
    char previous[kClientConfigCapacity] = {};

    bool valid() const {
        return revision != 0 && applied <= revision && current[0] != '\0' &&
            memchr(current, '\0', sizeof(current)) && memchr(previous, '\0', sizeof(previous));
    }

    bool publish(const char* json) {
        const size_t length = strlen(json);
        if (revision == UINT32_MAX || length == 0 || length >= sizeof(current)) return false;
        if (strcmp(current, json) == 0) return true;
        strcpy(previous, current);
        memcpy(current, json, length + 1);
        ++revision;
        return true;
    }

    bool rollback() {
        if (revision == UINT32_MAX || previous[0] == '\0') return false;
        char restored[kClientConfigCapacity];
        strcpy(restored, previous);
        return publish(restored);
    }

    bool acknowledge(uint32_t reportedRevision) {
        if (reportedRevision != revision) return false;
        applied = revision;
        return true;
    }
};
