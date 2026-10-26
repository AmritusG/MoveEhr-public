// MoveEhr W1 step 2: the loader probe (not installed; built by CMake as moveehr_animprobe).
//   hash <file>                         ModelData fingerprint (vertices, indices, parts, bbox, fit, slot names) + the anim summary
//                                       (W8: MOVEEHR_PROBE_MOVES=1 = Movement Source + Same Folder)
//   dump <file> <clip|-1> <t> <out.bin> CPU-posed positions of every final vertex (clip -1 = the rest pose), header: count, fitScale
//   fbxref <file> <clip> <t> <out.bin>  ufbx's own evaluation (ufbx_evaluate_scene + evaluate_skinning + blend shapes) of every logical
//                                       vertex of the drawn meshes: an independent reference for the FBX bake + hierarchy + skin path
// Built against LoadEhr's core too (-DNO_ANIM), so `hash` compares the two loaders byte for byte.
#include "ModelLoader.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifndef NO_ANIM
#include "Anim.h"
#include "ufbx.h"
#endif

using namespace mehr;

static uint64_t Fnv( const void* d, size_t n, uint64_t h = 1469598103934665603ull )
{
	const uint8_t* p = (const uint8_t*)d;
	for( size_t i = 0; i < n; ++i )
		h = ( h ^ p[ i ] ) * 1099511628211ull;
	return h;
}

static int Hash( const char* path )
{
	std::atomic< bool > cancel{ false };
	std::string err;
#ifndef NO_ANIM
	auto m = LoadModel( path, 0, cancel, err, getenv( "MOVEEHR_PROBE_MOVES" ) != nullptr ); // W8 B: Movement Source + Same Folder
#else
	auto m = LoadModel( path, 0, cancel, err );
#endif
	if( !m )
	{
		printf( "LOAD FAIL %s: %s\n", path, err.c_str() );
		return 1;
	}
	uint64_t h = Fnv( m->vertices.data(), m->vertices.size() * sizeof( Vertex ) );
	h          = Fnv( m->indices.data(), m->indices.size() * 4, h );
	for( const Part& p : m->parts )
	{
		h = Fnv( &p.firstIndex, 4, h ), h = Fnv( &p.indexCount, 4, h ), h = Fnv( &p.slot, 4, h ), h = Fnv( p.name.data(), p.name.size(), h );
		h = Fnv( p.boxMin, 12, h ), h = Fnv( p.boxMax, 12, h ), h = Fnv( p.baseColor, 16, h );
	}
	for( const std::string& s : m->slotNames )
		h = Fnv( s.data(), s.size(), h );
	h = Fnv( m->bboxMin, 12, h ), h = Fnv( m->bboxMax, 12, h ), h = Fnv( m->fitCenter, 12, h ), h = Fnv( &m->fitScale, 4, h );
	printf( "hash %016llx verts %zu tris %zu parts %zu fit %.6g", (unsigned long long)h, m->vertices.size(), m->triangleCount, m->parts.size(), m->fitScale );
#ifndef NO_ANIM
	if( m->anim )
	{
		const AnimData& a = *m->anim;
		printf( " | anim '%s' nodes %zu joints %zu weights %zu clips %zu maxinf %d dropped %.4f", a.kind.c_str(), a.nodes.size(), a.joints.size(),
		        a.restWeights.size(), a.clips.size(), a.maxInfluences, a.droppedWeight );
		for( const AnimClip& c : a.clips )
			printf( " [%s %.3f s, %zu ch, %d moving%s]", c.name.c_str(), c.duration, c.channels.size(), c.moving, c.listed ? "" : ", unlisted" );
	}
	else
		printf( " | static" );
#endif
	printf( "  %s\n", path );
	return 0;
}

#ifndef NO_ANIM
static int Dump( const char* path, int clip, double t, const char* out )
{
	std::atomic< bool > cancel{ false };
	std::string err;
	auto m = LoadModel( path, 0, cancel, err, getenv( "MOVEEHR_PROBE_MOVES" ) != nullptr ); // W8 B: + the folder's movements
	if( !m || !m->anim )
	{
		printf( "no animated model: %s\n", err.c_str() );
		return 1;
	}
	const AnimData& a = *m->anim;
	AnimPose pose;
	if( clip < 0 )
		RestPose( a, pose );
	else
		SampleClip( a, clip, t, false, pose );
	std::vector< double > J;
	JointMatrices( a, pose, J );
	FILE* f = fopen( out, "wb" );
	const uint32_t n = (uint32_t)a.verts.size();
	fwrite( &n, 4, 1, f );
	fwrite( &m->fitScale, 4, 1, f );
	double worstBind = 0; // pose vs the static (bind) vertex: ~0 for the rest pose
	for( uint32_t v = 0; v < n; ++v )
	{
		double p[ 3 ];
		CpuSkinVertex( a, J, pose.weights, v, p );
		const float fp[ 3 ] = { (float)p[ 0 ], (float)p[ 1 ], (float)p[ 2 ] };
		fwrite( fp, 4, 3, f );
		const Vertex& sv = m->vertices[ v ];
		worstBind        = std::max( worstBind, std::max( std::fabs( p[ 0 ] - sv.px ), std::max( std::fabs( p[ 1 ] - sv.py ), std::fabs( p[ 2 ] - sv.pz ) ) ) );
	}
	fclose( f );
	printf( "dump %u verts clip %d t %.4f -> %s | vs the bind vertices: max |diff| %.3g fitted\n", n, clip, t, out, worstBind * m->fitScale );
	return 0;
}

static int FbxRef( const char* path, int clip, double t, const char* out )
{
	ufbx_load_opts opts           = {};
	opts.target_axes              = ufbx_axes_right_handed_y_up;
	opts.target_unit_meters       = 1.0f;
	opts.space_conversion         = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
	opts.generate_missing_normals = true;
	ufbx_error err;
	ufbx_scene* scene = ufbx_load_file( path, &opts, &err );
	if( !scene || clip < 0 || clip >= (int)scene->anim_stacks.count )
	{
		printf( "fbxref: cannot load / no clip %d\n", clip );
		return 1;
	}
	const ufbx_anim_stack* st = scene->anim_stacks.data[ clip ];
	ufbx_evaluate_opts eo     = {};
	eo.evaluate_skinning      = true;
	ufbx_scene* ev            = ufbx_evaluate_scene( scene, st->anim, st->time_begin + t, &eo, &err );
	if( !ev )
	{
		printf( "fbxref: evaluate failed\n" );
		return 1;
	}
	std::vector< float > pts;
	for( size_t ni = 0; ni < ev->nodes.count; ++ni )
	{
		const ufbx_node* node = ev->nodes.data[ ni ];
		const ufbx_mesh* mesh = node->mesh;
		if( !mesh || !node->visible )
			continue;
		for( size_t vi = 0; vi < mesh->num_vertices; ++vi )
		{
			ufbx_vec3 p = mesh->skinned_position.exists ? ufbx_get_vertex_vec3( &mesh->skinned_position, mesh->vertex_first_index.data[ vi ] ) : mesh->vertices.data[ vi ];
			if( !mesh->skinned_position.exists || mesh->skinned_is_local )
				p = ufbx_transform_position( &node->geometry_to_world, p );
			pts.insert( pts.end(), { (float)p.x, (float)p.y, (float)p.z } );
		}
	}
	FILE* f          = fopen( out, "wb" );
	const uint32_t n = uint32_t( pts.size() / 3 );
	const float one  = 1.0f;
	fwrite( &n, 4, 1, f );
	fwrite( &one, 4, 1, f );
	fwrite( pts.data(), 4, pts.size(), f );
	fclose( f );
	printf( "fbxref %u logical verts stack '%.*s' t %.4f -> %s\n", n, (int)st->name.length, st->name.data, t, out );
	ufbx_free_scene( ev );
	ufbx_free_scene( scene );
	return 0;
}
#endif

int main( int argc, char** argv )
{
	if( argc >= 3 && !strcmp( argv[ 1 ], "hash" ) )
	{
		int rc = 0;
		for( int i = 2; i < argc; ++i )
			rc |= Hash( argv[ i ] );
		return rc;
	}
#ifndef NO_ANIM
	if( argc == 6 && !strcmp( argv[ 1 ], "dump" ) )
		return Dump( argv[ 2 ], atoi( argv[ 3 ] ), atof( argv[ 4 ] ), argv[ 5 ] );
	if( argc == 6 && !strcmp( argv[ 1 ], "fbxref" ) )
		return FbxRef( argv[ 2 ], atoi( argv[ 3 ] ), atof( argv[ 4 ] ), argv[ 5 ] );
#endif
	if( argc == 4 && !strcmp( argv[ 1 ], "root" ) ) // W2 A4: a clip's Root Motion node and every translating node's travel
	{
		std::atomic< bool > cancel{ false };
		std::string err;
		auto m = LoadModel( argv[ 2 ], 0, cancel, err );
		if( !m || !m->anim )
			return 1;
		const AnimData& a  = *m->anim;
		const AnimClip& cl = a.clips[ atoi( argv[ 3 ] ) ];
		printf( "clip '%s' rootNode %d '%s'\n", cl.name.c_str(), cl.rootNode, cl.rootNode >= 0 ? a.nodes[ cl.rootNode ].name.c_str() : "-" );
		for( const AnimChannel& ch : cl.channels )
		{
			if( ch.path != AP_T || ch.times.size() < 2 )
				continue;
			double lo[ 3 ] = { 1e30, 1e30, 1e30 }, hi[ 3 ] = { -1e30, -1e30, -1e30 };
			for( size_t k = 0; k < ch.times.size(); ++k )
				for( int q = 0; q < 3; ++q )
					lo[ q ] = std::min( lo[ q ], (double)ch.values[ k * 3 + q ] ), hi[ q ] = std::max( hi[ q ], (double)ch.values[ k * 3 + q ] );
			int d = 0;
			for( int n = ch.node; n >= 0; n = a.nodes[ n ].parent )
				++d;
			if( hi[ 0 ] - lo[ 0 ] + hi[ 1 ] - lo[ 1 ] + hi[ 2 ] - lo[ 2 ] > 1e-6 )
				printf( "  node %d depth %d '%s' T range %.4g %.4g %.4g\n", ch.node, d, a.nodes[ ch.node ].name.c_str(), hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ], hi[ 2 ] - lo[ 2 ] );
		}
		return 0;
	}
	printf( "usage: anim_probe hash <files...> | dump <file> <clip|-1> <t> <out.bin> | fbxref <file> <clip> <t> <out.bin>\n" );
	return 2;
}
