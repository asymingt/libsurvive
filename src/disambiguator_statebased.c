//
#include "survive_internal.h"
#include <assert.h>
#include <math.h> /* for sqrt */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//#define DEBUG_TB(...) SV_INFO(__VA_ARGS__)
#define DEBUG_TB(...)
/**
 * The lighthouses go in the following order:
 *
 *     Ticks  State
 *         0  ACode 0b1x0 (4) <--- B
 *    20 000  ACode 0b0x0 (0) <--- A/c
 *            LH A X Sweep
 *   400 000  ACode 0b1x1 (5) <--- B
 *   420 000  ACode 0b0x1 (1) <--- A/c
 *            LH A Y SWEEP
 *   800 000  ACode 0b0x0 (0) <--- B
 *   820 000  ACode 0b1x0 (4) <--- A/c
 *            LH B X Sweep
 * 1 200 000  ACode 0b0x1 (1) <--- B
 * 1 220 000  ACode 0b1x1 (5) <--- A/c
 *            LH B Y SWEEP
 * 1 600 000  < REPEAT >
 *
 * NOTE: Obviously you cut the data bit out for this
 *
 * This disambiguator works by finding where in that order it is, and tracking along with it.
 * It is able to maintain this tracking for extended periods of time without further data
 * by knowing the modulo of the start of the cycle and calculating appropriatly although this
 * will run into issues when the timestamp rolls over or we simply drift off in accuracy.
 *
 * Neither case is terminal though; it will just have to find the modulo again which only takes
 * a handful of pulses.
 *
 * The main advantage to this scheme is that its reasonably fast and is able to deal with being
 * close enough to the lighthouse that the lengths are in a valid sync pulse range.
 */

// Every pulse_window seems roughly 20k ticks long. That leaves ~360 to the capture window
#define PULSE_WINDOW 20000
#define CAPTURE_WINDOW 360000

enum LighthouseState {
	LS_UNKNOWN = 0,

	LS_WaitLHA_ACode4 = 1,
	LS_WaitLHA_ACode0,
	LS_SweepAX,
	LS_WaitLHA_ACode5,
	LS_WaitLHA_ACode1,
	LS_SweepAY,
	LS_WaitLHB_ACode0,
	LS_WaitLHB_ACode4,
	LS_SweepBX,
	LS_WaitLHB_ACode1,
	LS_WaitLHB_ACode5,
	LS_SweepBY,

	LS_END
};

typedef struct {
	int acode, lh, axis, window;
	bool is_sweep;
} LighthouseStateParameters;

// clang-format off
const LighthouseStateParameters LS_Params[LS_END + 1] = {
	{.lh = -1, .axis = -1, .window = 0},

    {.acode = 4, .lh = 1, .axis = 0, .window = PULSE_WINDOW},                // 0
    {.acode = 0, .lh = 0, .axis = 0, .window = PULSE_WINDOW},                // 20000
    {.acode = 4, .lh = 0, .axis = 0, .window = CAPTURE_WINDOW, .is_sweep = 1}, // 40000

    {.acode = 5, .lh = 1, .axis = 1, .window = PULSE_WINDOW},                // 400000
    {.acode = 1, .lh = 0, .axis = 1, .window = PULSE_WINDOW},                // 420000
    {.acode = 1, .lh = 0, .axis = 1, .window = CAPTURE_WINDOW, .is_sweep = 1}, // 440000

    // In 60hz single LH mode, it just repeats the above. With any other configuration, the second half of the table is used.
    {.acode = 0, .lh = 1, .axis = 0, .window = PULSE_WINDOW},                // 800000
	{.acode = 4, .lh = 0, .axis = 0, .window = PULSE_WINDOW},                // 820000
	{.acode = 4, .lh = 1, .axis = 0, .window = CAPTURE_WINDOW, .is_sweep = 1}, // 840000

	{.acode = 1, .lh = 1, .axis = 1, .window = PULSE_WINDOW},                // 1200000
	{.acode = 5, .lh = 0, .axis = 1, .window = PULSE_WINDOW},                // 1220000
	{.acode = 5, .lh = 1, .axis = 1, .window = CAPTURE_WINDOW, .is_sweep = 1}, // 1240000

	{.lh = -1, .axis = -1, .window = 0}                          // 1600000
};
// clang-format on

#define ACODE_TIMING(acode)                                                                                            \
	((3000 + ((acode)&1) * 500 + (((acode) >> 1) & 1) * 1000 + (((acode) >> 2) & 1) * 2000) - 250)
#define ACODE(s, d, a) ((s << 2) | (d << 1) | a)

static inline int LSParam_acode(enum LighthouseState s) { return LS_Params[s].acode; }

static int LSParam_offset_for_state(enum LighthouseState s) {
	static int offsets[LS_END + 1] = {-1};
	if (offsets[0] == -1) {
		int offset = 0;
		for (int i = 0; i < LS_END + 1; i++) {
			offsets[i] = offset;
			offset += LS_Params[i].window;
		}
	}
	return offsets[s];
}

static enum LighthouseState LighthouseState_findByOffset(int offset, int *error) {
	for (int i = 2; i < LS_END + 1; i++) {
		if (LSParam_offset_for_state(i) > offset) {
			int offset_from_last = LSParam_offset_for_state(i - 1);
			int offset_from_this = LSParam_offset_for_state(i);

			int dist_from_last = offset - offset_from_last;
			int dist_from_this = offset_from_this - offset;

			bool this_is_closest = dist_from_last > dist_from_this;
			if (LS_Params[i - 1].is_sweep && dist_from_this > 1000) {
				this_is_closest = false;
			}

			if (error) {
				*error = this_is_closest ? dist_from_this : dist_from_last;
			}
			return this_is_closest ? i : i - 1;
		}
	}
	assert(false);
	return -1;
}

typedef struct {
	SurviveContext *ctx;

	bool single_60hz_mode;
} Global_Disambiguator_data_t;

typedef struct {
	SurviveObject *so;
	/* Keep running average of sync signals as they come in */
	uint32_t last_timestamp;
	uint64_t last_sync_timestamp;
	uint64_t last_sync_length;
	int last_sync_count;

	uint32_t first_sync_timestamp;
	uint32_t longest_sync_length;

	/**  This part of the structure is general use when we know our state */
	enum LighthouseState state;

	// We track offset for both lighthouses seperately
	uint32_t mod_offset[NUM_LIGHTHOUSES];
	int confidence;

	/** This rest of the structure is dedicated to finding a state when we are unknown */
	int stabalize;
	int failures;
	bool lastWasSync;

#define SYNC_HISTORY_LEN 12
	LightcapElement sync_history[SYNC_HISTORY_LEN];
	int sync_offset;

	LightcapElement sweep_data[];
} Disambiguator_data_t;

static int find_acode(uint32_t pulseLen) {
	const static int offset = 50;
	if (pulseLen < 2500 + offset)
		return -1;

	if (pulseLen < 3000 + offset)
		return 0;
	if (pulseLen < 3500 + offset)
		return 1;
	if (pulseLen < 4000 + offset)
		return 2;
	if (pulseLen < 4500 + offset)
		return 3;
	if (pulseLen < 5000 + offset)
		return 4;
	if (pulseLen < 5500 + offset)
		return 5;
	if (pulseLen < 6000 + offset)
		return 6;
	if (pulseLen < 6500 + offset)
		return 7;

	return -1;
}

static bool overlaps(const LightcapElement *a, const LightcapElement *b) {
	int overlap = 0;
	if (a->timestamp < b->timestamp && a->length + a->timestamp > b->timestamp)
		overlap = a->length + a->timestamp - b->timestamp;
	else if (b->timestamp < a->timestamp && b->length + b->timestamp > a->timestamp)
		overlap = b->length + b->timestamp - a->timestamp;

	return overlap > a->length / 2;
}

const int SKIP_BIT = 4;
const int DATA_BIT = 2;
const int AXIS_BIT = 1;

#define LOWER_SYNC_TIME 2250
#define UPPER_SYNC_TIME 6750

#define DIV_ROUND_CLOSEST(n, d) ((((n) < 0) ^ ((d) < 0)) ? (((n) - (d) / 2) / (d)) : (((n) + (d) / 2) / (d)))

LightcapElement get_last_sync(Disambiguator_data_t *d) {
	if (d->last_sync_count == 0) {
		return (LightcapElement){0};
	}
	LightcapElement lastSync = {
		.timestamp = d->first_sync_timestamp, .length = d->longest_sync_length, .sensor_id = -d->last_sync_count};
	return lastSync;
}

enum LightcapClassification { LCC_SWEEP, LCC_SYNC };
static enum LightcapClassification naive_classify(Disambiguator_data_t *d, const LightcapElement *le) {
	bool clearlyNotSync = le->length < LOWER_SYNC_TIME || le->length > UPPER_SYNC_TIME;

	if (clearlyNotSync) {
		return LCC_SWEEP;
	} else {
		return LCC_SYNC;
	}
}

static uint32_t SolveForMod_Offset(Disambiguator_data_t *d, enum LighthouseState state, const LightcapElement *le) {
	assert(LS_Params[state].is_sweep == 0); // Doesn't work for sweep data
	SurviveContext *ctx = d->so->ctx;
	DEBUG_TB("Solve for mod %d (%u - %u) = %u", state, le->timestamp, LSParam_offset_for_state(state),
			 (le->timestamp - LSParam_offset_for_state(state)));

	return (le->timestamp - LSParam_offset_for_state(state));
}

static enum LighthouseState SetState(Disambiguator_data_t *d, const LightcapElement *le,
									 enum LighthouseState new_state);

static enum LighthouseState EndSweep(Disambiguator_data_t *d, const LightcapElement *le) { return LS_UNKNOWN; }

static void AddSyncHistory(Disambiguator_data_t *d, LightcapElement sync) {
	if (sync.length) {
		d->sync_history[d->sync_offset++] = sync;
		if (d->sync_offset >= SYNC_HISTORY_LEN)
			d->sync_offset = 0;
	}
}

static Disambiguator_data_t *get_best_latest_state(Global_Disambiguator_data_t *g) {
	int max_confidence = 0;
	Disambiguator_data_t *best_d = 0;
	for (int i = 0; i < g->ctx->objs_ct; i++) {
		Disambiguator_data_t *d = g->ctx->objs[i]->disambiguator_data;
		if (d && d->state != LS_UNKNOWN && max_confidence < d->confidence) {
			best_d = d;
			max_confidence = d->confidence;
		}
	}

	return best_d;
}

static uint32_t calculate_error(int target_acode, const LightcapElement *le) {
	// Calculate what it would be with and without data
	uint32_t time_error_d0 = abs(ACODE_TIMING(target_acode) - le->length);
	uint32_t time_error_d1 = abs(ACODE_TIMING(target_acode | DATA_BIT) - le->length);

	// Take the least of the two erors
	return (time_error_d0) > (time_error_d1) ? time_error_d1 : time_error_d0;
}

#define DEBUG_LOCK DEBUG_TB
static uint32_t apply_mod_offset(uint32_t timestamp, uint32_t mod_offset, enum LighthouseState end_state) {
	int mod_group = LSParam_offset_for_state(end_state);
	if (timestamp > mod_offset)
		return (timestamp - mod_offset) % mod_group;

	// Indicates mod_offset was from _before_ a 32bit rollover
	if (mod_offset - timestamp > 0xFFFFFFFF / 2) {
		return (0xFFFFFFFF - mod_offset + timestamp) % mod_group;
	}

	timestamp = timestamp % mod_group;
	mod_offset = mod_offset % mod_group;

	int rtn = ((int32_t)timestamp - (int32_t)mod_offset) % mod_group;
	if (rtn < 0)
		rtn += mod_group;
	return rtn;
}

static int find_inliers(Disambiguator_data_t *d, uint32_t guess_mod, bool test60hz) {
	int inliers = 0;
	SurviveContext *ctx = d->so->ctx;
	for (int i = 0; i < SYNC_HISTORY_LEN && d->sync_history[i].length > 0; i++) {
		const LightcapElement *le = &d->sync_history[i];

		int end_of_mod = test60hz ? LS_WaitLHB_ACode0 : LS_END;
		int le_offset = apply_mod_offset(le->timestamp, guess_mod, end_of_mod);

		int offset_error;
		enum LighthouseState this_state = LighthouseState_findByOffset(le_offset, &offset_error);

		int acode = LSParam_acode(this_state);
		uint32_t error = calculate_error(acode, le);

		DEBUG_LOCK("--%d %10u %10u %4u %d(%d) \t %2d %u %u %u %d", i, le_offset, le->timestamp, le->length, acode,
				   LS_Params[this_state].lh, this_state, ACODE_TIMING(acode), ACODE_TIMING(acode | DATA_BIT), error,
				   offset_error);

		if (LS_Params[this_state].is_sweep)
			continue;

		if (LS_Params[this_state].lh && test60hz)
			continue;

		if (error < 500 && offset_error < 500) {
			inliers++;
		}
	}
	return inliers;
}

static enum LighthouseState find_relative_offset(Disambiguator_data_t *d, uint32_t *mod, bool *single_60hz) {
	SurviveContext *ctx = d->so->ctx;
	Global_Disambiguator_data_t *g = d->so->ctx->disambiguator_data;
	Disambiguator_data_t *best_d = get_best_latest_state(g);

	int ri = (d->sync_offset + (SYNC_HISTORY_LEN - 1)) % SYNC_HISTORY_LEN;
	LightcapElement *re = d->sync_history + ri;
	int acode = find_acode(re->length) & 0x5;

	DEBUG_LOCK("Starting search... %s %d %d", d->so->codename, ri, acode);
	for (enum LighthouseState guess = LS_UNKNOWN + 1; guess != LS_END; guess++) {
		const LighthouseStateParameters *params = &LS_Params[guess];
		if (LSParam_acode(guess) == acode && !params->is_sweep) {
			uint32_t guess_mod = SolveForMod_Offset(d, guess, re);
			DEBUG_LOCK("%10u %4u %d %u %u %d", re->timestamp, re->length, acode & 0x5, guess_mod,
					   re->timestamp - guess_mod, guess);

			for (int test60hz = 0; test60hz < ((guess >= LS_WaitLHB_ACode0) ? 1 : 2); test60hz++) {
				// We are already locked on one device; so we know if its 60hz mode or not
				if (best_d && test60hz != g->single_60hz_mode)
					continue;

				int inliers = find_inliers(d, guess_mod, test60hz);
				DEBUG_LOCK("With 60hz -- %d %d", test60hz, inliers);
				if (inliers > SYNC_HISTORY_LEN - 1) {
					*mod = guess_mod;
					*single_60hz = test60hz == 1;
					return guess;
				}
			}
		}
	}

	return LS_UNKNOWN;
}

static enum LighthouseState EndSync(Disambiguator_data_t *d, const LightcapElement *le) {
	LightcapElement lastSync = get_last_sync(d);
	Global_Disambiguator_data_t *g = d->so->ctx->disambiguator_data;

	AddSyncHistory(d, lastSync);

	uint32_t mod = 0;
	bool is60hz;
	enum LighthouseState new_state = find_relative_offset(d, &mod, &is60hz);
	if (new_state != LS_UNKNOWN) {
		d->mod_offset[0] = d->mod_offset[1] = mod;
		g->single_60hz_mode = is60hz;
		if (g->single_60hz_mode) {
			SurviveContext *ctx = d->so->ctx;
			SV_INFO("Disambiguator is in 60hz mode (mode A)");
		}
		return new_state;
	} else {
		return LS_UNKNOWN;
	}
}

static void RegisterSync(Disambiguator_data_t *d, const LightcapElement *le) {
	if (le->timestamp < d->first_sync_timestamp || d->longest_sync_length == 0)
		d->first_sync_timestamp = le->timestamp;

	if (le->length > d->longest_sync_length) {
		d->longest_sync_length = le->length;
	}

	d->last_sync_timestamp += le->timestamp;
	d->last_sync_length += le->length;
	d->last_sync_count++;
}

static void ResetSync(Disambiguator_data_t *d) {
	d->first_sync_timestamp = d->longest_sync_length = 0;
	d->last_sync_timestamp = d->last_sync_length = d->last_sync_count = 0;
}

static enum LighthouseState AttemptFindState(Disambiguator_data_t *d, const LightcapElement *le) {
	/*
	enum LighthouseState best_guess = get_best_latest_state(d->so->ctx->disambiguator_data);
	if(best_guess != LS_UNKNOWN) {
		SurviveContext* ctx = d->so->ctx;
		SV_INFO("Disambiguator solving state by stealing other trackers state");
		d->mod_offset = SolveForMod_Offset(d, best_guess, le);
		return best_guess;
	}
*/

	enum LightcapClassification classification = naive_classify(d, le);

	if (classification == LCC_SYNC) {
		LightcapElement lastSync = get_last_sync(d);

		// Handle the case that this is a new SYNC coming in
		if (d->lastWasSync == false || overlaps(&lastSync, le) == false) {
			// Now that the previous two states are in, check to see if they tell us where we are
			enum LighthouseState new_state = d->lastWasSync ? EndSync(d, le) : EndSweep(d, le);
			if (new_state != LS_UNKNOWN)
				return new_state;

			// Otherwise, just reset the sync registers and do another

			ResetSync(d);
		}

		RegisterSync(d, le);
		d->lastWasSync = true;
	} else {
		// If this is the start of a new sweep, check to see if the end of the sync solves
		// the state
		if (d->lastWasSync) {
			enum LighthouseState new_state = EndSync(d, le);
			if (new_state != LS_UNKNOWN)
				return new_state;
		}
		d->lastWasSync = false;
	}

	return LS_UNKNOWN;
}

static enum LighthouseState SetState(Disambiguator_data_t *d, const LightcapElement *le,
									 enum LighthouseState new_state) {

	SurviveContext *ctx = d->so->ctx;
	Global_Disambiguator_data_t *g = ctx->disambiguator_data;

	if (new_state >= LS_END)
		new_state = 1;

	if (d->state == LS_UNKNOWN && new_state != LS_UNKNOWN) {
		Disambiguator_data_t *best_d = get_best_latest_state(g);
		DEBUG_TB("Setting state to %d for %s, best state is %d", new_state, d->so->codename,
				 best_d ? best_d->state : LS_UNKNOWN);
	}

	d->state = new_state;
	if (new_state == LS_UNKNOWN) {
		memset(d->sync_history, 0, sizeof(LightcapElement) * SYNC_HISTORY_LEN);
		d->sync_offset = 0;
	}
	if (new_state == LS_UNKNOWN && get_best_latest_state(g) == 0) {
		if (g->single_60hz_mode) {
			SV_INFO("Disambiguator Reseting 60hz mode flag");
		}
		g->single_60hz_mode = false;
	}

	ResetSync(d);

	memset(d->sweep_data, 0, sizeof(LightcapElement) * d->so->sensor_ct);

	return new_state;
}

static void PropagateState(Disambiguator_data_t *d, const LightcapElement *le);
static void RunACodeCapture(int target_acode, Disambiguator_data_t *d, const LightcapElement *le) {
	// Just ignore small signals; this has a measurable impact on signal quality
	if (le->length < 400)
		return;

	// We know what state we are in, so we verify that state as opposed to
	// trying to suss out the acode.

	uint32_t error = calculate_error(target_acode, le);
	SurviveContext *ctx = d->so->ctx;
	Global_Disambiguator_data_t *g = ctx->disambiguator_data;

	// Errors do happen; either reflections or some other noise. Our scheme here is to
	// keep a tally of hits and misses, and if we ever go into the negatives reset
	// the state machine to find the state again.
	if (error > 1250) {

		// Penalize semi-harshly -- if it's ever off track it will take this many syncs
		// to reset
		const int penalty = 3;
		if (d->confidence < penalty) {
			SetState(d, le, LS_UNKNOWN);
			SV_WARN("Disambiguator got lost at %u; refinding state for %s", le->timestamp, d->so->codename);
		}
		d->confidence -= penalty;

		DEBUG_TB("Disambiguator missed %s; %d expected %d but got %d(%d) - %u %d", d->so->codename, error, target_acode,
				 le->length, d->confidence, d->mod_offset, le->timestamp);
		return;
	}

	if (d->confidence < 50) {
		DEBUG_TB("Disambiguator hit %s; %d expected %d but got %d(%d) - %u %u", d->so->codename, error, target_acode,
				 le->length, d->confidence, d->mod_offset, le->timestamp);
	}

	if (d->confidence < 100) {
		d->confidence++;
	}
	// If its a real timestep, integrate it here and we can take the average later

	RegisterSync(d, le);
}

static void ProcessStateChange(Disambiguator_data_t *d, const LightcapElement *le, enum LighthouseState new_state) {
	SurviveContext *ctx = d->so->ctx;
	Global_Disambiguator_data_t *g = d->so->ctx->disambiguator_data;
	int end_of_mod = g->single_60hz_mode ? LS_WaitLHB_ACode0 : LS_END;

	// Leaving a sync ...
	if (LS_Params[d->state].is_sweep == 0) {
		if (d->last_sync_count > 0) {
			LightcapElement lastSync = {.timestamp = d->first_sync_timestamp,
										.length = d->longest_sync_length,
										.sensor_id = -d->last_sync_count};

			AddSyncHistory(d, lastSync);
			// Use the average of the captured pulse to adjust where we are modulo against.
			// This lets us handle drift in any of the timing chararacteristics
			uint32_t new_offset = SolveForMod_Offset(d, d->state, &lastSync);
			int32_t delta = (new_offset - d->mod_offset[LS_Params[d->state].lh]) % end_of_mod;
			if (abs(delta) > 100) {
				SV_WARN("Drift in timecodes %s %u", d->so->codename, delta);
			}
			d->mod_offset[LS_Params[d->state].lh] = new_offset;

			// Figure out if it looks more like it has data or doesn't. We need this for OOX
			int lengthData = ACODE_TIMING(LSParam_acode(d->state) | DATA_BIT);
			int lengthNoData = ACODE_TIMING(LSParam_acode(d->state));
			bool hasData = abs(lengthData - lastSync.length) < abs(lengthNoData - lastSync.length);
			int acode = LSParam_acode(d->state);
			if (hasData) {
				acode |= DATA_BIT;
			}

			int next_state = d->state + 1;

			Global_Disambiguator_data_t *g = ctx->disambiguator_data;
			if (next_state == LS_END || (g->single_60hz_mode && next_state == LS_WaitLHB_ACode0))
				next_state = 0;

			int index_code = LS_Params[next_state].is_sweep ? -1 : -2;
			if (d->confidence > 80)
				ctx->lightproc(d->so, index_code, acode, 0, lastSync.timestamp, lastSync.length,
							   LS_Params[d->state].lh);
		}
	} else {
		// Leaving a sweep ...
		size_t avg_length = 0;
		size_t cnt = 0;

		for (int i = 0; i < d->so->sensor_ct; i++) {
			LightcapElement le = d->sweep_data[i];
			if (le.length > 0) {
				avg_length += le.length;
				cnt++;
			}
		}
		if (cnt > 0) {
			double var = 3;
			size_t minl = 10;
			size_t maxl = var * DIV_ROUND_CLOSEST(avg_length, cnt);

			int lh = LS_Params[d->state].lh;

			for (int i = 0; i < d->so->sensor_ct; i++) {
				const LightcapElement *le = &d->sweep_data[i];
				// Only care if we actually have data AND we have a time of last sync. We won't have the latter
				// if we synced with the LH at certain times.
				if (le->length > 0 && le->length >= minl && le->length <= maxl) {
					int le_offset = apply_mod_offset(le->timestamp + le->length / 2, d->mod_offset[lh], end_of_mod);
					int32_t offset_from = le_offset - LSParam_offset_for_state(d->state) + 20000;

					assert(offset_from > 0);
					// Send the lightburst out.
					if (d->confidence > 80) {
						d->so->ctx->lightproc(d->so, i, LSParam_acode(d->state), offset_from, le->timestamp, le->length,
											  lh);
					}
				}
			}
		}
	}
	SetState(d, le, new_state);
}

static void PropagateState(Disambiguator_data_t *d, const LightcapElement *le) {
	struct SurviveContext *ctx = d->so->ctx;
	if (le->sensor_id >= d->so->sensor_ct) {
		SV_WARN("Invalid sensor %d detected hit", le->sensor_id);
		return;
	}

	Global_Disambiguator_data_t *g = ctx->disambiguator_data;
	int end_of_mod = g->single_60hz_mode ? LS_WaitLHB_ACode0 : LS_END;

	int lh = LS_Params[d->state].lh;
	int le_offset = apply_mod_offset(le->timestamp + le->length / 2, d->mod_offset[lh], end_of_mod);

	/** Find where this new element fits into our state machine. This can skip states if its been a while since
	 * its been able to process, or if a LH is missing. */
	int offset_error;
	enum LighthouseState new_state = LighthouseState_findByOffset(le_offset, &offset_error);

	if (d->state != new_state) {
		if (d->state + 1 != new_state && (d->state != (LS_END - 1) && new_state == 1)) {
			DEBUG_TB("Missed some states... %d to %d", d->state, new_state);
		}
		// This processes the change -- think setting buffers, and sending OOTX / lightproc calls
		ProcessStateChange(d, le, new_state);
	}

	const LighthouseStateParameters *param = &LS_Params[d->state];
	if (param->is_sweep == 0) {
		RunACodeCapture(LSParam_acode(d->state), d, le);
	} else if (le->length > d->sweep_data[le->sensor_id].length &&
			   le->length < 7000 /*anything above 10k seems to be bullshit?*/) {
		// Note we only select the highest length one per sweep. Also, we bundle everything up and send it later all at
		// once.
		// so that we can do this filtering. Might not be necessary?
		if (le->length > 3000) {
			d->confidence--;
		}
		assert(le->sensor_id < d->so->sensor_ct);
		d->sweep_data[le->sensor_id] = *le;
	}
}

void DisambiguatorStateBased(SurviveObject *so, const LightcapElement *le) {
	SurviveContext *ctx = so->ctx;

	// Note, this happens if we don't have config yet -- just bail
	if (so->sensor_ct == 0) {
		return;
	}

	if (so->ctx->disambiguator_data == NULL) {
		DEBUG_TB("Initializing Global Disambiguator Data");
		Global_Disambiguator_data_t *d = calloc(1, sizeof(Global_Disambiguator_data_t));
		d->ctx = ctx;
		ctx->disambiguator_data = d;
	}

	if (so->disambiguator_data == NULL) {
		DEBUG_TB("Initializing Disambiguator Data for TB %d", so->sensor_ct);
		Disambiguator_data_t *d = calloc(1, sizeof(Disambiguator_data_t) + sizeof(LightcapElement) * so->sensor_ct);
		d->so = so;
		so->disambiguator_data = d;
	}

	Disambiguator_data_t *d = so->disambiguator_data;
	// It seems like the first few hundred lightcapelements are missing a ton of data; let it stabilize.
	if (d->stabalize < 200) {
		d->stabalize++;
		return;
	}

	DEBUG_TB("%s LE: %2u\t%4u\t%8x\t%2u", so->codename, le->sensor_id, le->length, le->timestamp, d->state);

	if (d->state == LS_UNKNOWN) {
		enum LighthouseState new_state = AttemptFindState(d, le);
		if (new_state != LS_UNKNOWN) {
			d->confidence = 0;
			d->failures = 0;

			int le_offset = (le->timestamp - d->mod_offset[0]) % LSParam_offset_for_state(LS_END);
			enum LighthouseState new_state1 = LighthouseState_findByOffset(le_offset, 0);
			SetState(d, le, new_state);
			SV_INFO("Locked onto state %d(%d, %d) at %u for %s", new_state, new_state1, le_offset, d->mod_offset[0],
					d->so->codename);
		} else {
			d->failures++;
			if (d->failures > 1000) {
				d->failures = 0;
				SV_WARN("Could not find disambiguator state for %s", d->so->codename);
			}
		}
	} else {
		uint32_t timediff = survive_timecode_difference(le->timestamp, d->last_timestamp);
		if (timediff > d->so->timebase_hz) {
			int penalty = timediff / d->so->timebase_hz * 10;
			if (d->confidence < penalty) {
				SetState(d, le, LS_UNKNOWN);
				SV_WARN("Disambiguator got lost at %u (sync timeout %u); refinding state for %s", le->timestamp,
						timediff, d->so->codename);
				return;
			}

			d->confidence = d->confidence - penalty;
		}
		PropagateState(d, le);
	}

	d->last_timestamp = le->timestamp;
}

REGISTER_LINKTIME(DisambiguatorStateBased);
