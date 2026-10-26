#pragma once
// CPU-side model produced by the loader worker and consumed (uploaded) by the render thread.
#include "Anim.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mehr
{
static constexpr int kMaxParts = 8;

enum Channel : int
{
	CH_BASECOLOR = 0,
	CH_NORMAL,
	CH_ROUGHNESS,
	CH_METALLIC,
	CH_HEIGHT,
	CH_EMISSIVE,
	CH_AO,
	CH_COUNT
};
const char* ChannelName( int channel );

// Decoded image, rows top-first (the renderer samples with the "t=0 is the image top" convention).
// The full mip chain is built on the loader thread so the render thread only copies bytes.
struct Image
{
	int width = 0, height = 0, components = 0; // components: 1 or 4 (8-bit)
	bool srgb = false;                         // colour data (base colour / emissive)
	std::vector< uint8_t > pixels;             // level 0
	std::vector< std::vector< uint8_t > > mips; // levels 1..n
	bool Valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

// One texture binding for a channel. `swizzle` selects which source component feeds the
// channel (packed ORM / glTF metallicRoughness maps), -1 = use the image as-is.
struct TextureRef
{
	std::shared_ptr< Image > image; // shared: a packed map can feed several channels
	int component    = -1;          // for scalar channels: 0=R 1=G 2=B 3=A
	bool flipGreen   = false;       // DirectX-style normal map
	std::string source;             // log / debug: where it came from
};

struct Part
{
	std::string name;
	uint32_t firstIndex = 0;
	uint32_t indexCount = 0;
	std::array< TextureRef, CH_COUNT > tex;
	float baseColor[ 4 ] = { 0.8f, 0.8f, 0.8f, 1.0f };
	float emissive[ 3 ]  = { 0, 0, 0 };
	float roughness      = 0.5f;
	float metallic       = 0.0f;
	float normalScale    = 1.0f;
	bool alphaCutout     = false;
	float glass          = 0.0f;            // W18: transmission / see-through BLEND -> the Glass surface at this amount
	float glassTint[ 3 ] = { 1, 1, 1 };     // linear: base colour factor x the base colour map's average
	int slot             = 0;               // control slot ("Part N" params), 0..kMaxParts-1
	bool fractured       = false;           // W2: a part of the fractured copy (text / logo pieces): drawn only while pieces fly
	float boxMin[ 3 ]    = { 0, 0, 0 };     // bbox of the part's triangles, model units (after orient.txt)
	float boxMax[ 3 ]    = { 0, 0, 0 };
};

// Packed vertex, 28 bytes: position, uv, normal + tangent as GL_INT_2_10_10_10_REV.
struct Vertex
{
	float px, py, pz;
	float u, v;
	uint32_t normal;  // xyz snorm10, w unused
	uint32_t tangent; // xyz snorm10, w = handedness (+1/-1 in the 2-bit field)
};
static_assert( sizeof( Vertex ) == 28, "the triangle-soup shader reads 7 words per vertex" );

// Positions / uvs / indices kept on the CPU after upload (shard clustering, dust sampling, LODs).
// Built on the loader thread, shared by every instance through the GPU model.
struct CpuGeometry
{
	std::vector< float > pos; // 3 per vertex, raw model units
	std::vector< float > uv;  // 2 per vertex
	std::vector< uint32_t > idx;
	std::vector< std::array< uint32_t, 2 > > parts; // firstIndex, indexCount
	std::vector< int > partSlot;                    // control slot per part
	std::vector< uint16_t > triGroup;               // W21: letter / logo shape per triangle (empty for files)
	int groupCount = 0;
	std::vector< float > groupBox;                  // W21: per group min xyz, max xyz (fitted space: the letter's pivot)
	// W2: the fractured copy (text / logo pieces). idx / parts above are the WHOLE mesh only (dust, SDFs, LODs, surface points);
	// the copy's triangles follow it in the GPU buffers: fracIdx (absolute vertex ids), its piece per triangle. triGroup covers
	// both (whole first), so the letters move the pieces too.
	std::vector< uint32_t > fracIdx;
	std::vector< uint16_t > fracPiece;
	int pieceCount = 0;
	float fitCenter[ 3 ] = { 0, 0, 0 };
	float fitScale       = 1;
	// W7: the dissolve noise (Fbm( fitted x 3 + 7 ), the main FS) at quantiles 0, 1/128 .. 1 over the whole mesh's area (the tails are skewed: 16 steps were too coarse there): a transition's
	// swap cuts at the quantile of its progress, so the share of the surface swapped follows the progress (the noise clusters near 0.5)
	float fitBox[ 6 ] = { -1, -1, -1, 1, 1, 1 }; // W7: the whole mesh's bbox in fitted space (min xyz, max xyz)
	float swapQ[ 129 ] = {}; // filled by the load worker (all 0 = no geometry: Cut() then runs evenly)
};

struct ModelData
{
	std::string path;
	std::vector< Vertex > vertices;
	std::vector< uint32_t > indices; // sorted by part
	std::vector< Part > parts;       // draw parts, one per (node, material) group; several can share a control slot
	std::vector< std::string > slotNames; // at most kMaxParts: the names shown on the "Part N" params
	std::vector< uint16_t > triGroup;     // W21: generated models only (see CpuGeometry)
	int groupCount = 0;
	size_t wholeIndexCount = 0;           // W2: > 0 = indices [0, wholeIndexCount) are the whole mesh, the fractured copy follows
	std::vector< uint16_t > triPiece;     // W2: the piece of each fractured triangle (buffer order)
	int pieceCount = 0;
	std::string status;                   // W21: generated models: the Status line
	float bboxMin[ 3 ] = { 0, 0, 0 };
	float bboxMax[ 3 ] = { 0, 0, 0 };
	// Auto-fit: centre on the bbox, largest dimension -> 2.0 units.
	float fitCenter[ 3 ] = { 0, 0, 0 };
	float fitScale       = 1.0f;
	size_t triangleCount = 0;
	std::shared_ptr< const CpuGeometry > geometry;
	std::shared_ptr< const AnimData > anim; // MoveEhr W1: null = a static file (LoadEhr's path, untouched)

	double msParse = 0, msGeometry = 0, msDecode = 0;
};

uint32_t PackSnorm1010102( float x, float y, float z, float w );
}
