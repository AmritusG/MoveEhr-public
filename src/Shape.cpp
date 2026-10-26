// Stage 4 shape: shard clustering (off the render thread), the shape uniforms of the triangle-soup stage,
// and Blow To Dust: area-weighted surface points simulated with transform feedback (curl-noise wind),
// which fly off where the mesh dissolves and spring home when it reforms.
#include "Generate.h"
#include "Log.h"
#include "MathUtil.h"
#include "Renderer.h"
#include "Shaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <unordered_map>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

// W26: each shard's radius (fitted units: the farthest triangle centroid from the shard's centroid), for Bounce collisions
void ShardRadii( const std::vector< float >& cen, int count, ShardJob& job )
{
	job.radius.assign( size_t( count ), 0.0f );
	for( size_t t = 0; t < job.triShard.size(); ++t )
	{
		const int s    = job.triShard[ t ];
		const float* c = &job.shardData[ size_t( s ) * 4 ];
		const float d  = std::sqrt( ( cen[ t * 3 ] - c[ 0 ] ) * ( cen[ t * 3 ] - c[ 0 ] ) + ( cen[ t * 3 + 1 ] - c[ 1 ] ) * ( cen[ t * 3 + 1 ] - c[ 1 ] ) +
		                            ( cen[ t * 3 + 2 ] - c[ 2 ] ) * ( cen[ t * 3 + 2 ] - c[ 2 ] ) );
		job.radius[ s ] = std::max( job.radius[ s ], d );
	}
}

// ---- shards: object-space Voronoi of triangle centroids around area-weighted random seeds
// W2 text / logos: shard ids 0..L-1 = the letters (the whole mesh, Shatter Mode Letters), L..L+P-1 = the solid pieces of the
// fractured copy (Pieces; Pieces + Letters: a piece takes its letter's seed, so a letter's pieces share one kick and spin)
void BuildShards( const CpuGeometry& g, int count, int mode, ShardJob& job )
{
	auto t0          = std::chrono::steady_clock::now();
	const size_t tri = g.idx.size() / 3;
	const size_t all = tri + ( g.triGroup.empty() ? 0 : g.fracIdx.size() / 3 );
	std::vector< float > cen( all * 3 ), area( all );
	float mn[ 3 ] = { 1e30f, 1e30f, 1e30f }, mx[ 3 ] = { -1e30f, -1e30f, -1e30f };
	for( size_t t = 0; t < all && !job.cancel.load(); ++t )
	{
		const uint32_t* I = t < tri ? &g.idx[ t * 3 ] : &g.fracIdx[ ( t - tri ) * 3 ];
		const float* a = &g.pos[ I[ 0 ] * 3 ];
		const float* b = &g.pos[ I[ 1 ] * 3 ];
		const float* c = &g.pos[ I[ 2 ] * 3 ];
		float e1[ 3 ], e2[ 3 ], cr[ 3 ];
		for( int k = 0; k < 3; ++k )
		{
			// fitted space: the model spans -1..1
			cen[ t * 3 + k ] = ( ( a[ k ] + b[ k ] + c[ k ] ) / 3.0f - g.fitCenter[ k ] ) * g.fitScale;
			e1[ k ] = b[ k ] - a[ k ], e2[ k ] = c[ k ] - a[ k ];
			if( t < tri )
				mn[ k ] = std::min( mn[ k ], cen[ t * 3 + k ] ), mx[ k ] = std::max( mx[ k ], cen[ t * 3 + k ] );
		}
		Mat4::Cross( e1, e2, cr );
		area[ t ] = std::sqrt( cr[ 0 ] * cr[ 0 ] + cr[ 1 ] * cr[ 1 ] + cr[ 2 ] * cr[ 2 ] ) + 1e-30f;
	}
	if( tri == 0 || job.cancel.load() )
		return;
	if( !g.triGroup.empty() )
	{
		// W21 text / logos: every letter (logo shape) is one shard; W2: + every solid piece of the fractured copy
		const int L = std::max( g.groupCount, 1 ), P = g.fracIdx.empty() ? 0 : g.pieceCount;
		count = L + P;
		job.triShard.resize( all );
		for( size_t t = 0; t < all; ++t )
			job.triShard[ t ] = t < tri ? g.triGroup[ t ] : uint16_t( L + g.fracPiece[ t - tri ] );
		std::vector< double > sum( size_t( count ) * 4, 0.0 );
		std::vector< int > letterOf( size_t( count ), 0 );
		for( size_t t = 0; t < all; ++t )
		{
			const int s = job.triShard[ t ];
			for( int k = 0; k < 3; ++k )
				sum[ s * 4 + k ] += cen[ t * 3 + k ] * area[ t ];
			sum[ s * 4 + 3 ] += area[ t ];
			letterOf[ s ] = g.triGroup[ t ];
		}
		job.shardData.resize( size_t( count ) * 4 );
		for( int s = 0; s < count; ++s )
		{
			for( int k = 0; k < 3; ++k )
				job.shardData[ s * 4 + k ] = sum[ s * 4 + 3 ] > 0 ? float( sum[ s * 4 + k ] / sum[ s * 4 + 3 ] ) : 0.0f;
			job.shardData[ s * 4 + 3 ] = float( s >= L && mode == 2 ? letterOf[ s ] : s ) + 0.5f;
		}
		ShardRadii( cen, count, job );
		job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
		return;
	}
	// seeds: area-weighted random triangle centroids (fixed seed: the same model breaks the same way)
	std::vector< double > cum( tri );
	double acc = 0;
	for( size_t t = 0; t < tri; ++t )
		cum[ t ] = acc += area[ t ];
	std::mt19937 rng( 1234567u + (unsigned)count );
	std::uniform_real_distribution< double > U01( 0.0, 1.0 );
	std::vector< float > seeds( size_t( count ) * 3 );
	for( int s = 0; s < count; ++s )
	{
		size_t t = std::lower_bound( cum.begin(), cum.end(), U01( rng ) * acc ) - cum.begin();
		t        = std::min( t, tri - 1 );
		for( int k = 0; k < 3; ++k )
			seeds[ s * 3 + k ] = cen[ t * 3 + k ];
	}
	// uniform grid over the seeds, cell ~ the mean seed spacing
	float ext[ 3 ], vol = 1;
	for( int k = 0; k < 3; ++k )
		ext[ k ] = std::max( mx[ k ] - mn[ k ], 1e-4f ), vol *= ext[ k ];
	const float cell = std::cbrt( vol / count ) * 1.0f;
	int dim[ 3 ];
	for( int k = 0; k < 3; ++k )
		dim[ k ] = std::max( 1, std::min( 256, (int)std::ceil( ext[ k ] / cell ) ) );
	auto cellOf = [ & ]( const float* p, int* c ) {
		for( int k = 0; k < 3; ++k )
			c[ k ] = std::max( 0, std::min( dim[ k ] - 1, (int)( ( p[ k ] - mn[ k ] ) / cell ) ) );
	};
	std::unordered_map< int, std::vector< int > > grid;
	for( int s = 0; s < count; ++s )
	{
		int c[ 3 ];
		cellOf( &seeds[ s * 3 ], c );
		grid[ ( c[ 2 ] * dim[ 1 ] + c[ 1 ] ) * dim[ 0 ] + c[ 0 ] ].push_back( s );
	}
	job.triShard.resize( tri );
	for( size_t t = 0; t < tri; ++t )
	{
		if( ( t & 0xffff ) == 0 && job.cancel.load() )
			return;
		const float* p = &cen[ t * 3 ];
		int c[ 3 ];
		cellOf( p, c );
		int best    = 0;
		float bestD = 1e30f;
		for( int r = 1; r < 64 && best >= 0; ++r )
		{
			for( int z = c[ 2 ] - r; z <= c[ 2 ] + r; ++z )
				for( int y = c[ 1 ] - r; y <= c[ 1 ] + r; ++y )
					for( int x = c[ 0 ] - r; x <= c[ 0 ] + r; ++x )
					{
						if( x < 0 || y < 0 || z < 0 || x >= dim[ 0 ] || y >= dim[ 1 ] || z >= dim[ 2 ] )
							continue;
						auto it = grid.find( ( z * dim[ 1 ] + y ) * dim[ 0 ] + x );
						if( it == grid.end() )
							continue;
						for( int s : it->second )
						{
							const float* q = &seeds[ s * 3 ];
							float d = ( p[ 0 ] - q[ 0 ] ) * ( p[ 0 ] - q[ 0 ] ) + ( p[ 1 ] - q[ 1 ] ) * ( p[ 1 ] - q[ 1 ] ) + ( p[ 2 ] - q[ 2 ] ) * ( p[ 2 ] - q[ 2 ] );
							if( d < bestD )
								bestD = d, best = s;
						}
					}
			// every seed within r cells is exact once the best distance fits inside the searched shell
			if( bestD < ( r * cell ) * ( r * cell ) )
				break;
		}
		job.triShard[ t ] = (uint16_t)best;
	}
	// shard centroid (area-weighted) + a random seed
	std::vector< double > sum( size_t( count ) * 4, 0.0 );
	for( size_t t = 0; t < tri; ++t )
	{
		const int s = job.triShard[ t ];
		for( int k = 0; k < 3; ++k )
			sum[ s * 4 + k ] += cen[ t * 3 + k ] * area[ t ];
		sum[ s * 4 + 3 ] += area[ t ];
	}
	job.shardData.resize( size_t( count ) * 4 );
	for( int s = 0; s < count; ++s )
	{
		const double w = sum[ s * 4 + 3 ];
		for( int k = 0; k < 3; ++k )
			job.shardData[ s * 4 + k ] = w > 0 ? float( sum[ s * 4 + k ] / w ) : seeds[ s * 3 + k ];
		job.shardData[ s * 4 + 3 ] = float( s ) + 0.5f;
	}
	ShardRadii( cen, count, job );
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

// ---- dust: area-weighted points on the surface, sorted by part (raw position + uv)
void BuildDust( const CpuGeometry& g, int count, DustJob& job )
{
	auto t0          = std::chrono::steady_clock::now();
	const size_t tri = g.idx.size() / 3;
	std::vector< double > cum( tri );
	double acc = 0;
	for( size_t t = 0; t < tri; ++t )
	{
		const float* a = &g.pos[ g.idx[ t * 3 ] * 3 ];
		const float* b = &g.pos[ g.idx[ t * 3 + 1 ] * 3 ];
		const float* c = &g.pos[ g.idx[ t * 3 + 2 ] * 3 ];
		float e1[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, e2[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] }, cr[ 3 ];
		Mat4::Cross( e1, e2, cr );
		cum[ t ] = acc += std::sqrt( cr[ 0 ] * cr[ 0 ] + cr[ 1 ] * cr[ 1 ] + cr[ 2 ] * cr[ 2 ] );
	}
	if( tri == 0 || acc <= 0 )
		return;
	std::mt19937 rng( 7654321u );
	std::uniform_real_distribution< double > U01( 0.0, 1.0 );
	std::vector< std::pair< int, size_t > > picks( count ); // part, triangle
	for( int i = 0; i < count; ++i )
	{
		size_t t = std::min( size_t( std::lower_bound( cum.begin(), cum.end(), U01( rng ) * acc ) - cum.begin() ), tri - 1 );
		int part = 0;
		for( size_t p = 0; p < g.parts.size(); ++p )
			if( t * 3 >= g.parts[ p ][ 0 ] && t * 3 < g.parts[ p ][ 0 ] + g.parts[ p ][ 1 ] )
				part = (int)p;
		picks[ i ] = { part, t };
	}
	std::stable_sort( picks.begin(), picks.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
	job.points.resize( size_t( count ) * 5 );
	job.partRanges.assign( g.parts.size(), { 0u, 0u } );
	for( int i = 0; i < count; ++i )
	{
		const size_t t = picks[ i ].second;
		double r1 = std::sqrt( U01( rng ) ), r2 = U01( rng );
		float wa = float( 1 - r1 ), wb = float( r1 * ( 1 - r2 ) ), wc = float( r1 * r2 );
		const uint32_t ia = g.idx[ t * 3 ], ib = g.idx[ t * 3 + 1 ], ic = g.idx[ t * 3 + 2 ];
		float* o = &job.points[ size_t( i ) * 5 ];
		for( int k = 0; k < 3; ++k )
			o[ k ] = g.pos[ ia * 3 + k ] * wa + g.pos[ ib * 3 + k ] * wb + g.pos[ ic * 3 + k ] * wc;
		uint32_t tri = (uint32_t)t; // MoveEhr W1: the point as a ref too (its posed origin follows an animated model)
		float tf;
		std::memcpy( &tf, &tri, 4 );
		job.refs.insert( job.refs.end(), { tf, wb, wc } );
		for( int k = 0; k < 2; ++k )
			o[ 3 + k ] = g.uv[ ia * 2 + k ] * wa + g.uv[ ib * 2 + k ] * wb + g.uv[ ic * 2 + k ] * wc;
		auto& pr = job.partRanges[ picks[ i ].first ];
		if( pr[ 1 ] == 0 )
			pr[ 0 ] = (uint32_t)i;
		pr[ 1 ]++;
	}
	// the Dust transition's key: the dust noise's quantiles over these points (as the shaders read it)
	std::vector< float > n( (size_t)count );
	for( int i = 0; i < count && !job.cancel.load(); ++i )
	{
		const float* o = &job.points[ size_t( i ) * 5 ];
		float f[ 3 ];
		for( int k = 0; k < 3; ++k )
			f[ k ] = ( o[ k ] - g.fitCenter[ k ] ) * g.fitScale * 3.0f + 17.0f;
		n[ i ] = FbmF( f[ 0 ], f[ 1 ], f[ 2 ] );
	}
	std::sort( n.begin(), n.end() );
	for( int q = 0; q <= 16; ++q )
		job.keyQ[ q ] = n[ std::min< size_t >( n.size() - 1, size_t( q ) * ( n.size() - 1 ) / 16 ) ];
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}

// Same Hash3 / Noise3 / Fbm as the main fragment shader, so a point leaves exactly where the mesh dissolves.
const char* kDustNoise = R"GLSL(
float Hash3( vec3 p )
{
	p = fract( p * 0.3183099 + 0.1 );
	p *= 17.0;
	return fract( p.x * p.y * p.z * ( p.x + p.y + p.z ) );
}
float Noise3( vec3 x )
{
	vec3 i = floor( x );
	vec3 f = fract( x );
	f      = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( mix( Hash3( i + vec3( 0, 0, 0 ) ), Hash3( i + vec3( 1, 0, 0 ) ), f.x ),
	                 mix( Hash3( i + vec3( 0, 1, 0 ) ), Hash3( i + vec3( 1, 1, 0 ) ), f.x ), f.y ),
	            mix( mix( Hash3( i + vec3( 0, 0, 1 ) ), Hash3( i + vec3( 1, 0, 1 ) ), f.x ),
	                 mix( Hash3( i + vec3( 0, 1, 1 ) ), Hash3( i + vec3( 1, 1, 1 ) ), f.x ), f.y ),
	            f.z );
}
float Fbm( vec3 p )
{
	float v = 0.0, a = 0.5;
	for( int i = 0; i < 4; ++i )
	{
		v += a * Noise3( p );
		p *= 2.03;
		a *= 0.5;
	}
	return v / 0.9375;
}
)GLSL";

const char* kDustSimVS = R"GLSL(
layout( location = 0 ) in vec3 aOrigin; // raw model units
#ifdef ANIM
layout( location = 4 ) in vec3 aRest; // MoveEhr W1: the bind origin (aOrigin = the posed one): the release noise stays on the surface
#define NOISE_ORIGIN aRest
#else
#define NOISE_ORIGIN aOrigin
#endif
layout( location = 1 ) in vec2 aUV;
layout( location = 2 ) in vec3 aPos;    // world
layout( location = 3 ) in vec3 aVel;
out vec3 oPos;
out vec3 oVel;
uniform mat4 uModel;
uniform vec3 uFitCenter;
uniform float uFitScale;
uniform float uCut;
uniform float uDt;
uniform float uCurlY; // the wind field's drift, wrapped at 64 (NP tiles every 64)
uniform float uWind;
uniform float uReturn;
uniform int uReset; // 1 = every point at home
uniform vec3 uCenter;
// Noise3 on a lattice that repeats every 64 cells on each axis
float NP( vec3 x )
{
	vec3 i = floor( x );
	vec3 f = fract( x );
	f      = f * f * ( 3.0 - 2.0 * f );
	vec3 a = mod( i, 64.0 ), b = mod( i + 1.0, 64.0 );
	return mix( mix( mix( Hash3( vec3( a.x, a.y, a.z ) ), Hash3( vec3( b.x, a.y, a.z ) ), f.x ),
	                 mix( Hash3( vec3( a.x, b.y, a.z ) ), Hash3( vec3( b.x, b.y, a.z ) ), f.x ), f.y ),
	            mix( mix( Hash3( vec3( a.x, a.y, b.z ) ), Hash3( vec3( b.x, a.y, b.z ) ), f.x ),
	                 mix( Hash3( vec3( a.x, b.y, b.z ) ), Hash3( vec3( b.x, b.y, b.z ) ), f.x ), f.y ),
	            f.z );
}
vec3 VN( vec3 q ) { return vec3( NP( q ), NP( q + vec3( 31.4, 7.1, 3.3 ) ), NP( q + vec3( 11.7, 71.1, 23.9 ) ) ); }
vec3 Curl( vec3 q )
{
	const float e = 0.1;
	vec3 dx = ( VN( q + vec3( e, 0, 0 ) ) - VN( q - vec3( e, 0, 0 ) ) ) / ( 2.0 * e );
	vec3 dy = ( VN( q + vec3( 0, e, 0 ) ) - VN( q - vec3( 0, e, 0 ) ) ) / ( 2.0 * e );
	vec3 dz = ( VN( q + vec3( 0, 0, e ) ) - VN( q - vec3( 0, 0, e ) ) ) / ( 2.0 * e );
	return vec3( dy.z - dz.y, dz.x - dx.z, dx.y - dy.x );
}
void main()
{
	vec3 home     = ( uModel * vec4( aOrigin, 1.0 ) ).xyz;
	vec3 f        = ( NOISE_ORIGIN - uFitCenter ) * uFitScale;
	bool released = Fbm( f * 3.0 + 17.0 ) < uCut * 1.08 - 0.04;
	vec3 pos = aPos, vel = aVel;
	if( uReset == 1 )
	{
		pos = home;
		vel = vec3( 0.0 );
	}
	else if( released )
	{
		float away = length( pos - home );
		vec3 out_  = normalize( home - uCenter + 1e-4 ) * 1.5 * max( 0.0, 1.0 - away * 2.0 );
		vec3 acc   = Curl( pos * 1.2 + vec3( 0.0, uCurlY, 0.0 ) ) * ( 0.8 + uWind ) + vec3( 0.6, 0.25, 0.1 ) * uWind + out_ - vel * 0.6;
		vel += acc * uDt;
		pos += vel * uDt;
	}
	else
	{
		pos += ( home - pos ) * ( 1.0 - exp( -uDt * uReturn ) );
		vel *= 0.5;
		if( length( home - pos ) < 1e-3 )
			pos = home;
	}
	oPos = pos;
	oVel = vel;
}
)GLSL";

const char* kDustSimFS = R"GLSL(
out vec4 fragColor;
void main() { fragColor = vec4( 0.0 ); }
)GLSL";

const char* kDustDrawVS = R"GLSL(
layout( location = 0 ) in vec3 aOrigin;
#ifdef ANIM
layout( location = 4 ) in vec3 aRest; // MoveEhr W1: the bind origin: the Dust transition's key stays on the surface
#define NOISE_ORIGIN aRest
#else
#define NOISE_ORIGIN aOrigin
#endif
layout( location = 1 ) in vec2 aUV;
layout( location = 2 ) in vec3 aPos;
uniform mat4 uModel;
uniform mat4 uViewProj;
uniform float uPointSize;
out vec2 vUV;
out float vFade;
#ifdef TRANS
// W16 Dust transition: no simulation, every point sits where the slider puts it (scrubbing back retraces it).
// Key k = the dust noise through its CDF on THIS model (uKeyQ, measured over the points: k is even over 0..1 whatever
// the surface, so as much of it is in flight at 0.3 as at 0.7; SetShapeUniforms inverts it for the mesh), flight u over the point's own
// stretch of the slider, eased at both ends; the point shrinks away over the far half of its flight.
uniform vec3 uFitCenter;
uniform float uFitScale;
uniform int uSide; // 1 the outgoing model, 2 the incoming one
uniform float uTransE, uStagger, uEase, uSpread, uWind;
uniform float uKeyQ[ 17 ];
void main()
{
	vec3 f  = ( NOISE_ORIGIN - uFitCenter ) * uFitScale;
	float n = Fbm( f * 3.0 + 17.0 ), k = 1.0;
	for( int i = 1; i < 17; ++i )
		if( n < uKeyQ[ i ] )
		{
			k = ( float( i - 1 ) + clamp( ( n - uKeyQ[ i - 1 ] ) / max( uKeyQ[ i ] - uKeyQ[ i - 1 ], 1e-6 ), 0.0, 1.0 ) ) / 16.0;
			break;
		}
	float u = clamp( ( uTransE - uStagger * k ) / ( 1.0 - uStagger ), 0.0, 1.0 );
	float a = uSide == 1 ? u : 1.0 - u;
	float s = mix( a, a * a * ( 3.0 - 2.0 * a ), uEase );
	vec3 h  = fract( sin( vec3( 12.9898, 78.233, 37.719 ) * dot( NOISE_ORIGIN, vec3( 1.13, 2.71, 3.97 ) ) ) * 43758.5453 ) - 0.5;
	vec3 sw = vec3( Noise3( f * 1.7 + 3.1 ), Noise3( f * 1.7 + 11.3 ), Noise3( f * 1.7 + 23.9 ) ) - 0.5;
	vec3 go = normalize( f + h * 0.9 + vec3( 0.0, 0.0, 0.35 ) ) * ( 0.7 + 0.6 * h.x ) + sw * 1.6 + vec3( 0.6, 0.25, 0.1 ) * uWind;
	vec3 p  = f + go * ( 2.4 * uSpread * s );
	vUV     = aUV;
	vFade   = a > 0.0 ? smoothstep( 0.0, 0.01, length( p - f ) ) * ( 1.0 - uEase * smoothstep( 0.5, 1.0, a ) ) : 0.0;
	vec4 c  = uViewProj * uModel * vec4( p / uFitScale + uFitCenter, 1.0 );
	gl_Position  = vFade > 0.0 ? c : vec4( 2.0, 2.0, 2.0, 1.0 ); // at home: the mesh shows it
	gl_PointSize = uPointSize * vFade / max( c.w * 0.25, 0.25 );
}
#else
void main()
{
	vec3 home = ( uModel * vec4( aOrigin, 1.0 ) ).xyz;
	float d   = length( aPos - home );
	vUV       = aUV;
	vFade     = smoothstep( 0.0, 0.02, d );
	vec4 c    = uViewProj * vec4( aPos, 1.0 );
	gl_Position  = d > 1e-3 ? c : vec4( 2.0, 2.0, 2.0, 1.0 ); // still on the surface: the mesh shows it
	gl_PointSize = uPointSize * vFade / max( c.w * 0.25, 0.25 );
}
#endif
)GLSL";

const char* kDustDrawFS = R"GLSL(
in vec2 vUV;
in float vFade;
out vec4 fragColor;
uniform sampler2D uBase;
uniform int uHasBase;
uniform vec4 uBaseFactor;
uniform vec3 uLight;
void main()
{
	vec2 q = gl_PointCoord * 2.0 - 1.0;
	if( dot( q, q ) > 1.0 )
		discard;
	vec4 b    = uBaseFactor * ( uHasBase == 1 ? texture( uBase, vUV ) : vec4( 1.0 ) );
	vec3 c    = ToDisplay( b.rgb * uLight * ( 1.0 - 0.35 * dot( q, q ) ) );
	fragColor = vec4( c, 1.0 );
}
)GLSL";
}

// W26: area-weighted surface points (raw xyz + uv, by part) for the firefly swarm's homes: the dust sampler
void SampleSurface( const CpuGeometry& g, int count, DustJob& job ) { BuildDust( g, count, job ); }

bool Renderer::InitShape( std::string& error )
{
	// transform feedback program: varyings must be declared before linking (MoveEhr W1: + its ANIM twin, the posed origins)
	auto buildSim = [ & ]( const char* defines, GLuint& prog ) -> bool {
		std::string vs = std::string( "#version 410 core\n" ) + defines + kDustNoise + kDustSimVS;
		std::string fs = std::string( "#version 410 core\n" ) + kDustSimFS;
		GLuint v = glCreateShader( GL_VERTEX_SHADER ), f = glCreateShader( GL_FRAGMENT_SHADER );
		const char* pv = vs.c_str();
		const char* pf = fs.c_str();
		glShaderSource( v, 1, &pv, nullptr );
		glShaderSource( f, 1, &pf, nullptr );
		glCompileShader( v );
		glCompileShader( f );
		GLint ok = 0;
		glGetShaderiv( v, GL_COMPILE_STATUS, &ok );
		if( !ok )
		{
			char log[ 2048 ];
			glGetShaderInfoLog( v, sizeof( log ), nullptr, log );
			error = std::string( "dust sim compile failed: " ) + log;
			Log( "%s", error.c_str() );
			return false;
		}
		prog = glCreateProgram();
		glAttachShader( prog, v );
		glAttachShader( prog, f );
		const char* varyings[ 2 ] = { "oPos", "oVel" };
		glTransformFeedbackVaryings( prog, 2, varyings, GL_INTERLEAVED_ATTRIBS );
		glLinkProgram( prog );
		glDeleteShader( v ), glDeleteShader( f );
		glGetProgramiv( prog, GL_LINK_STATUS, &ok );
		if( !ok )
		{
			char log[ 2048 ];
			glGetProgramInfoLog( prog, sizeof( log ), nullptr, log );
			error = std::string( "dust sim link failed: " ) + log;
			Log( "%s", error.c_str() );
			return false;
		}
		return true;
	};
	if( !buildSim( "", progDustSim ) || !buildSim( "#define ANIM\n", progDustSimA ) )
		return false;
	// 1-element buffers bound to the shard units until the real ones exist (no "unloadable texture" draws)
	glGenBuffers( 1, &dummyShardBuf );
	glBindBuffer( GL_ARRAY_BUFFER, dummyShardBuf );
	const float zero[ 4 ] = { 0, 0, 0, 0 };
	glBufferData( GL_ARRAY_BUFFER, sizeof( zero ), zero, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glGenTextures( 1, &dummyShardOf ), glGenTextures( 1, &dummyShardData );
	glBindTexture( GL_TEXTURE_BUFFER, dummyShardOf );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_R16UI, dummyShardBuf );
	glBindTexture( GL_TEXTURE_BUFFER, dummyShardData );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, dummyShardBuf );
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	{
		// W26 Shatter Physics Bounce: the soup stage reading the recorded flights (render + shadow)
		const std::string mainFS = std::string( kLightLib ) + kMainFS;
		progSoupB       = Link( std::string( "#define BOUNCE\n" ) + kSoupVS, mainFS, error );
		progShadowSoupB = Link( std::string( "#define BOUNCE\n" ) + kSoupVS, "#define SHADOW_PASS\n" + mainFS, error );
		if( !progSoupB || !progShadowSoupB )
			return false;
		for( GLuint p : { progSoupB, progShadowSoupB } )
		{
			glUseProgram( p );
			char name[ 16 ];
			for( int i = 0; i < CH_COUNT; ++i )
			{
				snprintf( name, sizeof( name ), "uTex%d", i );
				glUniform1i( U( p, name ), i );
			}
			glUniform1i( U( p, "uInputTex" ), 8 );
			glUniform1i( U( p, "uIdxBuf" ), 9 );
			glUniform1i( U( p, "uVtxBuf" ), 10 );
			glUniform1i( U( p, "uShardPath" ), 24 );
			BindShadowSamplers( p );
		}
	}
	progDustDraw = Link( kDustDrawVS, std::string( kToneLib ) + kDustDrawFS, error );
	if( !progDustDraw )
		return false;
	glUseProgram( progDustDraw );
	glUniform1i( U( progDustDraw, "uBase" ), 0 );
	progDustTrans = Link( std::string( "#define TRANS\n" ) + kDustNoise + kDustDrawVS, std::string( kToneLib ) + kDustDrawFS, error );
	if( !progDustTrans )
		return false;
	glUseProgram( progDustTrans );
	glUniform1i( U( progDustTrans, "uBase" ), 0 );
	// MoveEhr W1: the same draws on the posed origins (an animated side)
	progDustDrawA  = Link( std::string( "#define ANIM\n" ) + kDustDrawVS, std::string( kToneLib ) + kDustDrawFS, error );
	progDustTransA = Link( std::string( "#define ANIM\n#define TRANS\n" ) + kDustNoise + kDustDrawVS, std::string( kToneLib ) + kDustDrawFS, error );
	if( !progDustDrawA || !progDustTransA )
		return false;
	for( GLuint p : { progDustDrawA, progDustTransA } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uBase" ), 0 );
	}
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseShape()
{
	for( ShardSlot& sl : shards )
	{
		if( sl.pathBuf )
			glDeleteBuffers( 1, &sl.pathBuf ), sl.pathBuf = 0;
		if( sl.pathTex )
			glDeleteTextures( 1, &sl.pathTex ), sl.pathTex = 0;
		sl.pathKey = 0;
	}
	for( GLuint* p : { &progDustSim, &progDustDraw, &progDustTrans, &progDustSimA, &progDustDrawA, &progDustTransA, &progSoupB, &progShadowSoupB } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( LetterSlot& L : letters )
	{
		for( GLuint* b : { &L.ofBuf, &L.xfBuf } )
			if( *b )
				glDeleteBuffers( 1, b ), *b = 0;
		for( GLuint* t : { &L.ofTex, &L.xfTex } )
			if( *t )
				glDeleteTextures( 1, t ), *t = 0;
		L = LetterSlot();
	}
	for( GLuint* t : { &dummyShardOf, &dummyShardData } )
		if( *t )
			glDeleteTextures( 1, t ), *t = 0;
	if( dummyShardBuf )
		glDeleteBuffers( 1, &dummyShardBuf ), dummyShardBuf = 0;
	for( ShardSlot& sh : shards )
	{
		for( GLuint* t : { &sh.ofTex, &sh.dataTex, &sh.rankTex, &sh.triTex, &sh.chunkTex, &sh.rangeTex, &sh.partTex, &sh.pivotTex } )
			if( *t )
				glDeleteTextures( 1, t ), *t = 0;
		for( GLuint* b : { &sh.ofBuf, &sh.dataBuf, &sh.rankBuf, &sh.triBuf, &sh.chunkBuf, &sh.rangeBuf, &sh.partBuf, &sh.pivotBuf } )
			if( *b )
				glDeleteBuffers( 1, b ), *b = 0;
		if( sh.job )
			sh.job->cancel.store( true );
		sh = ShardSlot();
	}
	for( DustSlot& d : dust )
	{
		for( GLuint* b : { &d.origin, &d.state[ 0 ], &d.state[ 1 ], &d.refBuf, &d.posedBuf } )
			if( *b )
				glDeleteBuffers( 1, b ), *b = 0;
		for( GLuint* v : { &d.simVAO[ 0 ], &d.simVAO[ 1 ], &d.drawVAO[ 0 ], &d.drawVAO[ 1 ], &d.simVAOp[ 0 ], &d.simVAOp[ 1 ], &d.drawVAOp[ 0 ], &d.drawVAOp[ 1 ] } )
			if( *v )
				glDeleteVertexArrays( 1, v ), *v = 0;
		if( d.job )
			d.job->cancel.store( true );
		d = DustSlot();
	}
}

// W21: the letter id per triangle (once per model) and this frame's per-letter transforms, as texture buffers
bool Renderer::UpdateLetters( GpuModel* model, const std::vector< float >& xf )
{
	if( !model || !model->geometry || model->geometry->triGroup.empty() || xf.size() < size_t( model->geometry->groupCount ) * 12 )
		return false;
	LetterSlot& L = letters[ SlotOf( model ) ];
	if( !L.ofBuf )
	{
		glGenBuffers( 1, &L.ofBuf ), glGenBuffers( 1, &L.xfBuf );
		glGenTextures( 1, &L.ofTex ), glGenTextures( 1, &L.xfTex );
	}
	GLint prevArray = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevArray );
	if( L.key != model->serial )
	{
		const std::vector< uint16_t >& g = model->geometry->triGroup;
		glBindBuffer( GL_ARRAY_BUFFER, L.ofBuf );
		glBufferData( GL_ARRAY_BUFFER, g.size() * sizeof( uint16_t ), g.data(), GL_STATIC_DRAW );
		glBindTexture( GL_TEXTURE_BUFFER, L.ofTex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R16UI, L.ofBuf );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		L.key = model->serial;
	}
	glBindBuffer( GL_ARRAY_BUFFER, L.xfBuf );
	glBufferData( GL_ARRAY_BUFFER, xf.size() * sizeof( float ), xf.data(), GL_STREAM_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, prevArray );
	// Apple GL: a texture buffer keeps the storage it was attached to; re-attach after every re-specification
	glBindTexture( GL_TEXTURE_BUFFER, L.xfTex );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, L.xfBuf );
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	return true;
}

GLuint Renderer::LettersProgram( GLuint p ) const
{
	return p == progProjSoup ? progProjSoupL : p == progSoup ? progSoupL : p == progSoupI ? progSoupLI : p == progShadowSoup ? progShadowSoupL : p == progShadowSoupI ? progShadowSoupLI : p;
}

uint64_t Renderer::ShardKey( const GpuModel* model, const ShapeState& s )
{
	// W2: the mode only matters for a fractured text / logo (Pieces + Letters gives the pieces their letter's seed)
	const uint64_t mode = model && model->hasPieces && s.shatterMode == 2 ? 1 : 0;
	return model ? ( model->serial * 4096 + (uint64_t)s.shardCount ) * 2 + mode : 0;
}

// Starts / collects the shard clustering for (model, count); uploads the result as texture buffers.
void Renderer::UpdateShards( GpuModel* model, int count, int mode )
{
	if( !model || !model->geometry )
		return;
	ShardSlot& sh      = shards[ SlotOf( model ) ];
	ShapeState ks;
	ks.shardCount = count, ks.shatterMode = mode;
	const uint64_t key = ShardKey( model, ks );
	if( key != sh.key && ( !sh.job || sh.job->key != key ) )
	{
		if( sh.job )
			sh.job->cancel.store( true );
		auto job = std::make_shared< ShardJob >();
		job->key = key;
		auto geo = model->geometry;
		sh.job   = job;
		std::thread( [ job, geo, count, mode ]() {
			BuildShards( *geo, count, mode, *job );
			job->done.store( true );
		} ).detach();
	}
	if( sh.job && sh.job->done.load() )
	{
		auto job = sh.job;
		sh.job.reset();
		if( job->triShard.empty() || job->cancel.load() )
			return;
		if( !sh.ofBuf )
		{
			glGenBuffers( 1, &sh.ofBuf ), glGenBuffers( 1, &sh.dataBuf );
			glGenTextures( 1, &sh.ofTex ), glGenTextures( 1, &sh.dataTex );
		}
		GLint prevArray = 0;
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevArray );
		glBindBuffer( GL_ARRAY_BUFFER, sh.ofBuf );
		glBufferData( GL_ARRAY_BUFFER, job->triShard.size() * sizeof( uint16_t ), job->triShard.data(), GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, sh.dataBuf );
		glBufferData( GL_ARRAY_BUFFER, job->shardData.size() * sizeof( float ), job->shardData.data(), GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, prevArray );
		glBindTexture( GL_TEXTURE_BUFFER, sh.ofTex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R16UI, sh.ofBuf );
		glBindTexture( GL_TEXTURE_BUFFER, sh.dataTex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, sh.dataBuf );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		sh.key    = job->key;
		sh.data   = job->shardData; // W26 Bounce: the CPU flight starts from these
		sh.radius = job->radius;
		sh.pathKey = 0;
		{
			// W6 Pieces transition: the shards ranked bottom-up (centroid height + a little jitter); texel i = (the rank of shard i,
			// 0..1; the shard of rank i): a shard flies to the other model's shard of the same rank
			const int n = int( job->shardData.size() / 4 );
			std::vector< int > order( static_cast< size_t >( n ) );
			for( int i = 0; i < n; ++i )
				order[ size_t( i ) ] = i;
			auto key = [ & ]( int i ) { return job->shardData[ size_t( i ) * 4 + 1 ] + 0.08f * ( std::fmod( std::fabs( std::sin( float( i ) * 12.9898f ) * 43758.5453f ), 1.0f ) - 0.5f ); };
			std::stable_sort( order.begin(), order.end(), [ & ]( int a, int b ) { return key( a ) < key( b ); } );
			std::vector< float > rk( size_t( n ) * 2 );
			for( int r = 0; r < n; ++r )
				rk[ size_t( order[ size_t( r ) ] ) * 2 ] = ( float( r ) + 0.5f ) / float( n ), rk[ size_t( r ) * 2 + 1 ] = float( order[ size_t( r ) ] );
			if( !sh.rankBuf )
				glGenBuffers( 1, &sh.rankBuf ), glGenTextures( 1, &sh.rankTex );
			glBindBuffer( GL_ARRAY_BUFFER, sh.rankBuf );
			glBufferData( GL_ARRAY_BUFFER, std::max< size_t >( 8, rk.size() * sizeof( float ) ), rk.data(), GL_STATIC_DRAW );
			glBindBuffer( GL_ARRAY_BUFFER, prevArray );
			glBindTexture( GL_TEXTURE_BUFFER, sh.rankTex );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_RG32F, sh.rankBuf ); // after glBufferData (Apple GL)
			glBindTexture( GL_TEXTURE_BUFFER, 0 );
			sh.count = n;
		}
		if( model->anim ) // MoveEhr W2 A3: an animated model's shards get posed pivots (the triangles by shard)
			UploadShardCsr( sh, job->triShard, model->geometry->idx.size() / 3, int( job->shardData.size() / 4 ) );
		if( LogVerbose() )
			Log( "SHARDS %d for %s in %.0f ms (off the render thread)", count, LogName( model->path ).c_str(), job->ms );
	}
}

namespace
{
// the soup stage's Hash3v (sin-hash), on the CPU: a shard's launch must match the analytic flight's
void Hash3vF( float n, float* o )
{
	const float a[ 3 ] = { n, n + 1.7f, n + 3.3f }, m[ 3 ] = { 12.9898f, 78.233f, 37.719f };
	for( int k = 0; k < 3; ++k )
	{
		const float v = std::sin( a[ k ] * m[ k ] ) * 43758.5453f;
		o[ k ]        = v - std::floor( v );
	}
}
}

// W26 Stage 9: Shatter Physics Bounce. On a new shatter (or a change of the dials) every shard's flight is simulated once on
// the CPU in FITTED space: the analytic launch (the same velocity and spin), gravity (world down, in fitted space), the floor
// plane and the wall when the catcher is a wall (world planes taken into fitted space with this frame's model matrix), each
// shard a sphere of half its radius: restitution (Bounciness), tangential friction (Floor Friction) that rolls it, rest when
// slow. 241 frames (4 s at 60 Hz) of offset + rotation per shard go to a texture buffer the BOUNCE soup programs read at the
// flight time: Reform runs the time back, so it retraces the flight; at time 0 the shatter code does not run at all.
void Renderer::UpdateBounce( const FrameState& fs, GpuModel* model )
{
	const ShapeState& sh = fs.shape;
	if( !sh.bounce || sh.shatterT <= 0.0f || !model )
		return;
	ShardSlot& sl = shards[ SlotOf( model ) ];
	if( sl.data.empty() || sl.key != ShardKey( model, sh ) )
		return;
	uint64_t key = sh.shatterGen;
	for( float v : { sh.bounciness, sh.friction, sh.shatterForce, sh.shatterGravity, sh.shardSpin } )
	{
		uint32_t b;
		std::memcpy( &b, &v, 4 );
		key = key * 1000003u ^ b;
	}
	key = key * 31 + sl.key;
	if( key == sl.pathKey )
		return;
	sl.pathKey = key;
	const int n = (int)( sl.data.size() / 4 ), NF = 241;
	// fitted -> world: w = A f + b, A = M3 / fitScale, b = M * fitCenter
	Mat4 M;
	std::memcpy( M.m, fs.model, sizeof( M.m ) );
	float A[ 9 ], b[ 3 ];
	for( int c = 0; c < 3; ++c )
		for( int r = 0; r < 3; ++r )
			A[ c * 3 + r ] = M.m[ c * 4 + r ] / std::max( model->fitScale, 1e-12f );
	M.Point( model->fitCenter, b );
	// world planes -> fitted: n_f = A^T n, d_f = n.b + d (normalised)
	struct Plane
	{
		float n[ 3 ], d;
	};
	std::vector< Plane > planes;
	auto addPlane = [ & ]( const float* wn, float wd ) {
		Plane p;
		float len = 0;
		for( int c = 0; c < 3; ++c )
			p.n[ c ] = A[ c * 3 ] * wn[ 0 ] + A[ c * 3 + 1 ] * wn[ 1 ] + A[ c * 3 + 2 ] * wn[ 2 ], len += p.n[ c ] * p.n[ c ];
		len = std::sqrt( len );
		if( len < 1e-9f )
			return;
		p.d = ( wn[ 0 ] * b[ 0 ] + wn[ 1 ] * b[ 1 ] + wn[ 2 ] * b[ 2 ] + wd ) / len;
		for( float& c : p.n )
			c /= len;
		planes.push_back( p );
	};
	const float floorN[ 3 ] = { 0, 1, 0 };
	addPlane( floorN, -fs.post.floorY );
	if( fs.post.catchPlane[ 2 ] > 0.5f ) // the catcher is a wall
		addPlane( fs.post.catchPlane, fs.post.catchPlane[ 3 ] );
	float g[ 3 ];
	for( int c = 0; c < 3; ++c ) // world down in fitted space (A is a scaled rotation: its transpose turns world into fitted)
		g[ c ] = -A[ c * 3 + 1 ];
	Mat4::Norm( g );
	for( float& c : g )
		c *= sh.shatterGravity;
	std::vector< float > path( size_t( n ) * NF * 8 );
	const float dt = 1.0f / 120.0f; // two steps per frame
	for( int i = 0; i < n; ++i )
	{
		const float* sd = &sl.data[ size_t( i ) * 4 ];
		float h[ 3 ], dir[ 3 ], ax[ 3 ];
		Hash3vF( sd[ 3 ] * 17.31f, h );
		for( int k = 0; k < 3; ++k )
			dir[ k ] = sd[ k ] + ( h[ k ] - 0.5f ) * 0.9f + ( k == 2 ? 0.35f : 0.0f );
		Mat4::Norm( dir );
		Hash3vF( sd[ 3 ] * 5.13f + 2.0f, ax );
		for( float& c : ax )
			c -= 0.5f - 1e-4f;
		Mat4::Norm( ax );
		float p[ 3 ] = {}, v[ 3 ], w[ 3 ], q[ 4 ] = { 0, 0, 0, 1 };
		const float spin = sh.shardSpin * ( 0.4f + h[ 1 ] );
		for( int k = 0; k < 3; ++k )
			v[ k ] = dir[ k ] * sh.shatterForce * ( 0.5f + h[ 0 ] ), w[ k ] = ax[ k ] * spin;
		const float r = std::max( 0.02f, 0.5f * ( i < (int)sl.radius.size() ? sl.radius[ i ] : 0.1f ) );
		for( int f = 0; f < NF; ++f )
		{
			float* o = &path[ ( size_t( i ) * NF + f ) * 8 ];
			o[ 0 ] = p[ 0 ], o[ 1 ] = p[ 1 ], o[ 2 ] = p[ 2 ], o[ 3 ] = 0.0f;
			o[ 4 ] = q[ 0 ], o[ 5 ] = q[ 1 ], o[ 6 ] = q[ 2 ], o[ 7 ] = q[ 3 ];
			for( int sub = 0; sub < 2; ++sub )
			{
				for( int k = 0; k < 3; ++k )
					v[ k ] += g[ k ] * dt, p[ k ] += v[ k ] * dt;
				bool contact = false;
				for( const Plane& pl : planes )
				{
					const float c[ 3 ] = { sd[ 0 ] + p[ 0 ], sd[ 1 ] + p[ 1 ], sd[ 2 ] + p[ 2 ] };
					const float dist   = pl.n[ 0 ] * c[ 0 ] + pl.n[ 1 ] * c[ 1 ] + pl.n[ 2 ] * c[ 2 ] + pl.d;
					if( dist >= r )
						continue;
					contact = true;
					for( int k = 0; k < 3; ++k )
						p[ k ] += pl.n[ k ] * ( r - dist );
					const float vn = pl.n[ 0 ] * v[ 0 ] + pl.n[ 1 ] * v[ 1 ] + pl.n[ 2 ] * v[ 2 ];
					if( vn < 0.0f )
					{
						float vt[ 3 ];
						for( int k = 0; k < 3; ++k )
							vt[ k ] = v[ k ] - pl.n[ k ] * vn;
						// restitution on the normal; friction on the tangent, which also sets it rolling
						const float e = vn < -0.3f ? sh.bounciness : 0.0f;
						float roll[ 3 ];
						Mat4::Cross( pl.n, vt, roll );
						for( int k = 0; k < 3; ++k )
							v[ k ] = vt[ k ] * ( 1.0f - sh.friction ) - pl.n[ k ] * vn * e, w[ k ] = w[ k ] * 0.6f + roll[ k ] / r * 0.4f * ( 1.0f - sh.friction );
					}
				}
				if( contact )
				{
					// resting contact: rolling / sliding friction over time
					const float k = std::exp( -dt * ( 1.0f + 6.0f * sh.friction ) );
					for( int c = 0; c < 3; ++c )
						v[ c ] *= ( std::fabs( v[ c ] ) < 0.02f ? 0.0f : 1.0f ) * ( c == 1 ? 1.0f : k ), w[ c ] *= k;
				}
				// q += 0.5 (w, 0) q dt, renormalised
				const float wq[ 4 ] = { w[ 0 ], w[ 1 ], w[ 2 ], 0.0f };
				float dq[ 4 ] = { wq[ 3 ] * q[ 0 ] + wq[ 0 ] * q[ 3 ] + wq[ 1 ] * q[ 2 ] - wq[ 2 ] * q[ 1 ], wq[ 3 ] * q[ 1 ] - wq[ 0 ] * q[ 2 ] + wq[ 1 ] * q[ 3 ] + wq[ 2 ] * q[ 0 ],
				                  wq[ 3 ] * q[ 2 ] + wq[ 0 ] * q[ 1 ] - wq[ 1 ] * q[ 0 ] + wq[ 2 ] * q[ 3 ], wq[ 3 ] * q[ 3 ] - wq[ 0 ] * q[ 0 ] - wq[ 1 ] * q[ 1 ] - wq[ 2 ] * q[ 2 ] };
				float ql = 0;
				for( int k = 0; k < 4; ++k )
					q[ k ] += 0.5f * dq[ k ] * dt, ql += q[ k ] * q[ k ];
				ql = 1.0f / std::sqrt( ql );
				for( float& c : q )
					c *= ql;
			}
		}
	}
	if( !sl.pathBuf )
		glGenBuffers( 1, &sl.pathBuf ), glGenTextures( 1, &sl.pathTex );
	GLint prev = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prev );
	glBindBuffer( GL_ARRAY_BUFFER, sl.pathBuf );
	glBufferData( GL_ARRAY_BUFFER, path.size() * sizeof( float ), path.data(), GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, prev );
	glBindTexture( GL_TEXTURE_BUFFER, sl.pathTex );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, sl.pathBuf ); // re-attach after every glBufferData (Apple GL)
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	if( LogVerbose() )
		Log( "BOUNCE flights for %d shards (%zu planes, gen %u)", n, planes.size(), sh.shatterGen );
}

namespace
{
// Dust transition: key kT -> the noise threshold of the mesh test (Fbm < dustCut * 1.08 - 0.04), through the quantiles
float TransDustCut( float kT, const float* q )
{
	float th = 2.0f;
	if( kT <= 0.0f )
		th = -1.0f;
	else if( kT < 1.0f )
	{
		const float x = kT * 16.0f;
		const int i   = std::min( 15, (int)x );
		th            = q[ i ] + ( q[ i + 1 ] - q[ i ] ) * ( x - float( i ) );
	}
	return std::max( 1e-6f, ( th + 0.04f ) / 1.08f );
}
}

void Renderer::SetShapeUniforms( GLuint p, const FrameState& fs, GpuModel* model )
{
	const ShapeState& s = fs.shape;
	glUniform1i( U( p, "uShapeOn" ), s.on ? 1 : 0 );
	// W26 lava cracks (only the CRACKS / CRACK_PASS programs declare these)
	const LavaState& lv = fs.lava;
	glUniform1f( U( p, "uCrackScale" ), lv.scale );
	glUniform3fv( U( p, "uCrackSeed" ), 1, lv.seed );
	glUniform1f( U( p, "uCrackFront" ), lv.front );
	glUniform1f( U( p, "uSplit" ), lv.split );
	glUniform1f( U( p, "uContour" ), s.contour );
	glUniform1f( U( p, "uDustCut" ), s.dustTrans ? TransDustCut( s.transKT, dust[ SlotOf( model ) ].keyQ ) : s.dustCut );
	glUniform1i( U( p, "uDustInvert" ), s.dustInvert ? 1 : 0 );
	glUniform1i( U( p, "uDissolveInvert" ), fs.dissolveInvert ? 1 : 0 );
	const ShardSlot& sh    = shards[ SlotOf( model ) ];
	const bool shardsReady = model && sh.key == ShardKey( model, s );
	glActiveTexture( GL_TEXTURE0 + 15 );
	glBindTexture( GL_TEXTURE_BUFFER, shardsReady ? sh.ofTex : dummyShardOf );
	glActiveTexture( GL_TEXTURE0 + 16 );
	glBindTexture( GL_TEXTURE_BUFFER, shardsReady ? sh.dataTex : dummyShardData );
	glActiveTexture( GL_TEXTURE0 );
	if( s.contour > 0.0f )
	{
		glUniform1i( U( p, "uContourMode" ), s.contourMode );
		glUniform1f( U( p, "uContourSpacing" ), s.contourSpacing );
		glUniform1f( U( p, "uContourWidth" ), s.contourWidth );
		glUniform1f( U( p, "uContourScroll" ), s.contourScroll );
		glUniform3fv( U( p, "uContourColor" ), 1, s.contourColor );
	}
	glUniform1i( U( p, "uFoldN" ), s.deform ? std::max( 1, s.foldN ) : 1 ); // W2: the DEFORM programs divide by it
	if( !s.on )
		return;
	glUniform1i( U( p, "uShardsReady" ), shardsReady ? 1 : 0 );
	if( p == progSoupB || p == progShadowSoupB ) // W26: the recorded flights (unit 24)
	{
		glActiveTexture( GL_TEXTURE0 + 24 );
		glBindTexture( GL_TEXTURE_BUFFER, sh.pathTex ? sh.pathTex : dummyShardData );
		glUniform1i( U( p, "uShardPath" ), 24 );
	}
	glUniform1f( U( p, "uShatterT" ), s.shatterT );
	glUniform1f( U( p, "uShatterForce" ), s.shatterForce );
	glUniform1f( U( p, "uShatterGravity" ), s.shatterGravity );
	glUniform1f( U( p, "uShardSpin" ), s.shardSpin );
	glUniform1i( U( p, "uTransSide" ), s.transSide );
	glUniform1f( U( p, "uTransE" ), s.transE );
	glUniform1f( U( p, "uTransStagger" ), s.transStagger );
	glUniform1f( U( p, "uTransEase" ), s.transEase );
	glUniform1f( U( p, "uMelt" ), s.melt );
	glUniform1f( U( p, "uDrip" ), s.drip );
	glUniform1i( U( p, "uSliceAxis" ), s.sliceAxis );
	glUniform1f( U( p, "uSliceCount" ), float( s.sliceCount ) );
	glUniform1f( U( p, "uSliceGap" ), s.sliceGap );
	glUniform1f( U( p, "uSliceWave" ), s.sliceWave );
	glUniform1f( U( p, "uSlide" ), s.slide );
	glUniform1f( U( p, "uVoxel" ), s.voxel );
	glUniform1f( U( p, "uVoxelSize" ), std::max( 1e-3f, s.voxelSize ) );
	glUniform1f( U( p, "uLowPoly" ), s.lowPoly );
	glUniform1f( U( p, "uLowPolySize" ), std::max( 1e-3f, s.lowPolySize ) );
	if( s.lowPoly > 0.0f )
	{
		glUniformMatrix3fv( U( p, "uLowPolyGrid" ), 1, GL_FALSE, s.lowPolyGrid );
		glUniform1i( U( p, "uTumble" ), s.tumble );
		glUniform3fv( U( p, "uTumbleAng" ), 1, s.tumbleAng );
		glUniform3fv( U( p, "uTumblePh" ), 1, s.tumblePhase );
		glUniform1f( U( p, "uTumbleRandom" ), s.tumbleRandom );
		glUniform1f( U( p, "uTumbleSway" ), s.tumbleSway );
		glUniform1f( U( p, "uTumbleW" ), s.tumbleW );
		glUniform1f( U( p, "uFacetGap" ), s.facetGap );
		glUniform1i( U( p, "uFlipOn" ), s.flipOn ? 1 : 0 );
		glUniform1f( U( p, "uFlipBase" ), s.flipBase );
		glUniform1f( U( p, "uFlipT" ), s.flipT );
		glUniform1f( U( p, "uFlipDur" ), s.flipDur );
		glUniform1f( U( p, "uFlipDeg" ), s.flipDeg );
	}
	glUniform1i( U( p, "uDeformAxis" ), s.deformAxis );
	glUniform1f( U( p, "uTwist" ), s.twist );
	glUniform1f( U( p, "uBend" ), s.bend );
	glUniform1f( U( p, "uRipple" ), s.ripple );
	glUniform1f( U( p, "uRippleFreq" ), s.rippleFreq );
	glUniform1f( U( p, "uRipplePhase" ), s.ripplePhase );
	glUniform1f( U( p, "uSlicePhase" ), s.slicePhase );
	if( s.deform ) // W2 Deform group (the DEFORM twins only)
	{
		glUniform1f( U( p, "uTaper" ), s.taper );
		glUniform1f( U( p, "uSquash" ), s.squash );
		glUniform1f( U( p, "uShear" ), s.shear );
		glUniform1f( U( p, "uBulge" ), s.bulge );
		glUniform1f( U( p, "uSpherize" ), s.spherize );
		glUniform1i( U( p, "uShapeTarget" ), s.shapeTarget );
		glUniform1f( U( p, "uWarp" ), s.warp );
		glUniform1f( U( p, "uWarpScale" ), s.warpScale );
		glUniform1f( U( p, "uWarpPhase" ), s.warpPhase );
		glUniform1f( U( p, "uGlitchShift" ), s.glitchShift );
		glUniform1f( U( p, "uGlitchBands" ), s.glitchBands );
		glUniform1f( U( p, "uGlitchStep" ), s.glitchStep );
		glUniform3fv( U( p, "uFoldAxes" ), 1, s.foldAxes );
		glUniform1f( U( p, "uFoldK" ), s.foldK );
		glUniform3fv( U( p, "uJellyLin" ), 1, s.jellyLin );
		glUniform3fv( U( p, "uJellyAng" ), 1, s.jellyAng );
		glUniform1f( U( p, "uJellyScale" ), s.jellyScale );
		SetXUniforms( p, fs, model ); // W9: the Build / Cut looks
	}
}

// Starts / collects the dust point sampling; steps the transform-feedback simulation.
void Renderer::UpdateDust( const FrameState& fs, GpuModel* model, bool prepOnly )
{
	const ShapeState& s = fs.shape;
	if( !model || !model->geometry )
		return;
	DustSlot& d       = dust[ SlotOf( model ) ];
	const bool wanted = s.dustCut > 0.0f || s.dustReturning;
	// the points are sampled as soon as a model is up (18 ms off-thread for 100k), so the first
	// Blow To Dust finds them ready and the programs already compiled
	const uint64_t key = model->serial * 1048576 + (uint64_t)s.dustCount;
	if( key != d.key && ( !d.job || d.job->key != key ) )
	{
		if( d.job )
			d.job->cancel.store( true );
		auto job = std::make_shared< DustJob >();
		job->key = key;
		auto geo = model->geometry;
		int n    = s.dustCount;
		d.job    = job;
		std::thread( [ job, geo, n ]() {
			BuildDust( *geo, n, *job );
			job->done.store( true );
		} ).detach();
	}
	if( d.job && d.job->done.load() )
	{
		auto job = d.job;
		d.job.reset();
		if( job->points.empty() || job->cancel.load() )
			return;
		const int n = (int)( job->points.size() / 5 );
		if( !d.origin )
		{
			glGenBuffers( 1, &d.origin );
			glGenBuffers( 2, d.state );
			glGenVertexArrays( 2, d.simVAO );
			glGenVertexArrays( 2, d.drawVAO );
		}
		GLint prevArray = 0;
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prevArray );
		glBindBuffer( GL_ARRAY_BUFFER, d.origin );
		glBufferData( GL_ARRAY_BUFFER, job->points.size() * sizeof( float ), job->points.data(), GL_STATIC_DRAW );
		for( int i = 0; i < 2; ++i )
		{
			glBindBuffer( GL_ARRAY_BUFFER, d.state[ i ] );
			glBufferData( GL_ARRAY_BUFFER, size_t( n ) * 6 * sizeof( float ), nullptr, GL_DYNAMIC_COPY );
		}
		for( int i = 0; i < 2; ++i )
			for( GLuint vao : { d.simVAO[ i ], d.drawVAO[ i ] } )
			{
				glBindVertexArray( vao );
				glBindBuffer( GL_ARRAY_BUFFER, d.origin );
				glEnableVertexAttribArray( 0 );
				glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)0 );
				glEnableVertexAttribArray( 1 );
				glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
				glBindBuffer( GL_ARRAY_BUFFER, d.state[ i ] );
				glEnableVertexAttribArray( 2 );
				glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)0 );
				if( vao == d.simVAO[ i ] )
				{
					glEnableVertexAttribArray( 3 );
					glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
				}
			}
		if( model->anim && job->refs.size() == size_t( n ) * 3 )
		{
			// MoveEhr W1: an animated model's dust: the refs, the posed origins (6 floats: xyz + face normal, refilled each posed frame)
			// and VAOs reading them (location 4 = the rest origin: the noise / keys stay on the surface)
			if( !d.refBuf )
				glGenBuffers( 1, &d.refBuf ), glGenBuffers( 1, &d.posedBuf ), glGenVertexArrays( 2, d.simVAOp ), glGenVertexArrays( 2, d.drawVAOp );
			glBindBuffer( GL_ARRAY_BUFFER, d.refBuf );
			glBufferData( GL_ARRAY_BUFFER, job->refs.size() * sizeof( float ), job->refs.data(), GL_STATIC_DRAW );
			glBindBuffer( GL_ARRAY_BUFFER, d.posedBuf );
			glBufferData( GL_ARRAY_BUFFER, size_t( n ) * 6 * sizeof( float ), nullptr, GL_DYNAMIC_COPY );
			for( int i = 0; i < 2; ++i )
				for( GLuint vao : { d.simVAOp[ i ], d.drawVAOp[ i ] } )
				{
					glBindVertexArray( vao );
					glBindBuffer( GL_ARRAY_BUFFER, d.posedBuf );
					glEnableVertexAttribArray( 0 );
					glVertexAttribPointer( 0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)0 );
					glBindBuffer( GL_ARRAY_BUFFER, d.origin );
					glEnableVertexAttribArray( 1 );
					glVertexAttribPointer( 1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
					glEnableVertexAttribArray( 4 );
					glVertexAttribPointer( 4, 3, GL_FLOAT, GL_FALSE, 5 * sizeof( float ), (void*)0 );
					glBindBuffer( GL_ARRAY_BUFFER, d.state[ i ] );
					glEnableVertexAttribArray( 2 );
					glVertexAttribPointer( 2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)0 );
					if( vao == d.simVAOp[ i ] )
					{
						glEnableVertexAttribArray( 3 );
						glVertexAttribPointer( 3, 3, GL_FLOAT, GL_FALSE, 6 * sizeof( float ), (void*)( 3 * sizeof( float ) ) );
					}
				}
		}
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, prevArray );
		d.count      = n;
		d.partRanges = job->partRanges;
		std::memcpy( d.keyQ, job->keyQ, sizeof( d.keyQ ) );
		d.key        = job->key;
		d.live       = false; // the next step resets every point to its home
		d.warm       = true;
		if( LogVerbose() )
			Log( "DUST %d points for %s in %.0f ms (off the render thread)", n, LogName( model->path ).c_str(), job->ms );
	}
	if( d.key != key || d.count == 0 || prepOnly )
		return;
	if( s.dustTrans )
	{
		d.live = false; // the transition draws the points where the slider puts them; afterwards they start at home
		return;
	}
	if( !wanted && !d.warm )
	{
		d.live = false;
		return;
	}

	// MoveEhr W1: an animated side: this frame's posed origins first (the points' homes follow the pose)
	d.posedNow             = d.refBuf && PointsFromRefs( model, d.refBuf, d.posedBuf, d.count );
	const GLuint progSim   = d.posedNow ? progDustSimA : progDustSim;
	// one simulation step: state[idx] -> state[1 - idx]
	glUseProgram( progSim );
	glUniformMatrix4fv( U( progSim, "uModel" ), 1, GL_FALSE, fs.model );
	glUniform3fv( U( progSim, "uFitCenter" ), 1, model->fitCenter );
	glUniform1f( U( progSim, "uFitScale" ), model->fitScale );
	glUniform1f( U( progSim, "uCut" ), s.dustCut );
	glUniform1f( U( progSim, "uDt" ), s.dt );
	glUniform1f( U( progSim, "uCurlY" ), s.curlY );
	glUniform1f( U( progSim, "uWind" ), s.dustWind );
	glUniform1f( U( progSim, "uReturn" ), 4.0f / std::max( 0.1f, s.reformTime ) );
	if( s.dustRestart )
		d.live = false; // a new Blow To Dust while the dust is out: every point snaps home, then bursts again
	glUniform1i( U( progSim, "uReset" ), d.live ? 0 : 1 );
	glUniform3fv( U( progSim, "uCenter" ), 1, fs.post.maskCenter );
	glEnable( GL_RASTERIZER_DISCARD );
	glBindVertexArray( d.posedNow ? d.simVAOp[ d.idx ] : d.simVAO[ d.idx ] );
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, d.state[ 1 - d.idx ] );
	glBeginTransformFeedback( GL_POINTS );
	glDrawArrays( GL_POINTS, 0, d.count );
	glEndTransformFeedback();
	glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0 );
	glDisable( GL_RASTERIZER_DISCARD );
	glBindVertexArray( 0 );
	d.idx  = 1 - d.idx;
	d.live = true;
}

// Draws the airborne points into the scene target (depth tested, opaque), coloured from each part's texture.
void Renderer::DrawDust( const FrameState& fs, GpuModel* model, int h )
{
	if( !model )
		return;
	DustSlot& d       = dust[ SlotOf( model ) ];
	const bool trans  = fs.shape.dustTrans;
	if( !( d.live || trans ) || d.key != model->serial * 1048576 + (uint64_t)fs.shape.dustCount )
		return;
	const bool warmOnly = d.warm && !( trans || fs.shape.dustCut > 0.0f || fs.shape.dustReturning );
	if( warmOnly )
	{
		// first frame after the points arrived: compile both draw programs through a 1-pixel scissor
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 1, 1 );
	}
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	float light[ 3 ];
	for( int k = 0; k < 3; ++k )
		light[ k ] = 0.35f + 0.25f * fs.keyColor[ k ] + fs.ambient;
	// MoveEhr W1: an animated side draws on its posed origins (the Dust transition runs no sim step: its origins are posed here)
	const bool posed      = d.refBuf && ( trans ? PointsFromRefs( model, d.refBuf, d.posedBuf, d.count ) : d.posedNow && PosedFor( model ) );
	const GLuint pDraw    = posed ? progDustDrawA : progDustDraw, pTrans = posed ? progDustTransA : progDustTrans;
	glEnable( GL_PROGRAM_POINT_SIZE );
	glBindVertexArray( posed ? d.drawVAOp[ d.idx ] : d.drawVAO[ d.idx ] );
	glActiveTexture( GL_TEXTURE0 );
	auto draw = [ & ]( GLuint prog ) {
		glUseProgram( prog );
		glUniformMatrix4fv( U( prog, "uModel" ), 1, GL_FALSE, fs.model );
		glUniformMatrix4fv( U( prog, "uViewProj" ), 1, GL_FALSE, VP.m );
		glUniform1f( U( prog, "uPointSize" ), fs.shape.dustSize * float( h ) / 1080.0f );
		glUniform3fv( U( prog, "uLight" ), 1, light );
		glUniform1f( U( prog, "uExposure" ), fs.exposure );
		glUniform1i( U( prog, "uTonemap" ), fs.tonemap );
		if( prog == pTrans )
		{
			const ShapeState& s = fs.shape;
			glUniform3fv( U( prog, "uFitCenter" ), 1, model->fitCenter );
			glUniform1f( U( prog, "uFitScale" ), model->fitScale );
			glUniform1i( U( prog, "uSide" ), s.transSide );
			glUniform1f( U( prog, "uTransE" ), s.transE );
			glUniform1f( U( prog, "uStagger" ), s.transStagger );
			glUniform1f( U( prog, "uEase" ), s.transEase );
			glUniform1f( U( prog, "uSpread" ), s.transSpread );
			glUniform1f( U( prog, "uWind" ), s.dustWind );
			glUniform1fv( U( prog, "uKeyQ" ), 17, d.keyQ );
		}
		for( size_t i = 0; i < d.partRanges.size() && i < model->parts.size(); ++i )
		{
			const auto& r = d.partRanges[ i ];
			if( r[ 1 ] == 0 || !fs.parts[ model->parts[ i ].slot ].visible )
				continue;
			const GpuPart& part = model->parts[ i ];
			glBindTexture( GL_TEXTURE_2D, part.tex[ CH_BASECOLOR ] ? part.tex[ CH_BASECOLOR ] : whiteTex );
			glUniform1i( U( prog, "uHasBase" ), part.tex[ CH_BASECOLOR ] ? 1 : 0 );
			glUniform4fv( U( prog, "uBaseFactor" ), 1, part.baseColor );
			glDrawArrays( GL_POINTS, (GLint)r[ 0 ], (GLsizei)r[ 1 ] );
		}
	};
	draw( trans ? pTrans : pDraw );
	if( warmOnly )
		draw( pTrans );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glBindVertexArray( 0 );
	glDisable( GL_PROGRAM_POINT_SIZE );
	if( warmOnly )
	{
		glDisable( GL_SCISSOR_TEST );
		d.live = false;
	}
	d.warm = false;
}
}
