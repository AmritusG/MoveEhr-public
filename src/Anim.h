#pragma once
// MoveEhr W1: model animation playback (glTF skins / morph targets / node keyframes; FBX through a load-time ufbx bake).
// ONE deformation path for all three kinds: every animated vertex is a skinned vertex.
//   * a skinned vertex: up to 8 joints (W2; larger sets merged to the top 8, renormalized);
//   * a vertex of a mesh on an animated node without a skin: one joint on that node, weight 1;
//   * a vertex of a static mesh in an animated file: joint 0 (the root: the orient.txt rotation only), weight 1;
//   * morph targets: source-space deltas added before skinning (glTF order), weights per mesh node.
// Spaces: p_src = the vertex in its joint's input space (glTF mesh space / FBX geometry space; for joint 0 the baked position before
// orient.txt). Palette J_j = O x world_j(pose) x bind_j (O = the orient.txt rotation, bind = inverse bind / geometry-to-node).
// The static ModelData vertex (the BIND geometry) = sum w J_j(rest) (p_src + rest morph): a file is drawn exactly as the static
// geometry while Anim Play is Off (no pose pass at all). Normals / tangents of the posed vertex = sum w D_j x the static normal /
// tangent, D_j = J_j(pose) x J_j(rest)^-1 (3x3), so files without tangents keep the generated ones.
// A file with no animation, no skin and no morph target goes down LoadEhr's path untouched (ModelData::anim stays null).
#include <cstdint>
#include <string>
#include <vector>

namespace mehr
{
// per FINAL vertex (parallel to ModelData::vertices), uploaded as the pose pass's second vertex buffer (locations 4-9)
constexpr int kMaxInfluences = 8; // MoveEhr W2: two joints / weights pairs (the CC0 characters use up to 8; top 4 dropped up to 12 %)
struct AnimVertex
{
	float p[ 3 ];                       // p_src
	uint16_t j[ kMaxInfluences ];       // palette joints (j[ 0..3 ] = location 5, j[ 4..7 ] = location 8)
	float w[ kMaxInfluences ];          // weights, sum 1 (w[ 0..3 ] = location 6, w[ 4..7 ] = location 9)
	uint32_t morph[ 4 ];                // delta base (texel-pair index of target 0), stride (vertices of that morph mesh), target count, weight base
};
static_assert( sizeof( AnimVertex ) == 76, "the pose pass reads 19 words per vertex" );

enum AnimPath : int
{
	AP_T = 0,
	AP_R,
	AP_S,
	AP_W
};
enum AnimInterp : int
{
	AI_LINEAR = 0,
	AI_STEP,
	AI_CUBIC
};

struct AnimNode
{
	int parent = -1;
	double t[ 3 ] = { 0, 0, 0 }, r[ 4 ] = { 0, 0, 0, 1 }, s[ 3 ] = { 1, 1, 1 }; // rest local TRS (r = quaternion xyzw)
	bool hasMatrix = false;                                                      // glTF matrix node (never animated)
	double m[ 16 ] = {};                                                         // column-major local matrix when hasMatrix
	int weightBase = -1, weightCount = 0;                                        // morph weights this node drives
	std::string name;
};

struct AnimJoint
{
	int node = -1;          // -1 = the root joint (world = identity)
	double bind[ 16 ] = {}; // column-major
};

struct AnimChannel
{
	int node   = -1;
	int path   = AP_T;
	int interp = AI_LINEAR;
	int comps  = 3;               // values per key (cubic: 3 x comps per key: in, value, out)
	int weightIndex = -1;         // AP_W with one value (FBX blend channel): writes this weight; -1 = the node's weights (glTF)
	std::vector< float > times;   // seconds, file time
	std::vector< float > values;
};

struct AnimClip
{
	std::string name;
	double start = 0, duration = 0; // file time of the first key, length in seconds (0 = a single pose)
	std::vector< AnimChannel > channels;
	int rootNode = -1;              // Root Motion In Place pins this node's X / Z translation to its clip-start value
	int moving   = 0;               // W2 A2: channels whose keys move (> 1e-5 from their first key); set by the loader
	bool listed  = true;            // W2 A2: false = a near-empty duplicate take (MOVEEHR-004): kept (stable values), not listed
};

struct AnimData
{
	std::vector< AnimVertex > verts;   // one per ModelData vertex
	std::vector< AnimNode > nodes;     // parents before children (index order)
	std::vector< AnimJoint > joints;   // joint 0 = the root
	std::vector< AnimClip > clips;
	std::vector< float > morphDeltas;  // 8 floats per (target, vertex): position delta xyz 0, normal delta xyz 0
	std::vector< float > restWeights;  // all morph weights at rest
	double orient[ 9 ] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; // orient.txt rotation (row-major 3x3)
	std::string kind;                  // "skin + morph + nodes" etc. (Status / log)
	int maxInfluences = 0;             // largest influence set seen in the file
	double droppedWeight = 0;          // largest weight share dropped by the top-8 merge (log)
	std::string movements;             // MoveEhr W8 B: what Movement Source + Same Folder added / skipped (Status)
};

// A pose: the local TRS of every node + every morph weight.
struct AnimPose
{
	std::vector< double > trs; // 10 per node: t xyz, r xyzw, s xyz
	std::vector< float > weights;
};

void RestPose( const AnimData& a, AnimPose& out );
// W2 A2: takes with the same name + length (Blender exports each action once per object, MOVEEHR-004) are compared by their baked
// motion (posed vertices at 17 times, fitted units): the same motion twice (< 0.005 apart) unlists the later copy; a copy with < 10 %
// of the other's motion is
// unlisted. Unlisted takes stay in `clips` (the Anim Clip value = the file's take index + 1, never renumbered).
void MarkDuplicateTakes( AnimData& a, double fitScale );
// Sample a clip at `t` seconds from the clip's start (clamped to [0, duration]) on top of the rest pose.
// rootInPlace = Root Motion In Place.
void SampleClip( const AnimData& a, int clip, double t, bool rootInPlace, AnimPose& out );
// out = mix( a, b, f ): lerp translation / scale / weights, slerp rotation the short way
void BlendPose( const AnimPose& a, const AnimPose& b, double f, AnimPose& out );
// Joint matrices of a pose: 16 doubles per joint (column-major J = O x world x bind)
void JointMatrices( const AnimData& a, const AnimPose& pose, std::vector< double >& J );
// Palette for the pose pass: 24 floats per joint = J (3 rows x 4) + D (3 rows x 4, xyz used): D = J x J_rest^-1 (3x3).
void PosePalette( const AnimData& a, const std::vector< double >& J, const std::vector< double >& Jrest, std::vector< float >& palette );
// CPU reference skin of vertex v (position, baked + oriented space) from joint matrices and weights, in double precision.
void CpuSkinVertex( const AnimData& a, const std::vector< double >& J, const std::vector< float >& weights, size_t v, double out[ 3 ] );
}
