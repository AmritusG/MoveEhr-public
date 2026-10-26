#pragma once
// W2 Stage 5: the projector plays video FILES. One decoder per file on its own worker thread, frames queued ahead of the play
// position; the render thread only picks the best decoded frame (never waits: a late frame shows the last one).
//   H.264 / HEVC / ProRes (incl. 4444 alpha): AVAssetReader -> BGRA IOSurface-backed CVPixelBuffers, bound to GL zero-copy
//     (CGLTexImageIOSurface2D, GL_TEXTURE_RECTANGLE) by the renderer.
//   HAP / HAP Alpha / HAP Q / HAP Q Alpha / HAP Alpha-Only: the raw compressed samples (outputSettings nil), the HAP sections parsed
//     and Snappy-decompressed here (our own decoder, written from the HAP and Snappy format descriptions), uploaded as DXT / BC
//     textures: no CPU pixel decode.
//   DXV (W3): DXVKit (third_party/dxvkit, our clean-room MIT decoder) opens every file first; a DXV track (DXV3 DXT1 / DXT5 / YCG6 /
//     YG10, legacy DXDI) is read and decoded to its BCn block planes here (random access, no AVFoundation) and uploaded like HAP.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mehr
{
struct VideoFrame
{
	double pts      = 0;   // seconds from the clip start
	int index       = 0;   // frame number
	int w = 0, h = 0;      // picture size
	void* pixelBuffer = nullptr; // CVPixelBufferRef (BGRA, IOSurface-backed), retained; null for HAP / DXV
	// HAP / DXV: block-compressed planes, rows top-first. format: 1 = DXT1 RGB, 5 = DXT5 RGBA, 6 = scaled YCoCg DXT5 (Hap Q),
	// 4 = BC4 alone, 7 = DXV YCG6 (BC4 Y + BC5 Co, Cg at half size), 8 = DXV YG10 (BC5 Y, A + BC5 Co, Cg at half size)
	int format = 0;
	int tw = 0, th = 0; // plane 0's texture size: the picture padded to whole blocks (HAP: to 4 px, DXV: to 16 px)
	std::vector< uint8_t > blocks, blocks2; // blocks2: Hap Q Alpha's BC4 alpha, or DXV HQ's Co, Cg plane
	uint64_t serial = 0;
	size_t Bytes() const;
	~VideoFrame();
};

class VideoPlayer
{
public:
	explicit VideoPlayer( const std::string& path );
	~VideoPlayer();
	bool Ready() const { return ready.load(); }
	bool Failed() const { return failed.load(); }
	std::string Info() const; // codec, size, fps, duration (or the error)
	double Duration() const { return duration; }
	double FrameDuration() const { return frameDur; }
	int Frames() const { return frames; }
	std::string Codec() const; // short: "DXV YCG6", "avc1", "Hap1", ...
	// W8: DXV frames that did not decode (skipped: the last frame stays) and the first one's error; 0 = none
	std::string BadInfo( uint64_t* n ) const;
	// render thread: the frame to show at `pos` seconds while playing forward (or backward); never blocks. `pos` is the play
	// position already wrapped / mirrored into 0..Duration by the caller; `loop` = the frames after the end are the first ones.
	// W4 hold 1 (Scrub) / 2 (a cue waiting): the frame at `pos` exactly; until it is decoded the last frame shown stays (Scrub: from
	// the 3rd render waiting, the decoded frame nearest to it on the way). The direction = the way it moved.
	std::shared_ptr< const VideoFrame > Frame( double pos, bool forward, bool loop, int hold = 0 );
	// W6 (Amrit: Scrub "Limited."): every frame is a key frame and decodes on its own (DXV, HAP / HAP Alpha / HAP Q / HAP Q Alpha /
	// HAP Alpha-Only): Video Loop Scrub works only on these; any other codec plays Scrub as Loop
	bool Intra() const { return intra.load(); }
	// stats (Debug Log): frames decoded, decode ms (worker), frames shown late
	uint64_t decoded = 0, late = 0;
	double decodeMs = 0;
	size_t cacheBytes = 0;
	// W4 scrub latency (render thread): per scrub target reached, ms from the request to the frame returned, and the renders that
	// showed another frame meanwhile; `superseded` = targets replaced by a newer one before they arrived. TakeScrub() empties them.
	struct ScrubStat
	{
		int frame;
		double ms;
		int held;
	};
	std::vector< ScrubStat > TakeScrub( uint64_t* superseded );

private:
	void Run();
	bool RunDxv();                          // false: not a DXV file (AVFoundation reads it)
	int Window( std::vector< int >& need ); // mtx held: the frames wanted around the request; the first missing one (-1 = none)
	std::string path, info, codec;
	std::atomic< bool > ready{ false }, failed{ false }, stop{ false }, intra{ false };
	double duration = 0, frameDur = 1.0 / 30.0;
	int frames = 0;
	int behind = 6;         // frames kept in the play direction when it runs backward (AVFoundation: more, by a memory budget)
	std::vector< int > keys; // AVFoundation: the sync (key) frames, ascending; empty = every frame is one (DXV, HAP, ProRes)
	std::thread worker;
	mutable std::mutex mtx;
	std::condition_variable cv;
	// the request (render thread -> worker) and the decoded frames (worker -> render thread)
	double wantPos = 0;
	bool wantForward = true, wantLoop = true, wantScrub = false;
	uint64_t wantSerial = 0;
	// W4 scrub latency
	int scrubTarget = -1, scrubHeld = 0;
	std::chrono::steady_clock::time_point scrubT0;
	std::vector< ScrubStat > scrubStats;
	uint64_t scrubSuperseded = 0;
	std::vector< char > bad; // W8 DXV: frames that did not decode (never fetched again)
	uint64_t badFrames = 0;
	std::string badInfo;
	std::map< int, std::shared_ptr< const VideoFrame > > cache;
	std::shared_ptr< const VideoFrame > last;
};

// Snappy decompression (format_description.txt): false on malformed input
bool SnappyDecompress( const uint8_t* in, size_t n, std::vector< uint8_t >& out );
// one HAP frame (a sample's bytes) -> its texture planes; false + error on anything unsupported
bool DecodeHapFrame( const uint8_t* data, size_t n, int w, int h, VideoFrame& out, std::string& error );
}
