// A plugin as it was written before the first extensions of version 1: its
// function table ends with last_error, and it knows nothing of limiting, noise
// or snapshots. It must keep working with a newer esim. The table is declared
// here as it was, not taken from the current header.
//
// A conductance between two terminals; config: {"g": ..}.

#include <cstdlib>
#include <cstring>
#include <string>

#include "plugin/esim_plugin.h"

namespace {

struct Instance {
    double g = 1e-3;
    esim_pos pos[4] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
};

void* create(const char* config, const char*, const esim_host*, char*, int32_t) {
    auto* inst = new Instance();
    const std::string json = config ? config : "";
    const size_t at = json.find("\"g\"");
    if (at != std::string::npos) inst->g = std::strtod(json.c_str() + json.find(':', at) + 1, nullptr);
    return inst;
}

void destroy(void* p) { delete static_cast<Instance*>(p); }

int32_t describe(void* p, esim_description* d) {
    Instance& s = *static_cast<Instance*>(p);
    d->n_nodes = 2;
    d->n_G = 4;
    d->pos_G = s.pos;
    d->thread_safe = 1;
    return 0;
}

int32_t evaluate(void* p, esim_eval* e) {
    const Instance& s = *static_cast<Instance*>(p);
    const double i = s.g * (e->w[0] - e->w[1]);
    e->f[0] = i;
    e->f[1] = -i;
    e->G[0] = s.g;
    e->G[1] = -s.g;
    e->G[2] = -s.g;
    e->G[3] = s.g;
    return 0;
}

struct OldTable {
    uint32_t size;
    uint32_t api_version;
    const char* name;
    void* (*create)(const char*, const char*, const esim_host*, char*, int32_t);
    void (*destroy)(void*);
    int32_t (*describe)(void*, esim_description*);
    int32_t (*evaluate)(void*, esim_eval*);
    void (*accept_step)(void*, double, const double*, int32_t, int32_t);
    double (*next_breakpoint)(void*, double);
    double (*max_step)(void*);
    void (*reset)(void*);
    const char* (*last_error)(void*);
};

const OldTable kOld = {sizeof(OldTable), ESIM_PLUGIN_API_VERSION, "legacy", create, destroy, describe,
                       evaluate,         nullptr,                 nullptr,  nullptr, nullptr,  nullptr};

}  // namespace

extern "C" ESIM_PLUGIN_EXPORT const esim_plugin_v1* esim_plugin_entry(void) {
    return reinterpret_cast<const esim_plugin_v1*>(&kOld);
}
