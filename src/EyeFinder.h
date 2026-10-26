#pragma once
// Eye finder v2 (W18) for the eye lights (Light C). Mask-agnostic: rasterises the model's CPU geometry (the parts
// Auto Fit sizes by) from the FRONT (+Z in fitted mask space, the camera side at Rotate 0) with an orthographic
// camera into a 512 x 512 coverage + front-depth image. Eye candidates come from two tests, merged:
//   - see-through: enclosed uncovered regions (holes), flood-filled from the border;
//   - cavity: covered pixels whose front depth steps back sharply from their rim (a morphological closing of the
//     depth map fills openings narrower than the kernel; closing - depth = how far the surface sits behind its rim).
//     This catches slits and sockets closed behind, lenses set back into frames and portholes.
// The symmetry axis is estimated from the silhouette + depth (mirror mismatch search), not assumed at x = 0. The pair
// is scored on area ratio, height, mirror offset about that axis, separation and height in the model. No pair: a single
// wide opening across the axis (visor, T-slit, porthole) becomes a pair at the 1/3 and 2/3 points of its widest band
// ("visor split"). Runs on a worker thread; results are cached per geometry + part selection.
#include "ModelData.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace mehr
{
struct EyeHole
{
	float center[ 3 ] = { 0, 0, 0 }; // eye opening: centroid (x, y) at the rim's front depth (z); fitted mask space
	float radius      = 0;           // equivalent-circle radius of the opening, fitted mask units
	float depth       = 0;           // cavity: how far the back wall sits behind the rim (0 = see-through)
	int areaPx        = 0;
};

struct EyeResult
{
	bool found = false;
	std::string kind;                // "see-through", "cavity" or "visor split"
	std::string reason;              // why no pair (or a one-line summary of the pair)
	EyeHole left, right;             // left = -X
	float axisX = 0;                 // estimated symmetry axis, fitted mask x
	std::vector< std::string > log;  // every candidate and every scored pair
	double ms = 0;
};

// What to search: the parts to rasterise (empty = all) and the fit that defines mask space.
struct EyeQuery
{
	std::vector< uint8_t > include; // per CpuGeometry part: 1 = rasterise
	float fitCenter[ 3 ] = { 0, 0, 0 };
	float fitScale       = 1;
	std::string Key() const;
};

struct EyeJob
{
	std::atomic< bool > done{ false };
	EyeResult result; // valid once done
};

// Starts (or joins) the eye search. `debugPng` (optional): write the candidate mask with the pair marked.
std::shared_ptr< EyeJob > RequestEyes( const std::shared_ptr< const CpuGeometry >& geometry, const EyeQuery& query, const std::string& debugPng = "" );

// The search itself (synchronous).
EyeResult FindEyes( const CpuGeometry& geometry, const EyeQuery& query, const std::string& debugPng = "" );

// Called at bundle unload: waits for running searches.
void ShutdownEyeFinder();
}
