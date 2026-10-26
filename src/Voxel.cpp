// MoveEhr W2 Part B: true voxels (docs/PLAN.md section 7). Per drawn side whose voxels are stale: the slot's surface spots are captured
// through the side's own soup program (its CAPTURE twin: posed, deformed, morph-styled, each part with its texture), splatted into an
// occupancy atlas (the owner = the lowest spot, by the depth test), filled to a visual hull, counted into a HistoPyramid, and listed by
// transform feedback; two indirect commands written on the GPU size the list pass and the instanced draw (no readback, no CPU count).
// A held / paused / static pose rebuilds nothing: the draw (the motions run in its vertex stage) is then the only per-frame cost.
#include "Anim.h"
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "VoxelShaders.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
inline uint64_t Mix( uint64_t h, uint64_t v ) { return ( h ^ ( v + 0x9e3779b97f4a7c15ull + ( h << 6 ) + ( h >> 2 ) ) ) * 0xff51afd7ed558ccdull; }
const uint32_t kEmpty = 0xFFFFFFFFu;

GLuint Compile( GLenum type, const std::string& src, std::string& err )
{
	GLuint s      = glCreateShader( type );
	const char* p = src.c_str();
	glShaderSource( s, 1, &p, nullptr );
	glCompileShader( s );
	GLint ok = 0;
	glGetShaderiv( s, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[ 4096 ];
		glGetShaderInfoLog( s, sizeof( log ), nullptr, log );
		err = log;
		glDeleteShader( s );
		return 0;
	}
	return s;
}
// a program from VS + FS; varyings = transform feedback (interleaved), the FS a stub
GLuint LinkVox( const std::string& vs, const std::string& fs, const char* what, std::vector< const char* > varyings = {} )
{
	std::string err;
	GLuint v = Compile( GL_VERTEX_SHADER, vs, err );
	GLuint f = v ? Compile( GL_FRAGMENT_SHADER, fs, err ) : 0;
	if( !v || !f )
	{
		Log( "VOXEL %s compile failed: %s", what, err.c_str() );
		if( v )
			glDeleteShader( v );
		return 0;
	}
	GLuint p = glCreateProgram();
	glAttachShader( p, v ), glAttachShader( p, f );
	if( !varyings.empty() )
		glTransformFeedbackVaryings( p, (GLsizei)varyings.size(), varyings.data(), GL_INTERLEAVED_ATTRIBS );
	glLinkProgram( p );
	glDeleteShader( v ), glDeleteShader( f );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[ 4096 ];
		glGetProgramInfoLog( p, sizeof( log ), nullptr, log );
		Log( "VOXEL %s link failed: %s", what, log );
		glDeleteProgram( p );
		return 0;
	}
	return p;
}
const char* kStubFS = "#version 410 core\nout vec4 fragColor;\nvoid main() { fragColor = vec4( 0.0 ); }\n";

void Tex2D( GLuint& t, GLenum ifmt, GLenum fmt, GLenum type, int w, int h )
{
	if( !t )
		glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
}
void Fbo( GLuint& f, std::initializer_list< GLuint > colors, GLuint depthRb = 0 )
{
	if( !f )
		glGenFramebuffers( 1, &f );
	glBindFramebuffer( GL_FRAMEBUFFER, f );
	GLenum bufs[ 4 ];
	int i = 0;
	for( GLuint c : colors )
	{
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, c, 0 );
		bufs[ i ] = GL_COLOR_ATTACHMENT0 + i, ++i;
	}
	if( depthRb )
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb );
	glDrawBuffers( i, bufs );
	const GLenum st = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( st != GL_FRAMEBUFFER_COMPLETE )
		Log( "VOXEL framebuffer incomplete 0x%x", st );
}
void Tbo( GLuint& buf, GLuint& tex, GLenum fmt, const void* data, size_t bytes, GLenum usage )
{
	if( !buf )
		glGenBuffers( 1, &buf ), glGenTextures( 1, &tex );
	glBindBuffer( GL_TEXTURE_BUFFER, buf );
	glBufferData( GL_TEXTURE_BUFFER, std::max< size_t >( 16, bytes ), data, usage );
	glBindTexture( GL_TEXTURE_BUFFER, tex );
	glTexBuffer( GL_TEXTURE_BUFFER, fmt, buf ); // after glBufferData (Apple GL keeps the old storage otherwise)
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
}
void BindTex( int unit, GLenum target, GLuint t )
{
	glActiveTexture( GL_TEXTURE0 + unit );
	glBindTexture( target, t );
	// a host (Arena) can leave a sampler object on a unit: its filtering overrides the texture's, and LINEAR makes an integer texture
	// incomplete, so texelFetch reads 0 (every voxel cell "owned": a solid grid). The harness binds none
	glBindSampler( GLuint( unit ), 0 );
}
void Lib( GLuint p, int res, int tiles, int size )
{
	glUniform1i( U( p, "uRes" ), res );
	glUniform1i( U( p, "uTiles" ), tiles );
	glUniform1i( U( p, "uSize" ), size );
}

// W3 part sizes: the body part a bone name says (lower case; "armature" is not an arm): 0 Head 1 Torso 2 Arms 3 Hands 4 Legs, -1 none
int PartOfName( std::string n )
{
	std::transform( n.begin(), n.end(), n.begin(), []( unsigned char ch ) { return char( std::tolower( ch ) ); } );
	for( size_t at; ( at = n.find( "armature" ) ) != std::string::npos; )
		n.erase( at, 8 );
	auto has = [ & ]( std::initializer_list< const char* > ws ) {
		for( const char* w : ws )
			if( n.find( w ) != std::string::npos )
				return true;
		return false;
	};
	if( has( { "hand", "finger", "thumb", "palm", "fist", "wrist" } ) )
		return 3;
	if( has( { "head", "neck", "jaw", "skull" } ) )
		return 0;
	if( has( { "leg", "thigh", "knee", "calf", "shin", "foot", "feet", "toe", "ankle" } ) )
		return 4;
	if( has( { "arm", "shoulder", "clavicle", "elbow", "collar" } ) )
		return 2;
	return -1;
}
// per vertex: the part of its heaviest bone (the first bone up the hierarchy that names one; none = Torso). A model without a skeleton,
// or whose bones name fewer than 2 parts: by bind height (Head = the top 15 %, Legs = below 45 %, Torso between)
std::vector< uint8_t > VertexParts( const CpuGeometry& g, const AnimData* a, const char*& how )
{
	const size_t nv = g.pos.size() / 3;
	std::vector< uint8_t > out( nv, 1 );
	if( a && a->verts.size() == nv && a->joints.size() > 1 )
	{
		std::vector< uint8_t > jp( a->joints.size(), 1 );
		for( size_t j = 0; j < a->joints.size(); ++j )
			for( int n = a->joints[ j ].node; n >= 0 && n < (int)a->nodes.size(); n = a->nodes[ n ].parent )
				if( const int p = PartOfName( a->nodes[ n ].name ); p >= 0 )
				{
					jp[ j ] = uint8_t( p );
					break;
				}
		int seen = 0;
		for( size_t i = 0; i < nv; ++i )
		{
			const AnimVertex& v = a->verts[ i ];
			int best            = 0;
			for( int k = 1; k < kMaxInfluences; ++k )
				if( v.w[ k ] > v.w[ best ] )
					best = k;
			out[ i ] = v.j[ best ] < jp.size() ? jp[ v.j[ best ] ] : 1;
			seen |= 1 << out[ i ];
		}
		if( __builtin_popcount( seen ) >= 2 )
		{
			how = "bones";
			return out;
		}
	}
	float lo = 1e30f, hi = -1e30f;
	for( size_t i = 0; i < nv; ++i )
		lo = std::min( lo, g.pos[ i * 3 + 1 ] ), hi = std::max( hi, g.pos[ i * 3 + 1 ] );
	for( size_t i = 0; i < nv; ++i )
	{
		const float h = ( g.pos[ i * 3 + 1 ] - lo ) / std::max( hi - lo, 1e-12f );
		out[ i ]      = h >= 0.85f ? 0 : h < 0.45f ? 4 : 1;
	}
	how = "height";
	return out;
}

// the spots: a lattice of sub-triangle centroids per triangle, spacing <= half a cell at bind (fitted units); a triangle smaller than one
// spot's share keeps one spot with probability (its area / the share), by a hash of its index. At most 2M spots (the share grows).
void BuildVoxSpots( const CpuGeometry& g, const AnimData* anim, int res, const std::vector< std::array< uint32_t, 3 > >& parts, VoxSpotJob& job )
{
	const auto t0  = std::chrono::steady_clock::now();
	const char* how                 = "";
	const std::vector< uint8_t > vp = VertexParts( g, anim, how );
	size_t perPart[ 5 ]             = {};
	const double s = 2.5 / res * 0.5, f2 = double( g.fitScale ) * g.fitScale;
	double share   = 0.4 * s * s;
	auto area      = [ & ]( size_t t ) {
		const float *a = &g.pos[ g.idx[ t * 3 ] * 3 ], *b = &g.pos[ g.idx[ t * 3 + 1 ] * 3 ], *c = &g.pos[ g.idx[ t * 3 + 2 ] * 3 ];
		const double e1[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, e2[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
		const double x = e1[ 1 ] * e2[ 2 ] - e1[ 2 ] * e2[ 1 ], y = e1[ 2 ] * e2[ 0 ] - e1[ 0 ] * e2[ 2 ], z = e1[ 0 ] * e2[ 1 ] - e1[ 1 ] * e2[ 0 ];
		return 0.5 * std::sqrt( x * x + y * y + z * z ) * f2;
	};
	double total = 0;
	for( const auto& p : parts )
		if( !p[ 2 ] )
			for( size_t t = p[ 0 ] / 3; t < ( size_t( p[ 0 ] ) + p[ 1 ] ) / 3 && t * 3 + 2 < g.idx.size(); ++t )
				total += area( t );
	share = std::max( share, total / 2.0e6 );
	job.ranges.assign( parts.size(), { 0u, 0u } );
	for( size_t pi = 0; pi < parts.size() && !job.cancel.load(); ++pi )
	{
		job.ranges[ pi ][ 0 ] = uint32_t( job.refs.size() / 4 );
		if( !parts[ pi ][ 2 ] )
			for( size_t t = parts[ pi ][ 0 ] / 3; t < ( size_t( parts[ pi ][ 0 ] ) + parts[ pi ][ 1 ] ) / 3 && t * 3 + 2 < g.idx.size(); ++t )
			{
				const double n = area( t ) / share;
				int m          = 0;
				if( n >= 1.0 )
					m = int( std::ceil( std::sqrt( n ) ) );
				else
				{
					const uint64_t h = Mix( 0x5eed, t );
					m                = double( h >> 11 ) / double( 1ull << 53 ) < n ? 1 : 0;
				}
				// the triangle's part: its corners' majority (all differ: the first corner's)
				const uint8_t q0 = vp[ g.idx[ t * 3 ] ], q1 = vp[ g.idx[ t * 3 + 1 ] ], q2 = vp[ g.idx[ t * 3 + 2 ] ];
				const float part = float( q1 == q2 ? q1 : q0 );
				const size_t was = job.refs.size();
				for( int i = 0; i < m; ++i )
					for( int j = 0; i + j < m; ++j )
					{
						job.refs.insert( job.refs.end(), { float( t ), float( ( i + 1.0 / 3.0 ) / m ), float( ( j + 1.0 / 3.0 ) / m ), part } );
						if( i + j + 2 <= m )
							job.refs.insert( job.refs.end(), { float( t ), float( ( i + 2.0 / 3.0 ) / m ), float( ( j + 2.0 / 3.0 ) / m ), part } );
					}
				perPart[ int( part ) ] += ( job.refs.size() - was ) / 4;
			}
		job.ranges[ pi ][ 1 ] = uint32_t( job.refs.size() / 4 ) - job.ranges[ pi ][ 0 ];
	}
	job.ms         = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	const double n = std::max< double >( 1.0, double( job.refs.size() / 4 ) );
	char buf[ 160 ];
	snprintf( buf, sizeof( buf ), "%s: head %.0f %%, torso %.0f %%, arms %.0f %%, hands %.0f %%, legs %.0f %%", how, 100 * perPart[ 0 ] / n, 100 * perPart[ 1 ] / n,
	          100 * perPart[ 2 ] / n, 100 * perPart[ 3 ] / n, 100 * perPart[ 4 ] / n );
	job.parts = buf;
}
}

bool Renderer::InitVoxels()
{
	if( progVoxDraw )
		return true;
	if( voxInitTried )
		return false;
	voxInitTried          = true;
	const auto t0         = std::chrono::steady_clock::now();
	const std::string lib = kVoxLib;
	progVoxSplat  = LinkVox( lib + kVoxSplatVS, lib + kVoxSplatFS, "splat" );
	progVoxExtent = LinkVox( lib + kVoxQuadVS, lib + kVoxExtentFS, "extents" );
	progVoxBase   = LinkVox( lib + kVoxQuadVS, lib + kVoxBaseFS, "base" );
	progVoxBaseC  = LinkVox( lib + kVoxQuadVS, "#version 410 core\n#define CARVE\n" + lib.substr( lib.find( '\n' ) + 1 ) + kVoxBaseFS, "base (carve)" );
	progVoxBlur   = LinkVox( lib + kVoxQuadVS, lib + kVoxBlurFS, "blur" );
	progVoxCount  = LinkVox( lib + kVoxQuadVS, lib + kVoxMcCountFS, "smooth count" );
	progVoxReduce = LinkVox( lib + kVoxQuadVS, lib + kVoxReduceFS, "reduce" );
	progVoxCmd    = LinkVox( lib + kVoxCmdVS, kStubFS, "commands", { "oTrav", "oDraw" } );
	progVoxTrav   = LinkVox( lib + kVoxTraverseVS, kStubFS, "traverse", { "oCell" } );
	progVoxDraw   = LinkVox( lib + kVoxMotion + kVoxDrawVS, lib + kVoxShade + kVoxDrawFS, "draw" );
	progVoxSmooth = LinkVox( lib + kVoxMotion + kVoxSmoothVS, lib + kVoxShade + kVoxSmoothFS, "smooth draw" );
	const std::string plainLib = "#version 410 core\n#define PLAIN\n" + lib.substr( lib.find( '\n' ) + 1 );
	progVoxPlain = LinkVox( plainLib + kVoxMotion + kVoxDrawVS, lib + kVoxShade + kVoxPlainFS, "plain cube draw" );
	// W3 part sizes: the BLOCKS twins draw a merged list (a block = one proxy 2^level cells across)
	const std::string blkLib   = "#version 410 core\n#define BLOCKS\n" + lib.substr( lib.find( '\n' ) + 1 );
	const std::string blkPlain = "#version 410 core\n#define BLOCKS\n#define PLAIN\n" + lib.substr( lib.find( '\n' ) + 1 );
	progVoxDrawB  = LinkVox( blkLib + kVoxMotion + kVoxDrawVS, lib + kVoxShade + kVoxDrawFS, "draw (blocks)" );
	progVoxPlainB = LinkVox( blkPlain + kVoxMotion + kVoxDrawVS, lib + kVoxShade + kVoxPlainFS, "plain cube draw (blocks)" );
	// W4 A1: the casters (shadow maps)
	const std::string shLib  = "#version 410 core\n#define SHADOW\n" + lib.substr( lib.find( '\n' ) + 1 );
	const std::string shBlk  = "#version 410 core\n#define SHADOW\n#define BLOCKS\n" + lib.substr( lib.find( '\n' ) + 1 );
	const std::string shPln  = "#version 410 core\n#define SHADOW\n#define BLOCKS\n#define PLAIN\n" + lib.substr( lib.find( '\n' ) + 1 );
	progVoxDrawS   = LinkVox( shBlk + kVoxMotion + kVoxDrawVS, shLib + kVoxShade + kVoxDrawFS, "draw (shadow)" );
	progVoxPlainS  = LinkVox( shPln + kVoxMotion + kVoxDrawVS, shLib + kVoxShade + kVoxPlainFS, "plain cube draw (shadow)" );
	progVoxSmoothS = LinkVox( shLib + kVoxMotion + kVoxSmoothVS, shLib + kVoxShade + kVoxSmoothFS, "smooth draw (shadow)" );
	progVoxBlock = LinkVox( lib + kVoxQuadVS, lib + kVoxBlockFS, "part blocks" );
	progVoxMerge = LinkVox( lib + kVoxQuadVS, lib + kVoxMergeFS, "part merge" );
	const GLuint all[] = { progVoxSplat, progVoxExtent, progVoxBase, progVoxBlur, progVoxCount, progVoxReduce, progVoxCmd, progVoxTrav, progVoxDraw, progVoxSmooth, progVoxPlain,
	                       progVoxBlock, progVoxMerge, progVoxDrawB, progVoxPlainB, progVoxDrawS, progVoxPlainS, progVoxSmoothS, progVoxBaseC };
	for( GLuint p : all )
		if( !p )
		{
			ReleaseVoxels();
			voxInitTried = true; // do not retry every frame
			return false;
		}
	// sampler units (each its own: Apple GL refuses a draw with two sampler types on one unit)
	for( GLuint p : all )
	{
		glUseProgram( p );
		const std::pair< const char*, int > u[] = { { "uCapPos", 45 }, { "uCapCol", 46 }, { "uOwner", 47 }, { "uExtXY", 48 }, { "uExtZ", 49 }, { "uField", 50 },
		                                            { "uBase", 52 },   { "uHpA", 53 },    { "uOwnRes", 54 }, { "uList", 55 },  { "uHpB", 56 },
		                                            { "uSolid", 52 },  { "uRef", 57 },    { "uCntF", 58 },   { "uOwnF", 59 },  { "uBlk2", 60 },
		                                            { "uBlk4", 61 },   { "uBlk8", 62 },   { "uCapNrm", 51 } };
		for( const auto& x : u )
			glUniform1i( U( p, x.first ), x.second );
	}
	glUseProgram( 0 );
	glGenVertexArrays( 1, &voxVao );
	Log( "VOXEL programs linked in %.0f ms", std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
	return true;
}

void Renderer::ReleaseVoxels()
{
	for( VoxSlot& v : vox )
	{
		for( GLuint* b : { &v.refBuf, &v.capBuf[ 0 ], &v.capBuf[ 1 ], &v.capBuf[ 2 ], &v.listBuf, &v.cmdBuf } )
			if( *b )
				glDeleteBuffers( 1, b ), *b = 0;
		for( GLuint* t : { &v.refTex, &v.capTex[ 0 ], &v.capTex[ 1 ], &v.capTex[ 2 ], &v.listTex, &v.ownerTex, &v.extTex[ 0 ], &v.extTex[ 1 ], &v.countTex, &v.ownTex,
		                   &v.fieldTex[ 0 ], &v.fieldTex[ 1 ], &v.hpTex[ 0 ], &v.hpTex[ 1 ], &v.blkTex[ 0 ], &v.blkTex[ 1 ], &v.blkTex[ 2 ], &v.mCountTex, &v.mOwnTex } )
			if( *t )
				glDeleteTextures( 1, t ), *t = 0;
		for( GLuint* f : { &v.splatFbo, &v.extFbo, &v.baseFbo, &v.fieldFbo[ 0 ], &v.fieldFbo[ 1 ], &v.hpFbo[ 0 ], &v.hpFbo[ 1 ], &v.countFbo, &v.blkFbo[ 0 ], &v.blkFbo[ 1 ],
		                   &v.blkFbo[ 2 ], &v.mergeFbo } )
			if( *f )
				glDeleteFramebuffers( 1, f ), *f = 0;
		if( v.depthRb )
			glDeleteRenderbuffers( 1, &v.depthRb ), v.depthRb = 0;
		if( v.job )
			v.job->cancel.store( true );
		v = VoxSlot();
	}
	for( GLuint* p : { &progVoxSplat, &progVoxExtent, &progVoxBase, &progVoxBlur, &progVoxCount, &progVoxReduce, &progVoxCmd, &progVoxTrav, &progVoxDraw, &progVoxSmooth,
	                   &progVoxPlain, &progVoxBlock, &progVoxMerge, &progVoxDrawB, &progVoxPlainB, &progVoxDrawS, &progVoxPlainS, &progVoxSmoothS, &progVoxBaseC } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	if( voxVao )
		glDeleteVertexArrays( 1, &voxVao ), voxVao = 0;
	voxInitTried = false;
}

bool Renderer::VoxSpots( int slot, GpuModel* model, int res )
{
	VoxSlot& v         = vox[ slot ];
	const uint64_t key = Mix( model->serial, uint64_t( res ) );
	if( v.wantKey != key )
	{
		if( v.job )
			v.job->cancel.store( true );
		v.wantKey = key;
		auto job  = std::make_shared< VoxSpotJob >();
		v.job     = job;
		std::vector< std::array< uint32_t, 3 > > parts;
		for( const GpuPart& gp : model->parts )
			parts.push_back( { gp.firstIndex, gp.indexCount, gp.fractured ? 1u : 0u } );
		auto geo  = model->geometry;
		auto anim = model->anim;
		auto name = LogName( model->path );
		std::thread( [ job, geo, anim, res, parts, name ]() {
			BuildVoxSpots( *geo, anim.get(), res, parts, *job );
			Log( "VOXEL spots for %s at res %d: %zu in %.0f ms (worker); parts by %s", name.c_str(), res, job->refs.size() / 4, job->ms, job->parts.c_str() );
			job->done.store( true );
		} ).detach();
	}
	if( v.job && v.job->done.load() && !v.job->cancel.load() )
	{
		Tbo( v.refBuf, v.refTex, GL_RGBA32F, v.job->refs.data(), v.job->refs.size() * sizeof( float ), GL_STATIC_DRAW );
		v.ranges  = v.job->ranges;
		v.spots   = v.job->refs.size() / 4;
		v.spotKey = key;
		v.job.reset();
	}
	return v.refTex && v.spots > 0;
}

bool Renderer::VoxTargets( VoxSlot& v, int res )
{
	if( v.res == res && v.splatFbo )
		return true;
	v.res   = res;
	v.tiles = int( std::ceil( std::sqrt( double( res ) ) ) );
	v.size  = 1;
	while( v.size < v.tiles * res )
		v.size *= 2;
	v.top       = int( std::lround( std::log2( double( v.size ) ) ) );
	const int S = v.size;
	Tex2D( v.ownerTex, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, S, S );
	if( !v.depthRb )
		glGenRenderbuffers( 1, &v.depthRb );
	glBindRenderbuffer( GL_RENDERBUFFER, v.depthRb );
	glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, S, S );
	glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	Fbo( v.splatFbo, { v.ownerTex }, v.depthRb );
	Tex2D( v.extTex[ 0 ], GL_RGBA32F, GL_RGBA, GL_FLOAT, res, res );
	Tex2D( v.extTex[ 1 ], GL_RGBA32F, GL_RGBA, GL_FLOAT, res, res );
	Fbo( v.extFbo, { v.extTex[ 0 ], v.extTex[ 1 ] } );
	Tex2D( v.countTex, GL_R32F, GL_RED, GL_FLOAT, S, S );
	Tex2D( v.ownTex, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, S, S );
	Fbo( v.baseFbo, { v.countTex, v.ownTex } );
	Fbo( v.countFbo, { v.countTex } );
	Tex2D( v.mCountTex, GL_R32F, GL_RED, GL_FLOAT, S, S );
	Tex2D( v.mOwnTex, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, S, S );
	Fbo( v.mergeFbo, { v.mCountTex, v.mOwnTex } );
	for( int k = 0; k < 3; ++k )
	{
		v.blkN[ k ] = ( res + ( 2 << k ) - 1 ) >> ( k + 1 );
		v.blkT[ k ] = int( std::ceil( std::sqrt( double( v.blkN[ k ] ) ) ) );
		const int w = v.blkT[ k ] * v.blkN[ k ];
		Tex2D( v.blkTex[ k ], GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, w, w );
		Fbo( v.blkFbo[ k ], { v.blkTex[ k ] } );
	}
	for( int k = 0; k < 2; ++k )
	{
		Tex2D( v.fieldTex[ k ], GL_R32F, GL_RED, GL_FLOAT, S, S );
		Fbo( v.fieldFbo[ k ], { v.fieldTex[ k ] } );
		Tex2D( v.hpTex[ k ], GL_RGBA32F, GL_RGBA, GL_FLOAT, S, std::max( 1, S / 2 ) );
		Fbo( v.hpFbo[ k ], { v.hpTex[ k ] } );
	}
	glBindTexture( GL_TEXTURE_2D, 0 );
	const size_t cells = size_t( res ) * res * res;
	v.listCap          = std::max( cells, std::min< size_t >( cells * 2, size_t( 4 ) << 20 ) );
	Tbo( v.listBuf, v.listTex, GL_RG32UI, nullptr, v.listCap * 8, GL_DYNAMIC_COPY );
	if( !v.cmdBuf )
		glGenBuffers( 1, &v.cmdBuf );
	glBindBuffer( GL_ARRAY_BUFFER, v.cmdBuf );
	glBufferData( GL_ARRAY_BUFFER, 32, nullptr, GL_DYNAMIC_COPY );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	v.built = false;
	Log( "VOXEL targets: res %d, atlas %d x %d (%d tiles a row), list capacity %zu", res, S, S, v.tiles, v.listCap );
	return true;
}

bool Renderer::BuildVoxels( const FrameState& fs, GpuModel* model, GLuint soupProg, bool everyFrame )
{
	if( !model || !model->geometry || !model->Complete() || fs.vox.look <= 0 || !InitVoxels() )
		return false;
	const int slot = SlotOf( model );
	if( slot < 0 || slot >= kSlots )
		return false;
	VoxSlot& v    = vox[ slot ];
	const int res = std::clamp( fs.vox.res, 8, 128 );
	if( !VoxSpots( slot, model, res ) )
		return false;
	VoxTargets( v, res );
	const bool smooth  = fs.vox.look == 2;
	const SlotPose* ps = PosedFor( model );
	uint64_t key       = Mix( Mix( Mix( model->serial, ps ? pose[ slot ].gen + 1 : 0 ), uint64_t( res ) ), v.spotKey );
	key                = Mix( key, smooth ? 1 + uint64_t( std::lround( fs.vox.bevel * 1000 ) ) * 2000 + uint64_t( std::lround( fs.vox.gap * 1000 ) ) : 0 );
	int maxLvl         = 0;
	uint64_t partKey   = 0;
	for( int k = 0; k < 5; ++k )
		maxLvl = std::max( maxLvl, fs.vox.partLvl[ k ] ), partKey = partKey * 4 + uint64_t( std::clamp( fs.vox.partLvl[ k ], 0, 3 ) );
	key = maxLvl > 0 ? Mix( key, partKey + 1 ) : key;
	if( !everyFrame && v.built && v.builtKey == key )
		return true;
	const auto t0 = std::chrono::steady_clock::now();
	// 1. capture the spots through the side's own program
	if( v.capSize < v.spots )
	{
		v.capSize = v.spots;
		for( int k = 0; k < 3; ++k )
			Tbo( v.capBuf[ k ], v.capTex[ k ], GL_RGBA32F, nullptr, v.capSize * 16, GL_DYNAMIC_COPY );
	}
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, v.capBuf[ k ] );
	// transform feedback needs a complete framebuffer bound: the host's (Arena's) is not, before the main pass (0x506; the harness's is)
	GLint capFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &capFbo );
	while( glGetError() != GL_NO_ERROR ) // what came before is not the capture's
		;
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	captureNow    = true;
	captureRefTex = v.refTex;
	captureRanges = &v.ranges;
	capTime = 0.0f, capSwirl = 0.0f, capPhase = 0.0f, capAxis = 1;
	DrawModel( soupProg, fs, model, false, true, 0 );
	captureNow    = false;
	captureRanges = nullptr;
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, 0 );
	glBindFramebuffer( GL_FRAMEBUFFER, capFbo );
	if( const GLenum e = glGetError() ) // once per program run: a failed capture would voxelise garbage
	{
		static bool said = false;
		if( !said )
			Log( "VOXEL capture GL error 0x%x", e ), said = true;
	}
	// GL state the passes change
	GLint prevFbo = 0, vp[ 4 ];
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &prevFbo );
	glGetIntegerv( GL_VIEWPORT, vp );
	const GLboolean depthOn = glIsEnabled( GL_DEPTH_TEST ), blendOn = glIsEnabled( GL_BLEND ), cullOn = glIsEnabled( GL_CULL_FACE ), scissorOn = glIsEnabled( GL_SCISSOR_TEST );
	GLboolean depthMask;
	glGetBooleanv( GL_DEPTH_WRITEMASK, &depthMask );
	GLint depthFunc;
	glGetIntegerv( GL_DEPTH_FUNC, &depthFunc );
	// the host (Arena) can leave state the harness never does: pin everything the passes depend on, and put it back after
	GLboolean colorMask[ 4 ];
	glGetBooleanv( GL_COLOR_WRITEMASK, colorMask );
	GLfloat depthRange[ 2 ];
	glGetFloatv( GL_DEPTH_RANGE, depthRange );
	const GLboolean srgbOn = glIsEnabled( GL_FRAMEBUFFER_SRGB ), stencilOn = glIsEnabled( GL_STENCIL_TEST ), offsetOn = glIsEnabled( GL_POLYGON_OFFSET_FILL ),
	                a2cOn = glIsEnabled( GL_SAMPLE_ALPHA_TO_COVERAGE ), discardOn = glIsEnabled( GL_RASTERIZER_DISCARD );
	if( LogVerbose() && !v.stateLogged )
	{
		GLint smp[ 3 ] = {};
		for( int k = 0; k < 3; ++k )
		{
			glActiveTexture( GL_TEXTURE0 + 47 + k );
			glGetIntegerv( GL_SAMPLER_BINDING, &smp[ k ] );
		}
		glActiveTexture( GL_TEXTURE0 );
		Log( "VOXEL host sampler objects on units 47 / 48 / 49 before the passes: %d / %d / %d", smp[ 0 ], smp[ 1 ], smp[ 2 ] );
		v.stateLogged = true;
		Log( "VOXEL host state at build: color mask %d%d%d%d, depth range %.2f..%.2f, srgb %d, stencil %d, offset %d, a2c %d, discard %d, depth %d, blend %d, "
		     "cull %d, scissor %d, fbo %d",
		     colorMask[ 0 ], colorMask[ 1 ], colorMask[ 2 ], colorMask[ 3 ], depthRange[ 0 ], depthRange[ 1 ], srgbOn, stencilOn, offsetOn, a2cOn, discardOn, depthOn,
		     blendOn, cullOn, scissorOn, prevFbo );
	}
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glDepthRange( 0.0, 1.0 );
	glDisable( GL_FRAMEBUFFER_SRGB ), glDisable( GL_STENCIL_TEST ), glDisable( GL_POLYGON_OFFSET_FILL ), glDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	glDisable( GL_RASTERIZER_DISCARD );
	glDisable( GL_BLEND ), glDisable( GL_CULL_FACE ), glDisable( GL_SCISSOR_TEST );
	glBindVertexArray( voxVao );
	const int S = v.size, R = v.res;
	// grid (fitted) <- world: the capture wrote w = Model (f / fitScale + fitCenter)
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	const Mat4 gridFromWorld = Mat4::Scale( model->fitScale ) * Mat4::Translate( -model->fitCenter[ 0 ], -model->fitCenter[ 1 ], -model->fitCenter[ 2 ] ) * M.Inverse();
	const float gridMin = -1.25f, cell = 2.5f / float( R );
	// 2. splat
	glBindFramebuffer( GL_FRAMEBUFFER, v.splatFbo );
	glViewport( 0, 0, S, S );
	const GLuint clearU[ 4 ] = { kEmpty, 0, 0, 0 };
	glClearBufferuiv( GL_COLOR, 0, clearU );
	glDepthMask( GL_TRUE );
	glClearDepth( 1.0 );
	glClear( GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glEnable( GL_PROGRAM_POINT_SIZE );
	glUseProgram( progVoxSplat );
	Lib( progVoxSplat, R, v.tiles, S );
	glUniformMatrix4fv( U( progVoxSplat, "uGridFromWorld" ), 1, GL_FALSE, gridFromWorld.m );
	glUniform1f( U( progVoxSplat, "uGridMin" ), gridMin );
	glUniform1f( U( progVoxSplat, "uCell" ), cell );
	glUniform1f( U( progVoxSplat, "uSpots" ), float( v.spots ) );
	BindTex( 45, GL_TEXTURE_BUFFER, v.capTex[ 0 ] );
	BindTex( 46, GL_TEXTURE_BUFFER, v.capTex[ 2 ] );
	glDrawArrays( GL_POINTS, 0, (GLsizei)v.spots );
	glDisable( GL_PROGRAM_POINT_SIZE );
	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	// 3. column extents
	glBindFramebuffer( GL_FRAMEBUFFER, v.extFbo );
	glViewport( 0, 0, R, R );
	glUseProgram( progVoxExtent );
	Lib( progVoxExtent, R, v.tiles, S );
	BindTex( 47, GL_TEXTURE_2D, v.ownerTex );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	// 4. solid + owners (+ the smooth field and its counts)
	glBindFramebuffer( GL_FRAMEBUFFER, v.baseFbo );
	glViewport( 0, 0, S, S );
	static const bool noCarve = getenv( "MOVEEHR_NO_CARVE" ) != nullptr; // W4 A2 debug: W3's hull fill (the "same" gates)
	const GLuint baseP        = noCarve ? progVoxBase : progVoxBaseC;
	glUseProgram( baseP );
	Lib( baseP, R, v.tiles, S );
	BindTex( 48, GL_TEXTURE_2D, v.extTex[ 0 ] );
	BindTex( 49, GL_TEXTURE_2D, v.extTex[ 1 ] );
	if( !noCarve )
	{
		const Mat4 worldFromGrid = gridFromWorld.Inverse();
		const float r[ 9 ]       = { worldFromGrid.m[ 0 ], worldFromGrid.m[ 1 ], worldFromGrid.m[ 2 ], worldFromGrid.m[ 4 ], worldFromGrid.m[ 5 ],
		                             worldFromGrid.m[ 6 ], worldFromGrid.m[ 8 ], worldFromGrid.m[ 9 ], worldFromGrid.m[ 10 ] };
		glUniformMatrix3fv( U( baseP, "uGridToWorldR" ), 1, GL_FALSE, r );
		glUniformMatrix4fv( U( baseP, "uWorldFromGridC" ), 1, GL_FALSE, worldFromGrid.m );
		glUniform1f( U( baseP, "uGridMin" ), gridMin );
		glUniform1f( U( baseP, "uCell" ), cell );
		static const float trimK = getenv( "MOVEEHR_TRIM" ) ? float( atof( getenv( "MOVEEHR_TRIM" ) ) ) : 0.35f; // share of a cell (W4 A2 sweep: 0.35 keeps the misses within 0.2 %)
		const float cellW        = cell * std::sqrt( worldFromGrid.m[ 0 ] * worldFromGrid.m[ 0 ] + worldFromGrid.m[ 1 ] * worldFromGrid.m[ 1 ] + worldFromGrid.m[ 2 ] * worldFromGrid.m[ 2 ] );
		glUniform1f( U( baseP, "uTrim" ), trimK * cellW );
		BindTex( 45, GL_TEXTURE_BUFFER, v.capTex[ 0 ] );
		BindTex( 51, GL_TEXTURE_BUFFER, v.capTex[ 1 ] );
	}
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	// 4b. W3 part sizes: the block atlases (x2 .. the largest size in use), then the merged count / owner atlases the later passes read
	v.merged               = maxLvl > 0;
	const GLuint cntTex    = v.merged ? v.mCountTex : v.countTex;
	const GLuint ownResTex = v.merged ? v.mOwnTex : v.ownTex;
	if( v.merged )
	{
		BindTex( 57, GL_TEXTURE_BUFFER, v.refTex );
		BindTex( 58, GL_TEXTURE_2D, v.countTex );
		BindTex( 59, GL_TEXTURE_2D, v.ownTex );
		glUseProgram( progVoxBlock );
		GLint pl[ 5 ];
		for( int k = 0; k < 5; ++k )
			pl[ k ] = std::clamp( fs.vox.partLvl[ k ], 0, 3 );
		glUniform1iv( U( progVoxBlock, "uPartLvl" ), 5, pl );
		glUniform1i( U( progVoxBlock, "uFRes" ), R );
		glUniform1i( U( progVoxBlock, "uFTiles" ), v.tiles );
		for( int k = 0; k < maxLvl; ++k )
		{
			const int w = v.blkT[ k ] * v.blkN[ k ];
			glBindFramebuffer( GL_FRAMEBUFFER, v.blkFbo[ k ] );
			glViewport( 0, 0, w, w );
			Lib( progVoxBlock, v.blkN[ k ], v.blkT[ k ], w );
			glUniform1i( U( progVoxBlock, "uL" ), 2 << k );
			glUniform1i( U( progVoxBlock, "uLvl" ), k + 1 );
			glDrawArrays( GL_TRIANGLES, 0, 3 );
		}
		for( int k = 0; k < 3; ++k )
			BindTex( 60 + k, GL_TEXTURE_2D, v.blkTex[ k ] );
		glBindFramebuffer( GL_FRAMEBUFFER, v.mergeFbo );
		glViewport( 0, 0, S, S );
		glUseProgram( progVoxMerge );
		Lib( progVoxMerge, R, v.tiles, S );
		glUniform3i( U( progVoxMerge, "uBN" ), v.blkN[ 0 ], v.blkN[ 1 ], v.blkN[ 2 ] );
		glUniform3i( U( progVoxMerge, "uBT" ), v.blkT[ 0 ], v.blkT[ 1 ], v.blkT[ 2 ] );
		glUniform1i( U( progVoxMerge, "uMaxLvl" ), maxLvl );
		glUniform1i( U( progVoxMerge, "uSmoothM" ), smooth ? 1 : 0 );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	if( smooth )
	{
		const float sigma = 0.5f + 2.0f * std::clamp( fs.vox.bevel, 0.0f, 1.0f );
		glUseProgram( progVoxBlur );
		Lib( progVoxBlur, R, v.tiles, S );
		glUniform1f( U( progVoxBlur, "uSigma" ), sigma );
		const GLuint src[ 3 ] = { cntTex, v.fieldTex[ 0 ], v.fieldTex[ 1 ] };
		const GLuint dst[ 3 ] = { v.fieldFbo[ 0 ], v.fieldFbo[ 1 ], v.fieldFbo[ 0 ] };
		for( int a = 0; a < 3; ++a )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, dst[ a ] );
			BindTex( 50, GL_TEXTURE_2D, src[ a ] );
			glUniform3i( U( progVoxBlur, "uAxis" ), a == 0, a == 1, a == 2 );
			glDrawArrays( GL_TRIANGLES, 0, 3 );
		}
		BindTex( 50, GL_TEXTURE_2D, v.fieldTex[ 0 ] );
		glBindFramebuffer( GL_FRAMEBUFFER, v.countFbo );
		glUseProgram( progVoxCount );
		Lib( progVoxCount, R, v.tiles, S );
		glUniform1f( U( progVoxCount, "uIso" ), 0.35f + 0.3f * std::clamp( fs.vox.gap, 0.0f, 0.9f ) );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	// 5. the HistoPyramid (level j into atlas j odd ? A : B, at x = S - (S >> (j - 1)))
	glUseProgram( progVoxReduce );
	Lib( progVoxReduce, R, v.tiles, S );
	BindTex( 52, GL_TEXTURE_2D, smooth ? v.countTex : cntTex ); // SMOOTH: the triangle counts the count pass wrote
	for( int j = 1; j <= v.top; ++j )
	{
		const int dst = ( j & 1 ) ? 0 : 1;
		// the atlas written this pass is not bound to any unit (the level below is read from the other one)
		BindTex( 53, GL_TEXTURE_2D, dst == 0 ? 0 : v.hpTex[ 0 ] );
		BindTex( 56, GL_TEXTURE_2D, dst == 1 ? 0 : v.hpTex[ 1 ] );
		glBindFramebuffer( GL_FRAMEBUFFER, v.hpFbo[ dst ] );
		glViewport( S - ( S >> ( j - 1 ) ), 0, S >> j, S >> j );
		glUniform1i( U( progVoxReduce, "uLevelJ" ), j );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
	}
	BindTex( 53, GL_TEXTURE_2D, v.hpTex[ 0 ] );
	BindTex( 56, GL_TEXTURE_2D, v.hpTex[ 1 ] );
	// 6. the indirect commands, then the list (transform feedback; a complete FBO bound)
	glBindFramebuffer( GL_FRAMEBUFFER, v.splatFbo );
	glEnable( GL_RASTERIZER_DISCARD );
	glUseProgram( progVoxCmd );
	Lib( progVoxCmd, R, v.tiles, S );
	glUniform1i( U( progVoxCmd, "uTop" ), v.top );
	glUniform1ui( U( progVoxCmd, "uCap" ), (GLuint)v.listCap );
	glUniform1ui( U( progVoxCmd, "uVerts" ), smooth ? 3u : 36u );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, v.cmdBuf );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArrays( GL_POINTS, 0, 1 );
	glEndTransformFeedback();
	glUseProgram( progVoxTrav );
	Lib( progVoxTrav, R, v.tiles, S );
	glUniform1i( U( progVoxTrav, "uTop" ), v.top );
	BindTex( 54, GL_TEXTURE_2D, ownResTex );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, v.listBuf );
	glBindBuffer( GL_DRAW_INDIRECT_BUFFER, v.cmdBuf );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArraysIndirect( GL_POINTS, (const void*)0 );
	glEndTransformFeedback();
	glBindBuffer( GL_DRAW_INDIRECT_BUFFER, 0 );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	glDisable( GL_RASTERIZER_DISCARD );
	glActiveTexture( GL_TEXTURE0 );
	glUseProgram( 0 );
	glBindVertexArray( 0 );
	// restore
	glBindFramebuffer( GL_FRAMEBUFFER, prevFbo );
	glViewport( vp[ 0 ], vp[ 1 ], vp[ 2 ], vp[ 3 ] );
	depthOn ? glEnable( GL_DEPTH_TEST ) : glDisable( GL_DEPTH_TEST );
	blendOn ? glEnable( GL_BLEND ) : glDisable( GL_BLEND );
	cullOn ? glEnable( GL_CULL_FACE ) : glDisable( GL_CULL_FACE );
	scissorOn ? glEnable( GL_SCISSOR_TEST ) : glDisable( GL_SCISSOR_TEST );
	glDepthMask( depthMask );
	glDepthFunc( depthFunc );
	glColorMask( colorMask[ 0 ], colorMask[ 1 ], colorMask[ 2 ], colorMask[ 3 ] );
	glDepthRange( depthRange[ 0 ], depthRange[ 1 ] );
	srgbOn ? glEnable( GL_FRAMEBUFFER_SRGB ) : glDisable( GL_FRAMEBUFFER_SRGB );
	stencilOn ? glEnable( GL_STENCIL_TEST ) : glDisable( GL_STENCIL_TEST );
	offsetOn ? glEnable( GL_POLYGON_OFFSET_FILL ) : glDisable( GL_POLYGON_OFFSET_FILL );
	a2cOn ? glEnable( GL_SAMPLE_ALPHA_TO_COVERAGE ) : glDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	discardOn ? glEnable( GL_RASTERIZER_DISCARD ) : glDisable( GL_RASTERIZER_DISCARD );
	if( LogVerbose() && ++v.countLogs <= 3 )
	{
		// the voxel count the GPU wrote (a blocking read: Debug Log only)
		GLuint cmd[ 8 ] = {};
		glBindBuffer( GL_ARRAY_BUFFER, v.cmdBuf );
		glGetBufferSubData( GL_ARRAY_BUFFER, 0, sizeof( cmd ), cmd );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		Log( "VOXEL list slot %c: %u entries of %zu cells (res %d)", 'A' + slot, cmd[ 5 ], size_t( R ) * R * R, R );
	}
	if( LogVerbose() )
		Log( "VOXEL build slot %c: %zu spots, res %d%s, %.2f ms CPU (every frame %d, posed %d gen %llu, key %s)", 'A' + slot, v.spots, R, smooth ? " smooth" : "",
		     std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count(), everyFrame ? 1 : 0, ps ? 1 : 0,
		     (unsigned long long)( ps ? pose[ slot ].gen : 0 ), v.builtKey == key ? "same" : "changed" );
	v.built = true, v.builtKey = key, v.smooth = smooth;
	return true;
}

void Renderer::DrawVoxels( const FrameState& fs, GpuModel* model, int hopSide, float hopE, const float* vpOverride, const float* shadowLightPos )
{
	if( !model || fs.vox.look <= 0 || !progVoxDraw )
		return;
	const int slot = SlotOf( model );
	if( slot < 0 || slot >= kSlots || !vox[ slot ].built )
		return;
	const VoxSlot& v = vox[ slot ];
	if( v.smooth != ( fs.vox.look == 2 ) )
		return; // the list was made for the other kind (rebuilt next frame)
	const bool smooth = v.smooth;
	// Cubes with Bevel 0: the proxy is the voxel (no ray, no gl_FragDepth: the early depth test works)
	const bool plain = !smooth && fs.vox.look == 1 && fs.vox.bevel <= 0.0f;
	const bool caster = shadowLightPos != nullptr;
	const GLuint p    = caster ? ( smooth ? progVoxSmoothS : plain ? progVoxPlainS : progVoxDrawS )
	                           : smooth ? progVoxSmooth : plain ? ( v.merged ? progVoxPlainB : progVoxPlain ) : ( v.merged ? progVoxDrawB : progVoxDraw );
	glUseProgram( p );
	if( caster )
	{
		glUniform3fv( U( p, "uShadowLightPos" ), 1, shadowLightPos );
		glUniform3fv( U( p, "uShadowDir" ), 1, casterDir );
		glUniform1i( U( p, "uShadowOrtho" ), casterOrtho );
	}
	Lib( p, v.res, v.tiles, v.size );
	Mat4 M, V, P;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 worldFromGrid = M * Mat4::Translate( model->fitCenter[ 0 ], model->fitCenter[ 1 ], model->fitCenter[ 2 ] ) * Mat4::Scale( 1.0f / std::max( model->fitScale, 1e-12f ) );
	Mat4 VP = P * V;
	if( vpOverride )
		std::memcpy( VP.m, vpOverride, sizeof( VP.m ) );
	const float ws = std::sqrt( worldFromGrid.m[ 0 ] * worldFromGrid.m[ 0 ] + worldFromGrid.m[ 1 ] * worldFromGrid.m[ 1 ] + worldFromGrid.m[ 2 ] * worldFromGrid.m[ 2 ] );
	glUniformMatrix4fv( U( p, "uWorldFromGrid" ), 1, GL_FALSE, worldFromGrid.m );
	glUniformMatrix4fv( U( p, "uVP" ), 1, GL_FALSE, VP.m );
	glUniformMatrix4fv( U( p, "uInvVP" ), 1, GL_FALSE, VP.Inverse().m );
	glUniform1f( U( p, "uWorldScale" ), std::max( ws, 1e-12f ) );
	GLint vpx[ 4 ];
	glGetIntegerv( GL_VIEWPORT, vpx );
	glUniform2f( U( p, "uViewport" ), float( std::max( 1, vpx[ 2 ] ) ), float( std::max( 1, vpx[ 3 ] ) ) );
	glUniform1f( U( p, "uGridMin" ), -1.25f );
	glUniform1f( U( p, "uCell" ), 2.5f / float( v.res ) );
	glUniform1f( U( p, "uGap" ), std::clamp( fs.vox.gap, 0.0f, 0.9f ) );
	glUniform1f( U( p, "uBevel" ), std::clamp( fs.vox.bevel, 0.0f, 1.0f ) );
	glUniform1i( U( p, "uLook" ), fs.vox.look );
	glUniform1f( U( p, "uIso" ), 0.35f + 0.3f * std::clamp( fs.vox.gap, 0.0f, 0.9f ) );
	glUniform1i( U( p, "uMotion" ), fs.vox.motion );
	glUniform1f( U( p, "uAmt" ), fs.vox.amount );
	glUniform1f( U( p, "uDis" ), fs.vox.dis );
	glUniform1f( U( p, "uPhase" ), fs.vox.phase );
	glUniform1f( U( p, "uBeat" ), fs.vox.beat );
	glUniform1f( U( p, "uBeatN" ), fs.vox.beatN );
	glUniform1f( U( p, "uReveal" ), fs.vox.reveal );
	glUniform1i( U( p, "uRevealStyle" ), fs.vox.revealStyle );
	glUniform1i( U( p, "uHopSide" ), hopSide );
	glUniform1f( U( p, "uHopE" ), hopE );
	// still voxels walled in on all 6 sides cannot be seen (no motion, all revealed, no hop, gaps too thin to look through): skipped
	glUniform1i( U( p, "uCullInside" ), fs.vox.motion == 0 && fs.vox.reveal >= 1.0f && hopSide < 0 && fs.vox.gap <= 0.2f ? 1 : 0 );
	glUniform3fv( U( p, "uCamPos" ), 1, fs.camPos );
	glUniform3fv( U( p, "uKeyDir" ), 1, fs.keyDir );
	glUniform3fv( U( p, "uKeyColor" ), 1, fs.keyColor );
	glUniform3fv( U( p, "uFillDir" ), 1, fs.fillDir );
	glUniform3fv( U( p, "uFillColor" ), 1, fs.fillColor );
	glUniform1f( U( p, "uAmbient" ), fs.ambient );
	glUniform1f( U( p, "uExposure" ), fs.exposure );
	glUniform1i( U( p, "uTonemap" ), fs.tonemap );
	const float* bc = model->parts.empty() ? nullptr : model->parts[ 0 ].baseColor;
	glUniform3f( U( p, "uMeanCol" ), bc ? bc[ 0 ] : 0.7f, bc ? bc[ 1 ] : 0.7f, bc ? bc[ 2 ] : 0.7f );
	// every declared sampler gets a texture of its type on its own unit
	BindTex( 45, GL_TEXTURE_BUFFER, v.capTex[ 0 ] );
	BindTex( 46, GL_TEXTURE_BUFFER, v.capTex[ 2 ] );
	BindTex( 47, GL_TEXTURE_2D, v.ownerTex );
	BindTex( 48, GL_TEXTURE_2D, v.extTex[ 0 ] );
	BindTex( 49, GL_TEXTURE_2D, v.extTex[ 1 ] );
	BindTex( 50, GL_TEXTURE_2D, v.fieldTex[ 0 ] );
	BindTex( 52, GL_TEXTURE_2D, v.merged ? v.mCountTex : v.countTex );
	BindTex( 53, GL_TEXTURE_2D, v.hpTex[ 0 ] );
	BindTex( 54, GL_TEXTURE_2D, v.merged ? v.mOwnTex : v.ownTex );
	BindTex( 55, GL_TEXTURE_BUFFER, v.listTex );
	BindTex( 56, GL_TEXTURE_2D, v.hpTex[ 1 ] );
	glActiveTexture( GL_TEXTURE0 );
	const GLboolean cullOn = glIsEnabled( GL_CULL_FACE );
	GLint cullMode;
	glGetIntegerv( GL_CULL_FACE_MODE, &cullMode );
	if( smooth )
		glDisable( GL_CULL_FACE );
	else if( plain )
	{
		// front faces (a mirrored model matrix turns the winding round)
		const float* m = worldFromGrid.m;
		const float det = m[ 0 ] * ( m[ 5 ] * m[ 10 ] - m[ 9 ] * m[ 6 ] ) - m[ 4 ] * ( m[ 1 ] * m[ 10 ] - m[ 9 ] * m[ 2 ] ) + m[ 8 ] * ( m[ 1 ] * m[ 6 ] - m[ 5 ] * m[ 2 ] );
		glEnable( GL_CULL_FACE );
		glCullFace( GL_BACK );
		glFrontFace( det < 0.0f ? GL_CW : GL_CCW );
	}
	else
	{
		// the back faces of each proxy: the ray finds the front either way (a mirrored model matrix too), and works with the camera inside
		glEnable( GL_CULL_FACE );
		glCullFace( GL_FRONT );
	}
	glBindVertexArray( voxVao );
	glBindBuffer( GL_DRAW_INDIRECT_BUFFER, v.cmdBuf );
	glDrawArraysIndirect( GL_TRIANGLES, (const void*)16 );
	glBindBuffer( GL_DRAW_INDIRECT_BUFFER, 0 );
	glBindVertexArray( 0 );
	cullOn ? glEnable( GL_CULL_FACE ) : glDisable( GL_CULL_FACE );
	glCullFace( cullMode );
	glFrontFace( GL_CCW );
	glUseProgram( 0 );
}
}
