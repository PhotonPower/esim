// Two small plugins in one library, for testing the plugin interface and as a
// template for real ones. Both have a closed-form counterpart in esim's own
// devices, so that the tests can compare results.
//
//   "solenoid"  Electromagnet with a position-dependent inductance
//                   L(z) = L0 / (1 + z / z0),   winding resistance R.
//               Terminals a, b; input z (position); branch coil (a -> b);
//               output F = i^2 / 2 dL/dz.
//               config: {"kind": "solenoid", "R": .., "L0": .., "z0": ..}
//
//   "diode"     A diode with series resistance Rs and junction capacitance Cj,
//               like a semiconductor simulator exposes a device: terminals
//               anode and cathode, the voltage over the junction is an
//               internal unknown y.
//               config: {"kind": "diode", "Rs": .., "Is": .., "Cj": ..,
//                        "limit": 1, "voltage": 1}
//               "limit" switches the SPICE-like limiting of the junction voltage
//               on, "voltage" declares the internal unknown as a voltage. The
//               diode reports its thermal and shot noise and writes snapshots.
//
// The library is C++ only for convenience; the interface is the C header.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "plugin/esim_plugin.h"

namespace {

double number(const std::string& json, const char* key, double fallback) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t at = json.find(pattern);
    if (at == std::string::npos) return fallback;
    at = json.find(':', at);
    if (at == std::string::npos) return fallback;
    return std::strtod(json.c_str() + at + 1, nullptr);
}

std::string text(const std::string& json, const char* key) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t at = json.find(pattern);
    if (at == std::string::npos) return "";
    at = json.find('"', json.find(':', at));
    if (at == std::string::npos) return "";
    const size_t end = json.find('"', at + 1);
    return end == std::string::npos ? "" : json.substr(at + 1, end - at - 1);
}

struct Instance {
    enum Kind { Solenoid, Diode } kind = Solenoid;
    double R = 1.0, L0 = 1e-2, z0 = 5e-3;  // solenoid
    double Rs = 10.0, Is = 1e-14, Cj = 1e-9;  // diode
    bool limit = false, voltageKind = false;
    double yold = 0.0;                        // reference point of the limiter
    std::vector<int32_t> kinds, noiseRows;
    std::vector<const char*> noiseNames;
    std::vector<esim_pos> posG, posC;
    std::vector<const char*> inNames, brNames, outNames, intNames;
    std::vector<int32_t> brNodes;
    std::string error;
    int evaluations = 0;
};

void* create(const char* config, const char*, const esim_host*, char* err, int32_t errLen) {
    const std::string json = config ? config : "";
    auto* inst = new Instance();
    const std::string kind = text(json, "kind");
    if (kind == "solenoid") {
        inst->kind = Instance::Solenoid;
        inst->R = number(json, "R", inst->R);
        inst->L0 = number(json, "L0", inst->L0);
        inst->z0 = number(json, "z0", inst->z0);
    } else if (kind == "diode") {
        inst->kind = Instance::Diode;
        inst->Rs = number(json, "Rs", inst->Rs);
        inst->Is = number(json, "Is", inst->Is);
        inst->Cj = number(json, "Cj", inst->Cj);
        inst->limit = number(json, "limit", 0.0) != 0.0;
        inst->voltageKind = number(json, "voltage", 0.0) != 0.0;
    } else {
        std::snprintf(err, static_cast<size_t>(errLen), "unknown kind \"%s\" (solenoid, diode)", kind.c_str());
        delete inst;
        return nullptr;
    }
    return inst;
}

void destroy(void* p) { delete static_cast<Instance*>(p); }

// Local indices of the solenoid: a 0, b 1, z 2, i 3, F 4.
// Local indices of the diode: anode 0, cathode 1, y 2.
int32_t describe(void* p, esim_description* d) {
    Instance& s = *static_cast<Instance*>(p);
    d->n_nodes = 2;
    if (s.kind == Instance::Solenoid) {
        s.inNames = {"z"};
        s.brNames = {"coil"};
        s.outNames = {"F"};
        s.brNodes = {0, 1};
        d->n_inputs = 1;
        d->n_branches = 1;
        d->n_outputs = 1;
        d->input_names = s.inNames.data();
        d->branch_names = s.brNames.data();
        d->output_names = s.outNames.data();
        d->branch_nodes = s.brNodes.data();
        s.posG = {{3, 3}, {3, 0}, {3, 1}, {4, 4}, {4, 3}, {4, 2}};
        s.posC = {{3, 3}, {3, 2}};
    } else {
        s.intNames = {"y"};
        d->n_internal = 1;
        d->internal_names = s.intNames.data();
        s.posG = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 0}, {2, 1}, {2, 2}};
        s.posC = {{2, 2}};
        if (s.voltageKind) {
            s.kinds = {ESIM_KIND_VOLTAGE};
            d->kinds = s.kinds.data();
        }
        // thermal noise of Rs (terminal a to the internal node), shot noise of the junction
        s.noiseRows = {0, 2, 2, 1};
        s.noiseNames = {"rs", "shot"};
        d->n_noise = 2;
        d->noise_rows = s.noiseRows.data();
        d->noise_names = s.noiseNames.data();
    }
    d->n_G = static_cast<int32_t>(s.posG.size());
    d->n_C = static_cast<int32_t>(s.posC.size());
    d->pos_G = s.posG.data();
    d->pos_C = s.posC.data();
    d->thread_safe = s.limit ? 0 : 1;  // the limiter keeps a reference point
    return 0;
}

int32_t evaluate(void* p, esim_eval* e) {
    Instance& s = *static_cast<Instance*>(p);
    const double* w = e->w;
    ++s.evaluations;
    if (s.kind == Instance::Solenoid) {
        const double va = w[0], vb = w[1], z = w[2], i = w[3];
        const double u = 1.0 + z / s.z0;
        const double L = s.L0 / u;
        const double dL = -s.L0 / (s.z0 * u * u);
        const double d2L = 2.0 * s.L0 / (s.z0 * s.z0 * u * u * u);
        e->f[3] = s.R * i - (va - vb);
        e->q[3] = L * i;
        e->f[4] = w[4] - 0.5 * i * i * dL;
        e->G[0] = s.R;
        e->G[1] = -1.0;
        e->G[2] = 1.0;
        e->G[3] = 1.0;
        e->G[4] = -i * dL;
        e->G[5] = -0.5 * i * i * d2L;
        e->C[0] = L;
        e->C[1] = dL * i;
    } else {
        const double vt = 1.380649e-23 * e->temperature / 1.602176634e-19;
        const double v = w[0] - w[1], y = w[2];
        // limiting of the junction voltage (SPICE pnjlim), see the header
        double ye = y;
        if (s.limit && e->limit_mode == ESIM_LIMIT_ON) {
            const double vcrit = vt * std::log(vt / (std::sqrt(2.0) * s.Is));
            if (y > vcrit && std::abs(y - s.yold) > 2.0 * vt) {
                if (s.yold > 0.0) {
                    const double arg = 1.0 + (y - s.yold) / vt;
                    ye = arg > 0.0 ? s.yold + vt * std::log(arg) : vcrit;
                } else {
                    ye = vt * std::log(y / vt);
                }
                e->limited = 1;
            }
        }
        if (s.limit && e->limit_mode != ESIM_LIMIT_OFF) s.yold = ye;
        const double a = ye / vt, amax = 40.0;
        // exponential, continued linearly beyond amax (keeps Newton calm)
        const double ex = a < amax ? std::exp(a) : std::exp(amax) * (1.0 + (a - amax));
        const double dex = a < amax ? std::exp(a) : std::exp(amax);
        const double ir = (v - y) / s.Rs;
        const double gdl = s.Is * dex / vt;
        e->f[0] = ir;
        e->f[1] = -ir;
        e->f[2] = s.Is * (ex - 1.0) + gdl * (y - ye) - ir;  // back to the real y by the tangent
        e->q[2] = s.Cj * y;
        const double g = 1.0 / s.Rs;
        const double gd = s.Is * dex / vt;
        const double G[9] = {g, -g, -g, -g, g, g, -g, g, g + gd};
        std::memcpy(e->G, G, sizeof G);
        e->C[0] = s.Cj;
    }
    return 0;
}

const char* lastError(void* p) { return static_cast<Instance*>(p)->error.c_str(); }

int32_t noise(void* p, const double* w, int32_t, double temperature, double* white, double* flicker) {
    const Instance& s = *static_cast<Instance*>(p);
    const double k = 1.380649e-23, q = 1.602176634e-19, vt = k * temperature / q;
    const double a = w[2] / vt, amax = 40.0;
    const double ex = a < amax ? std::exp(a) : std::exp(amax) * (1.0 + (a - amax));
    white[0] = 4.0 * k * temperature / s.Rs;
    white[1] = 2.0 * q * std::abs(s.Is * (ex - 1.0));
    flicker[0] = flicker[1] = 0.0;
    return 0;
}

int32_t snapshot(void*, double t, const double* w, int32_t n, const char* dir, const char* tag) {
    const std::string path = std::string(dir) + "/" + tag + ".snap.txt";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return 1;
    std::fprintf(f, "t %.9g\nn %d\n", t, n);
    for (int32_t k = 0; k < n; ++k) std::fprintf(f, "%.12g\n", w[k]);
    std::fclose(f);
    return 0;
}

const esim_plugin_v1 kApi = {sizeof(esim_plugin_v1),
                             ESIM_PLUGIN_API_VERSION,
                             "mock",
                             create,
                             destroy,
                             describe,
                             evaluate,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             lastError,
                             noise,
                             snapshot};

}  // namespace

extern "C" ESIM_PLUGIN_EXPORT const esim_plugin_v1* esim_plugin_entry(void) { return &kApi; }
