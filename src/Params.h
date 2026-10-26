#pragma once
// Parameter ids, generated from ParamList.inc (the single source of truth: order, names, ranges, groups).
// Keep the CLAUDE.md index map in sync with that file.
#include "ModelData.h"

#ifndef LOADEHR_FX
#define LOADEHR_FX 0
#endif
#ifndef LOADEHR_WOW
#define LOADEHR_WOW 0 // W26: 1 = the "LoadEhr Wow" bundles (see BUNDLES in ParamList.inc)
#endif

namespace mehr
{
enum ParamId : unsigned
{
#define X( id, ... ) id,
#include "ParamList.inc"
#undef X
	P_COUNT
};

static constexpr unsigned P_PARTS_FIRST = P_PART1_VISIBLE;
static constexpr unsigned kPartFields   = P_PART2_VISIBLE - P_PART1_VISIBLE;

enum PartField : unsigned
{
	PF_VISIBLE = 0,
	PF_HUE,
	PF_GLOW,
	PF_SKIN,
	PF_SURFACE
};
static_assert( P_PART1_SKIN - P_PART1_VISIBLE == PF_SKIN, "part fields drifted" );
static_assert( P_PART1_SURFACE - P_PART1_VISIBLE == PF_SURFACE && kPartFields == 5, "part fields drifted" );
inline unsigned PartParam( unsigned slot, PartField f ) { return P_PARTS_FIRST + slot * kPartFields + f; }

static constexpr unsigned kFFTSize = 512;

// LoadEhr W1: the four model slots' params, three per slot in a row (Model X, Status X, Reload X)
static constexpr int kModelSlots = 4;
enum SlotField : unsigned
{
	SF_FILE = 0,
	SF_STATUS,
	SF_RELOAD
};
static_assert( P_STATUS_A - P_MODEL_A == SF_STATUS && P_RELOAD_A - P_MODEL_A == SF_RELOAD && P_MODEL_B - P_MODEL_A == 3 && P_RELOAD_D - P_MODEL_A == 11,
               "slot params drifted" );
inline unsigned SlotParam( int slot, SlotField f ) { return P_MODEL_A + 3u * unsigned( slot ) + f; }
// the slot a param id belongs to as field f, or -1
inline int SlotOfParam( unsigned id, SlotField f ) { return id >= P_MODEL_A && id <= P_RELOAD_D && ( id - P_MODEL_A ) % 3 == f ? int( ( id - P_MODEL_A ) / 3 ) : -1; }
}
