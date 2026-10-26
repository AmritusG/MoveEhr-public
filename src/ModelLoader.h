#pragma once
#include "ModelData.h"
#include <atomic>
#include <string>

namespace mehr
{
// Loads .fbx / .obj (ufbx) or .gltf / .glb (cgltf) into a ModelData. Runs on a worker thread.
// Returns nullptr on failure (the reason is logged and written to `error`).
// `cancel` is polled between stages; a cancelled load returns nullptr.
// `maxTexture` > 0 downsamples every texture whose larger side exceeds it (by whole mip levels).
// MoveEhr W8 B: movements = Movement Source + Same Folder (the model's own folder's movement files join its clips; ModelLoader.cpp)
std::unique_ptr< ModelData > LoadModel( const std::string& path, int maxTexture, const std::atomic< bool >& cancel, std::string& error, bool movements = false );

bool IsSupportedModelFile( const std::string& path );
// MoveEhr W5 (S1) Test Model (TestModel.cpp): "test:cube" / "test:tube" = a binary glTF generated in memory, loaded like a file
bool IsTestModel( const std::string& path );
std::vector< uint8_t > TestModelGlb( const std::string& path );

// W21: a generated mesh (text / logo extrusion) through the same finishing steps as a loaded file (dedupe, tangents,
// per-part ranges and boxes, auto-fit). triPart indexes `parts` (at most kMaxParts, slot = index); triGroup = the
// letter / shape each triangle belongs to (the shards and the per-letter animation use it).
struct GenCorner
{
	float p[ 3 ], n[ 3 ], uv[ 2 ];
};
struct GenMesh
{
	std::vector< GenCorner > corners; // 3 per triangle
	std::vector< uint32_t > triPart, triGroup;
	std::vector< Part > parts;
	int groupCount = 0;
};
// W2: `frac` = the fractured copy (text / logo pieces): drawn instead of the whole mesh while pieces fly. Its triangles follow the
// whole mesh's in the buffers (parts flagged `fractured`, control slots shared); the fit, CpuGeometry::idx and parts stay the whole
// mesh's. triPiece / pieceCount: the piece of each fractured triangle (in `frac` order).
std::unique_ptr< ModelData > ModelFromMesh( GenMesh& mesh, const std::string& path, GenMesh* frac = nullptr, const std::vector< uint32_t >* triPiece = nullptr,
                                            int pieceCount = 0 );
void BuildImageMips( Image& img ); // the loader's CPU mip chain (sRGB-aware)
}
