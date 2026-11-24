/* esim plugin interface, version 1.
 *
 * A plugin is a shared library that contributes a device with its own
 * equations to a circuit: a field solver (FEM: windings, force), a
 * semiconductor simulator (contacts, drift-diffusion unknowns), a thermal or
 * mechanical model, a neural network, ... esim does not know what is inside; it
 * only sees a local system of differential-algebraic equations
 *
 *     d/dt q(w) + f(w, t) = 0
 *
 * in the unknowns w, in the same charge-oriented form as esim's own devices.
 *
 * The interface is plain C (fixed-size integers, doubles, pointers), so a
 * plugin can be built with any compiler and language and needs no esim header
 * other than this one.
 *
 * Local index space. The vector w (and the arrays f, q) has the blocks
 *
 *     [ nodes | inputs | branches | outputs | internal ]
 *       n_nodes n_inputs n_branches n_outputs n_internal
 *
 *   nodes     terminals of the device. w holds the node voltages (ground: 0).
 *             The plugin writes the current that leaves the node into the
 *             device in f (and the charge in q), as a SPICE device does.
 *   inputs    values from the circuit, given in the netlist as expressions of
 *             voltages, currents, variables, time (a position, a speed, a
 *             temperature, ...). Read only. A plugin never writes a row for
 *             an input.
 *   branches  currents through the device between two terminals (a winding).
 *             esim creates the unknown, adds the current to the two terminal
 *             rows and gives the plugin the row of the branch equation, e.g.
 *             f = R*i - (v(p) - v(n)), q = flux linkage.
 *   outputs   values the plugin hands back (a force, a power loss, ...). esim
 *             creates a variable for each one, so that equations and outputs
 *             of the netlist can use it. The plugin writes its row, e.g.
 *             f = F - F_field(...).
 *   internal  unknowns of the plugin: field degrees of freedom, carrier
 *             densities, states. Their rows belong to the plugin.
 *
 * The plugin therefore delivers n_nodes + n_branches + n_outputs + n_internal
 * rows (the rows of the input block stay 0), which is the number of unknowns
 * esim adds for it (plus the node rows, which belong to the circuit).
 *
 * Jacobians. The plugin lists the position of every non-zero of G = df/dw and
 * C = dq/dw once, in describe(), and only fills the values in evaluate().
 * Derivatives with respect to inputs are taken with respect to the input
 * value; esim applies the chain rule to the expression.
 *
 * Extension. All structs start with their size. esim fills in the size it
 * knows and the plugin only reads and writes members inside it; new members
 * are always appended, so old plugins and old hosts keep working. Optional
 * functions may be NULL.
 *
 * Limiting. In a Newton iteration (limit_mode = ESIM_LIMIT_ON) the plugin may
 * evaluate at a point damped towards the previous iterate, as SPICE does for
 * pn junctions, and sets limited = 1; esim then does not accept convergence at
 * this point. With ESIM_LIMIT_INIT the plugin stores w as its new reference
 * point (the start of a step, an accepted solution). The reference point is
 * the only state evaluate() may change.
 *
 * Threads. esim may call evaluate() from several threads at once, unless the
 * description says thread_safe = 0 (the default): then calls are serialized.
 */
#ifndef ESIM_PLUGIN_H
#define ESIM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESIM_PLUGIN_API_VERSION 1

#if defined(_WIN32)
#define ESIM_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ESIM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Analysis mode of an evaluation. */
#define ESIM_MODE_DC 0    /* operating point, no time derivatives */
#define ESIM_MODE_INIT 1  /* initial point of a transient analysis */
#define ESIM_MODE_TRAN 2  /* time step */

/* Kind of an unknown the plugin owns (esim_description::kinds). esim treats the
 * kinds differently: tolerances (a voltage in volt, a current in ampere), and
 * in the search for an operating point (gmin stepping, pseudo-transient) esim
 * adds a small conductance to the rows of VOLTAGE unknowns. Default: branches
 * are currents, outputs and internal unknowns are variables. */
#define ESIM_KIND_DEFAULT 0
#define ESIM_KIND_VOLTAGE 1
#define ESIM_KIND_CURRENT 2
#define ESIM_KIND_VARIABLE 3

/* Limiting of Newton iterates (esim_eval::limit_mode), as in SPICE. */
#define ESIM_LIMIT_OFF 0   /* plain evaluation at w; keep no reference point */
#define ESIM_LIMIT_INIT 1  /* plain evaluation at w; remember w as reference point */
#define ESIM_LIMIT_ON 2    /* Newton iteration: the plugin may evaluate at a point damped
                              towards its reference point, must then set limited = 1,
                              and moves the reference point along */

/* Services of esim for the plugin. Passed to create(); valid until destroy(). */
typedef struct esim_host {
    uint32_t size;
    uint32_t api_version;
    /* level: 0 note, 1 warning. The message is a single line of text. */
    void (*log)(int32_t level, const char* message);
} esim_host;

typedef struct esim_pos {
    int32_t row, col;  /* local indices, see above */
} esim_pos;

/* What a device instance looks like. esim sets everything to 0 and size to
 * sizeof(esim_description) before calling describe(). The arrays belong to the
 * plugin and stay valid until destroy(). */
typedef struct esim_description {
    uint32_t size;

    int32_t n_nodes;     /* terminals: the "nodes" of the component in the netlist */
    int32_t n_inputs;
    int32_t n_branches;
    int32_t n_outputs;
    int32_t n_internal;

    /* Names for the netlist and for messages. input_names and output_names are
     * required (the netlist refers to them), the others may be NULL. */
    const char* const* input_names;     /* n_inputs */
    const char* const* branch_names;    /* n_branches */
    const char* const* output_names;    /* n_outputs */
    const char* const* internal_names;  /* n_internal */

    /* Terminals (index into the nodes) between which a branch current flows,
     * from p through the device to n: p0, n0, p1, n1, ... */
    const int32_t* branch_nodes;

    /* Per unknown of branches, outputs and internal (in this order, so
     * n_branches + n_outputs + n_internal values): start value of the Newton
     * iteration and absolute tolerance. NULL: 0 and esim's default. A
     * tolerance <= 0 also means the default. */
    const double* start;
    const double* abstol;

    /* Non-zeros of the Jacobians. */
    int32_t n_G, n_C;
    const esim_pos* pos_G;
    const esim_pos* pos_C;

    /* 1: evaluate() may be called from several threads at once. */
    int32_t thread_safe;

    /* --- appended in the first revision of version 1 --- */

    /* Kinds of the unknowns of branches, outputs and internal (in this order,
     * n_branches + n_outputs + n_internal values of ESIM_KIND_*); NULL: all
     * default. */
    const int32_t* kinds;

    /* Noise sources: a current from row a to row b (local indices of rows the
     * plugin owns or of terminals; -1 is ground), with the spectral density
     * white + flicker / f in A^2/Hz. The positions are fixed; the values come
     * from the noise() function at the operating point. */
    int32_t n_noise;
    const int32_t* noise_rows;           /* 2 * n_noise: a0, b0, a1, b1, ... */
    const char* const* noise_names;      /* n_noise, may be NULL */
} esim_description;

/* One evaluation at time t and local vector w. The plugin writes the rows of
 * f and q (n_local = all blocks, esim has set them to 0) and the values of
 * G and C in the order of the positions it declared. */
typedef struct esim_eval {
    uint32_t size;
    double t;
    int32_t mode;        /* ESIM_MODE_* */
    double temperature;  /* circuit temperature in kelvin */
    int32_t n_local;
    const double* w;
    double* f;
    double* q;
    double* G;           /* n_G values */
    double* C;           /* n_C values */

    /* --- appended in the first revision of version 1 --- */
    int32_t limit_mode;  /* ESIM_LIMIT_* (input) */
    int32_t limited;     /* output, 0 on entry: 1 if f, q, G, C belong to a damped point */
} esim_eval;

typedef struct esim_plugin_v1 {
    uint32_t size;       /* sizeof(esim_plugin_v1) */
    uint32_t api_version;  /* ESIM_PLUGIN_API_VERSION */
    const char* name;

    /* config_json: the "config" object of the netlist (plugin and component
     * merged, text of a JSON object). base_dir: directory of the netlist, for
     * files the plugin reads. Returns an instance or NULL with a message in
     * err (at most err_len bytes, zero terminated). */
    void* (*create)(const char* config_json, const char* base_dir, const esim_host* host,
                    char* err, int32_t err_len);
    void (*destroy)(void* instance);

    /* 0 on success. */
    int32_t (*describe)(void* instance, esim_description* out);

    /* 0 on success; anything else ends the analysis with the message of
     * last_error() (or a general message if there is no such function). */
    int32_t (*evaluate)(void* instance, esim_eval* eval);

    /* --- optional -------------------------------------------------------- */
    /* Point of the time integration: the start (start = 1) or an accepted
     * step. The place to advance a history the plugin keeps itself. */
    void (*accept_step)(void* instance, double t, const double* w, int32_t n_local, int32_t start);
    /* Smallest time > t at which the equations are not smooth. */
    double (*next_breakpoint)(void* instance, double t);
    /* Largest sensible time step. */
    double (*max_step)(void* instance);
    /* Before every analysis: forget what earlier analyses left behind. */
    void (*reset)(void* instance);
    const char* (*last_error)(void* instance);

    /* --- appended in the first revision of version 1 --- */

    /* Spectral densities of the noise sources declared in the description, at
     * the operating point w: n_noise values each. 0 on success. */
    int32_t (*noise)(void* instance, const double* w, int32_t n_local, double temperature,
                     double* white, double* flicker);

    /* Writes the state w (fields over the grid, bands, densities, ...) for
     * inspection. dir is a directory that exists; tag is a file name stem
     * that identifies analysis, time and component (the plugin adds its own
     * extension). Called at the end of an operating point and of a transient
     * analysis, and at given times of a transient, if the netlist asks for it
     * ("snapshots" of the component). 0 on success. */
    int32_t (*snapshot)(void* instance, double t, const double* w, int32_t n_local,
                        const char* dir, const char* tag);
} esim_plugin_v1;

/* Every plugin library exports this function. */
typedef const esim_plugin_v1* (*esim_plugin_entry_fn)(void);
#define ESIM_PLUGIN_ENTRY "esim_plugin_entry"

#ifdef __cplusplus
}
#endif

#endif /* ESIM_PLUGIN_H */
