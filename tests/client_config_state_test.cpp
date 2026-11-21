#include <cassert>
#include <cstring>

#include "client_config_state.h"

int main() {
    ClientConfigState state;
    assert(state.valid() && state.revision == 1 && state.applied == 0);
    assert(!state.rollback());
    assert(state.publish("{\"theme\":\"dark\"}"));
    assert(state.revision == 2);
    assert(state.publish(state.current) && state.revision == 2);
    assert(!state.acknowledge(1) && state.applied == 0);
    assert(state.acknowledge(2) && state.applied == 2);
    assert(state.rollback() && state.revision == 3 && state.applied == 2);
    assert(std::strcmp(state.current, "{}") == 0);
    assert(std::strcmp(state.previous, "{\"theme\":\"dark\"}") == 0);
    assert(!state.acknowledge(2));

    ClientConfigState reloaded;
    std::memcpy(&reloaded, &state, sizeof(state));
    assert(reloaded.valid() && reloaded.acknowledge(3));
    reloaded.revision = UINT32_MAX;
    assert(!reloaded.publish("{\"theme\":\"light\"}") && !reloaded.rollback());
    std::memset(reloaded.current, 'x', sizeof(reloaded.current));
    assert(!reloaded.valid());
}
