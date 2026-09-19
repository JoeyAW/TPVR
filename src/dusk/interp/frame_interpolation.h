#pragma once

#include <dolphin/mtx.h>

#include <cstdint>
#include <memory>

#ifdef __cplusplus
namespace dusk::interp {

void begin_record();
void end_record();
void begin_sim_tick();
uint64_t sim_tick_seq();
void begin_frame(float step);
float get_interpolation_step();

void request_presentation_sync();
bool presentation_sync_active();

bool is_enabled();

bool should_capture();

void record_final_mtx(Mtx m, const void* key);
void record_final_mtx(Mtx m);

void forget_mtx(const void* key);

bool lookup_replacement(const void* key, Mtx out);
bool lookup_concat_replacement(const void* lhs, const void* rhs, Mtx out);

// VR hand/item lag fix (2026-08-09, see vr-mod-notes section 20): VR's
// tracked-controller matrices are written directly into J3DModel joint
// buffers (setAnmMtx()) from a call site that runs once per real render
// frame -- but this same buffer's matrices are ALSO what record_final_mtx()
// records once per SIM TICK (~30Hz) from the game's ordinary once-per-tick
// draw pass, and lookup_replacement()/lookup_concat_replacement() always
// prefer that once-per-tick-interpolated cached value over whatever the raw
// buffer currently holds -- silently discarding VR's fresher per-frame write
// at actual GX-submission time regardless of how recently it happened. Mark
// a matrix's address here (using the SAME identity lookup_replacement()
// keys off) once VR has written a genuinely fresh value into it for this
// frame, and lookup_replacement()/lookup_concat_replacement() will use that
// raw value directly instead of substituting the stale interpolated one.
// Cleared automatically at the start of each new recording cycle
// (begin_record()) so a stale marked address can't outlive the model/joint
// slot it was captured for.
void mark_live_this_frame(const void* key);

void begin_presentation(float step);
void end_presentation();
bool is_presentation_active();

typedef void (*InterpolationCallBack)(void* pUserWork);
void add_interpolation_callback(InterpolationCallBack pCallBack, void* pUserWork);
void add_interpolation_callback(InterpolationCallBack pCallBack, void* pUserWork, std::shared_ptr<void> owner);

}  // namespace dusk::interp
#endif
