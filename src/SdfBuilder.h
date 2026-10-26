#pragma once
// W20: the mask as a smoke obstacle. Built once per model geometry on a worker thread, in the model's own (raw) units:
// a kSdfRes^3 grid over the padded bbox with R = signed distance to the solid (negative inside) and G = cavity (1 in
// the hollow interior of a lantern / helmet: space the object encloses once holes narrower than ~6 voxels are closed).
// Solid = the voxels the triangles pass through + every voxel the outside cannot reach (closed shells are filled; an
// open mask stays a shell). Uploaded as a 3D texture; the sim samples it through the inverse of the mask's transform.
#include "ModelData.h"
#include <atomic>
#include <memory>
#include <vector>

namespace mehr
{
static constexpr int kSdfRes = 64;

struct SdfField
{
	float boxMin[ 3 ] = { 0, 0, 0 };
	float boxSize     = 1;           // cube edge, raw model units
	std::vector< float > rg;         // kSdfRes^3 x 2 (x fastest)
	int solidVoxels = 0, cavityVoxels = 0;
	double ms = 0;
};

struct SdfJob
{
	std::atomic< bool > done{ false };
	SdfField field;
};

std::shared_ptr< SdfJob > RequestSdf( const std::shared_ptr< const CpuGeometry >& geometry );
SdfField BuildSdf( const CpuGeometry& geometry );
void ShutdownSdfBuilder();
// W26: CPU queries on a built field, raw model units. SdfAt: trilinear signed distance (outside the box: + the distance
// to the box). SdfHit: the first surface along o + t d (d unit length) in [t0, t1]; a start inside the solid marches out
// first (an eye opening's rim); returns t1 when nothing is hit.
// W26 liquid: the field of the model with its hollows and openings up to ~2 r voxels closed (dilate r, flood from outside,
// erode r): a shell helmet becomes a solid head, so a morph between two models never passes through empty space
void FillSdf( SdfField& f, int r );
float SdfAt( const SdfField& f, const float* p );
float SdfHit( const SdfField& f, const float* o, const float* d, float t0, float t1 );
}
