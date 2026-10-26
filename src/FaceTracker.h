#pragma once
// Face tracking for the FX build (Stage 7): Apple Vision face landmarks on a background queue.
// The render thread only hands over a downscaled RGBA frame (rows top-first) when the tracker is idle and
// picks up the latest result; it never waits for Vision.
#include <cstdint>
#include <memory>
#include <vector>

namespace mehr
{
struct FaceResult
{
	// normalised image coordinates, origin bottom-left (= GL texture convention), y up
	float cx = 0, cy = 0, w = 0, h = 0;
	float roll = 0, yaw = 0, pitch = 0; // radians
};

struct FaceFrame
{
	std::vector< FaceResult > faces;
	uint64_t seq = 0;
	double ms    = 0; // Vision time for this frame
};

class FaceTracker
{
public:
	FaceTracker();
	~FaceTracker();
	bool Busy() const;
	// takes ownership of the pixels; returns immediately
	void Submit( std::vector< uint8_t >&& rgbaTopFirst, int w, int h );
	// copies the newest result; true when it is newer than the last one taken
	bool TakeLatest( FaceFrame& out );

private:
	struct Impl;
	std::shared_ptr< Impl > impl; // shared with the queue's blocks, so a late result never touches a dead tracker
};
}
