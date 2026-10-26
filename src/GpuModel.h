#pragma once
// GPU copy of a ModelData. Buffers and textures are shareable between plugin instances in the same
// GL share group; VAOs are not (each Renderer builds its own). Upload is incremental so a model
// switch never stalls a frame: geometry first, then at most N textures per frame.
#include "ModelData.h"
#include "GlCount.h" // MoveEhr W6: <OpenGL/gl3.h> + the GL call counters
#include <map>
#include <memory>
#include <string>

namespace mehr
{
struct GpuPart
{
	std::string name;
	uint32_t firstIndex = 0, indexCount = 0;
	GLuint tex[ CH_COUNT ]    = {};
	int component[ CH_COUNT ] = { -1, -1, -1, -1, -1, -1, -1 };
	bool flipGreen            = false;
	float baseColor[ 4 ]      = { 1, 1, 1, 1 };
	float emissive[ 3 ]       = { 0, 0, 0 };
	float roughness = 1, metallic = 0, normalScale = 1;
	bool alphaCutout = false;
	float glass = 0, glassTint[ 3 ] = { 1, 1, 1 };
	int slot = 0;
	float boxMin[ 3 ] = { 0, 0, 0 }, boxMax[ 3 ] = { 0, 0, 0 };
	bool fractured = false; // W2: the fractured copy (text / logo pieces): drawn instead of the whole mesh while pieces fly
};

// Simplified index buffers (meshoptimizer) for clones / ghosts: level 0 is the model's own ibo.
struct LodLevel
{
	GLuint ibo = 0, tbo = 0; // tbo: R32UI view for the triangle-soup stage
	std::vector< std::array< uint32_t, 2 > > parts; // firstIndex, indexCount per part
	size_t triangles = 0;
};
struct LodJob;
static constexpr int kLodLevels = 4;

class GpuModel
{
public:
	explicit GpuModel( std::shared_ptr< const ModelData > data );
	~GpuModel(); // must run with the owning GL context current

	// Advances the upload. Returns true once everything is on the GPU.
	bool UploadStep( int textureBudget );
	bool Complete() const { return complete; }

	GLuint vbo = 0, ibo = 0;
	GLuint vtxTBO = 0, idxTBO = 0; // R32UI texture-buffer views for the triangle-soup path
	std::vector< GpuPart > parts;
	std::vector< std::string > slotNames;
	float fitCenter[ 3 ] = { 0, 0, 0 };
	float fitScale       = 1;
	size_t triangleCount = 0;
	bool hasPieces       = false; // W2: carries a fractured copy (parts flagged `fractured`)
	std::string path;
	std::string status; // W21: generated models: the Status line
	double msUpload = 0, msWorstStep = 0;
	size_t texBytes = 0, geometryBytes = 0; // W17: reported in the UPLOAD line
	int texLargest = 0;
	uint64_t serial = 0; // unique per GpuModel, so VAOs never outlive the buffers they point at
	std::shared_ptr< const CpuGeometry > geometry;
	// MoveEhr W1: an animated file's pose-pass inputs (shared by every slot / instance showing it; the pose itself is per slot,
	// in the Renderer): AnimVertex per vertex (locations 4-7), morph deltas (RGBA32F, 2 texels per target and vertex)
	std::shared_ptr< const AnimData > anim;
	GLuint animVbo = 0, morphBuf = 0, morphTBO = 0;
	// LODs: built off the render thread after load, uploaded by whichever instance polls first
	void UpdateLods();
	int LodsReady() const { return lodsReady ? kLodLevels : 1; }
	LodLevel lods[ kLodLevels ];

private:
	std::shared_ptr< const ModelData > data; // released after upload
	bool complete      = false;
	bool geometryDone  = false;
	size_t nextTexture = 0;
	// W18: the texture in flight is filled in row chunks across frames (an 8K level 0 alone is 256 MB)
	bool texOpen = false;
	int texLevel = 0, texRow = 0;
	GLuint touchFBO[ 2 ] = { 0, 0 }, touchRB = 0; // W25: see Touch()
	void Touch( GLuint tex );
	struct TexJob
	{
		const Image* image;
		bool srgb;
		GLuint handle = 0;
	};
	std::vector< TexJob > texJobs;
	std::vector< std::array< int, CH_COUNT > > partJob; // per part/channel: index into texJobs or -1
	std::shared_ptr< LodJob > lodJob;
	bool lodsReady = false;
};

// Process-wide cache of GPU models keyed by file version + GL share group.
std::shared_ptr< GpuModel > FindGpuModel( const std::string& key );
void RegisterGpuModel( const std::string& key, const std::shared_ptr< GpuModel >& model );
void ForgetGpuModelsForPath( const std::string& path );
std::string ShareGroupKey();
}
