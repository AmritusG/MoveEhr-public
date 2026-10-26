// W9 Stage A: the line looks (Lines group, classic bundles). Every frame the look's spots (a triangle + barycentric each, built per model
// off the render thread) run through the CAPTURE twin of the model's own soup program with transform feedback (rasterizer off): their
// world position, normal and base colour this frame, after every deform, letter move, shard flight and morph. Then:
//   Plexus      the spots' neighbour pairs (found once, at rest, on the CPU: a grid of cell = Line Reach, the 8 nearest within reach)
//               as camera-facing lines whose light falls with the CURRENT distance ((1 - d / reach)^2: pairs flying apart fade out)
//   Light Paint the heads' past positions in a ring (60 samples / s of show time, 4 s), a ribbon per head through them, fading with age
//   Neon Tubes  the W5 feature edges as tube pairs: a white-hot core, coloured glow; their light on the model = the tubes' glow blurred
//               wide (Line Reach) x the model's own picture (screen space: no shadowing), light at alpha 0
// All of it is light: rgb added in the main pass (alpha kept, W23), and drawn again into the W5 HDR wire buffer for Wire Glow / Bloom.
#include "Generate.h"
#include "Log.h"
#include "Renderer.h"
#include "Shaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace mehr
{
GLuint Link( const std::string& vs, const std::string& fs, std::string& error );
void BuildPointRefs( const CpuGeometry& g, int source, const std::vector< PointSet::Part >& parts, PointSet::Job& job ); // Points.cpp
void BuildEdgeFlags( const CpuGeometry& g, std::vector< uint8_t >& out );                                                // Wire.cpp
std::vector< uint32_t > WeldByPosition( const CpuGeometry& g );                                                           // Wire.cpp

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

// shared by the line programs: a segment a -> b (world) as a camera-facing quad of width uWidthPx (+ 1 px of anti-aliasing)
const char* kLineLib = R"GLSL(
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform vec2 uViewport;
uniform float uProjY;    // P[1][1]: world size of a pixel at clip w = 2 w / (uProjY x height)
uniform float uWidthPx;
float WorldPerPx( vec4 c ) { return 2.0 * c.w / ( uProjY * uViewport.y ); }
// a point lifted toward the camera by `px` pixels' worth of world (so a line on the surface is not eaten by the surface's own depth)
vec3 Lift( vec3 p, float px )
{
	vec4 c = uViewProj * vec4( p, 1.0 );
	return p + normalize( uCamPos - p + vec3( 1e-6 ) ) * WorldPerPx( c ) * px;
}
// corner k (0..3, a triangle strip: a-, b-, a+, b+) of the quad round a -> b; across = signed px from the centre line
vec4 Corner( vec3 a, vec3 b, int k, float hw, float cap, out float across )
{
	vec4 ca = uViewProj * vec4( a, 1.0 ), cb = uViewProj * vec4( b, 1.0 );
	if( ca.w <= 1e-4 || cb.w <= 1e-4 )
	{
		across = 0.0;
		return vec4( 2.0, 2.0, 2.0, 1.0 ); // behind the camera: off screen
	}
	vec2 sa = ca.xy / ca.w * 0.5 * uViewport, sb = cb.xy / cb.w * 0.5 * uViewport;
	vec2 d  = sb - sa;
	float L = length( d );
	d       = L > 1e-4 ? d / L : vec2( 1.0, 0.0 );
	vec2 n  = vec2( -d.y, d.x );
	bool end   = ( k & 1 ) == 1;
	float side = ( k & 2 ) == 0 ? -1.0 : 1.0;
	vec4 c     = end ? cb : ca;
	vec2 off   = n * side * hw + d * ( end ? cap : -cap );
	c.xy += off * 2.0 / uViewport * c.w;
	across = side * hw;
	return c;
}
)GLSL";

// Plexus pairs (uLook 1) and neon edge tubes (uLook 3): one instance per line
const char* kLinePairVS = R"GLSL(
uniform samplerBuffer uCapPos, uCapNrm, uCapCol;
uniform usamplerBuffer uPairs;
uniform int uLook;
uniform float uReachW;      // plexus: the reach in world units
uniform float uBands[ 16 ];
uniform float uAudio;
uniform vec3 uLineColor;
uniform float uColorMix;
uniform float uWeight;      // the pair's side weight mid-hop
uniform float uTube;        // neon: Line Amount (the tubes' width and strength)
out vec2 vAcross;           // x: signed px from the centre, y: half width px
out float vAlong;
out vec3 vColor;
flat out float vBright, vSeed;
vec3 LineColour( vec3 base, float mixK ) { return mix( min( pow( max( base, 0.0 ), vec3( 1.0 / 2.2 ) ) * 1.6 + 0.08, vec3( 1.0 ) ), uLineColor, mixK ); }
void main()
{
	int ia, ib;
	if( uLook == 1 )
	{
		uvec2 pr = texelFetch( uPairs, gl_InstanceID ).rg;
		ia = int( pr.x ), ib = int( pr.y );
	}
	else
		ia = 2 * gl_InstanceID, ib = ia + 1;
	vec4 A = texelFetch( uCapPos, ia ), B = texelFetch( uCapPos, ib );
	vec4 nA = texelFetch( uCapNrm, ia ), nB = texelFetch( uCapNrm, ib );
	vec4 cA = texelFetch( uCapCol, ia ), cB = texelFetch( uCapCol, ib );
	float d    = length( B.xyz - A.xyz );
	float h    = clamp( ( 0.5 * ( nA.w + nB.w ) + 1.0 ) * 0.5, 0.0, 1.0 ); // rest height 0 (base) .. 1 (top)
	float band = uBands[ int( h * 15.99 ) ];
	float br   = min( cA.a, cB.a ) * uWeight * ( 1.0 + uAudio * band * 1.5 );
	vec3 a = A.xyz, b = B.xyz;
	float hw = 0.5 * uWidthPx;
	if( uLook == 1 )
	{
		float reach = uReachW * ( 1.0 + 0.5 * uAudio * band );
		float x     = clamp( d / max( reach, 1e-6 ), 0.0, 1.0 );
		br *= d < reach ? ( 1.0 - x ) * ( 1.0 - x ) : 0.0;
		a += nA.xyz * d * 0.15, b += nB.xyz * d * 0.15; // a chord over a curved surface: lifted off it along the normals
		a = Lift( a, 1.0 ), b = Lift( b, 1.0 );
		vColor = LineColour( mix( cA.rgb, cB.rgb, float( gl_VertexID & 1 ) ), uColorMix );
	}
	else
	{
		hw *= 3.0 + 8.0 * uTube;
		a = Lift( a, hw * 4.0 ), b = Lift( b, hw * 4.0 ); // well clear of the creases they sit on (a camera-facing quad cuts the faces)
		vColor = uLineColor; // neon: the picked colour (Wire Color)
	}
	vBright = br;
	vSeed   = fract( sin( float( gl_InstanceID ) * 12.9898 + 3.1 ) * 43758.5453 );
	vAlong  = float( gl_VertexID & 1 );
	float across;
	gl_Position = br <= 1e-3 ? vec4( 2.0, 2.0, 2.0, 1.0 ) : Corner( a, b, gl_VertexID, hw + 1.0, uLook == 3 ? hw * 0.5 : 0.0, across );
	vAcross     = vec2( across, hw );
}
)GLSL";

const char* kLinePairFS = R"GLSL(
in vec2 vAcross;
in float vAlong;
in vec3 vColor;
flat in float vBright, vSeed;
uniform int uLook;
uniform float uFlow;    // cycles (pulses run along the lines), wrapped at 1000
uniform float uSpeed;   // Line Speed (0 = no pulses / no flicker)
uniform float uFlicker; // neon flicker steps, wrapped at 1000
uniform float uGain;
out vec4 fragColor;
void main()
{
	float x  = abs( vAcross.x ), hw = max( vAcross.y, 0.5 );
	vec3 c;
	if( uLook == 1 )
	{
		float a     = clamp( hw + 0.5 - x, 0.0, 1.0 );
		float rt    = floor( ( 0.7 + 0.6 * vSeed ) * 1000.0 + 0.5 ) / 1000.0; // whole cycles per 1000: seamless at the clock's wrap
		float pulse = uSpeed > 0.0 ? 1.0 + 2.0 * pow( 0.5 + 0.5 * cos( 6.2831853 * ( vAlong - fract( uFlow * rt ) + vSeed ) ), 12.0 ) : 1.0;
		c = vColor * vBright * pulse * a;
	}
	else
	{
		float r    = x / hw;
		float core = exp( -r * r * 10.0 ), glow = exp( -r * r * 2.2 ) * ( 1.0 - smoothstep( 0.85, 1.05, r ) );
		float fl   = 1.0;
		if( uSpeed > 0.0 )
		{
			// a buzz (a hash per tube and flicker step: now and then a tube dips) and light flowing along the tube
			float st = fract( sin( ( floor( uFlicker ) + vSeed * 97.0 ) * 12.9898 ) * 43758.5453 );
			fl       = ( st > 0.93 ? 0.35 : 1.0 ) * ( 0.75 + 0.25 * cos( 6.2831853 * ( vAlong * 2.0 - uFlow + vSeed ) ) );
		}
		c = ( vColor * glow * 1.6 + vec3( 1.0 ) * core * 0.55 ) * vBright * fl;
	}
	fragColor = vec4( c * uGain, 0.0 ); // light: rgb added, alpha kept
}
)GLSL";

// Light Paint: one instance per head, a strip through its live position and its ring of past positions
const char* kRibbonVS = R"GLSL(
uniform samplerBuffer uCapPos, uCapNrm, uCapCol, uRing;
uniform int uRows, uHeads, uHeadRow, uFilled;
uniform float uNow, uTrail;
uniform float uBands[ 16 ]; // W10: Line Audio, as the pairs: the band of the head's rest height pulses its light and its length
uniform float uAudio;
uniform vec3 uLineColor;
uniform float uColorMix, uWeight;
out vec2 vAcross;
out vec3 vColor;
out float vFade;
vec4 P( int k ) // k 0 = live, k >= 1 = the ring's k-th newest sample (clamped to what exists)
{
	if( k <= 0 )
		return vec4( texelFetch( uCapPos, gl_InstanceID ).xyz, uNow );
	int r = ( uHeadRow - min( k - 1, uFilled - 1 ) + uRows * 4 ) % uRows;
	return texelFetch( uRing, r * uHeads + gl_InstanceID );
}
void main()
{
	int k      = gl_VertexID >> 1;
	int kk     = min( k, uFilled );
	vec4 p     = P( kk ), p0 = P( max( kk - 1, 0 ) ), p1 = P( min( kk + 1, uFilled ) );
	float h    = clamp( ( texelFetch( uCapNrm, gl_InstanceID ).w + 1.0 ) * 0.5, 0.0, 1.0 ); // rest height 0 (base) .. 1 (top)
	float band = uAudio * uBands[ int( h * 15.99 ) ];
	float age  = mod( uNow - p.w + 7200.0, 3600.0 );
	float fade = k > uFilled ? 0.0 : clamp( 1.0 - age / max( uTrail * ( 1.0 + 0.5 * band ), 1e-3 ), 0.0, 1.0 );
	vec4 col   = texelFetch( uCapCol, gl_InstanceID );
	vColor     = mix( min( pow( max( col.rgb, 0.0 ), vec3( 1.0 / 2.2 ) ) * 1.6 + 0.08, vec3( 1.0 ) ), uLineColor, uColorMix ) * ( 1.0 + band * 1.5 );
	vFade      = fade * col.a * uWeight;
	// the strip's direction on screen from its neighbours
	vec4 c   = uViewProj * vec4( p.xyz, 1.0 );
	vec4 ca  = uViewProj * vec4( p0.xyz, 1.0 ), cb = uViewProj * vec4( p1.xyz, 1.0 );
	vec2 d   = ( cb.w > 1e-4 ? cb.xy / cb.w : c.xy / c.w ) - ( ca.w > 1e-4 ? ca.xy / ca.w : c.xy / c.w );
	d *= uViewport;
	d = dot( d, d ) > 1e-8 ? normalize( d ) : vec2( 1.0, 0.0 );
	float side = ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0;
	float hw   = 0.5 * uWidthPx * ( 0.35 + 0.65 * sqrt( fade ) ) + 1.0;
	if( c.w <= 1e-4 || vFade <= 1e-3 )
		c = vec4( 2.0, 2.0, 2.0, 1.0 );
	else
		c.xy += vec2( -d.y, d.x ) * side * hw * 2.0 / uViewport * c.w;
	gl_Position = c;
	vAcross     = vec2( side * hw, hw - 1.0 );
}
)GLSL";

const char* kRibbonFS = R"GLSL(
in vec2 vAcross;
in vec3 vColor;
in float vFade;
uniform float uGain;
out vec4 fragColor;
void main()
{
	float a = clamp( max( vAcross.y, 0.5 ) + 0.5 - abs( vAcross.x ), 0.0, 1.0 );
	fragColor = vec4( vColor * vFade * vFade * a * uGain, 0.0 );
}
)GLSL";

// Plexus nodes: a round dot at every spot that has a line
const char* kLineDotVS = R"GLSL(
uniform samplerBuffer uCapPos, uCapCol;
uniform vec3 uLineColor;
uniform float uColorMix, uWeight;
out vec3 vColor;
void main()
{
	vec4 p      = texelFetch( uCapPos, gl_VertexID );
	vec4 col    = texelFetch( uCapCol, gl_VertexID );
	vColor      = mix( min( pow( max( col.rgb, 0.0 ), vec3( 1.0 / 2.2 ) ) * 1.6 + 0.08, vec3( 1.0 ) ), uLineColor, uColorMix ) * col.a * uWeight;
	gl_Position = uViewProj * vec4( Lift( p.xyz, uWidthPx * 2.0 ), 1.0 );
	gl_PointSize = max( 2.0, uWidthPx * 2.5 );
}
)GLSL";
const char* kLineDotFS = R"GLSL(
in vec3 vColor;
uniform float uGain;
out vec4 fragColor;
void main()
{
	float r = length( gl_PointCoord * 2.0 - 1.0 );
	fragColor = vec4( vColor * ( 1.0 - smoothstep( 0.6, 1.0, r ) ) * uGain, 0.0 );
}
)GLSL";

// Neon: the tubes' wide light on the model: the blurred glow x (a little + the model's own picture), where the model is
const char* kLineLightFS = R"GLSL(
in vec2 vUV;
uniform sampler2D uLight, uScene;
uniform float uGain;
out vec4 fragColor;
void main()
{
	vec4 s  = texture( uScene, vUV );
	vec3 l  = texture( uLight, vUV ).rgb;
	fragColor = vec4( l * ( 0.25 + 2.0 * s.rgb ) * s.a * uGain, 0.0 );
}
)GLSL";

uint64_t Mix64( uint64_t h, uint64_t v ) { return ( h ^ ( v + 0x9e3779b97f4a7c15ull + ( h << 6 ) + ( h >> 2 ) ) ) * 0xff51afd7ed558ccdull; }

// the spots of a look on a model (whole-mesh parts only, the GPU part order) + the plexus pairs
void BuildLineSet( const CpuGeometry& g, int look, int source, int count, float reach, const std::vector< PointSet::Part >& parts, Renderer::LineSet::Job& job )
{
	const auto t0 = std::chrono::steady_clock::now();
	std::vector< PointSet::Part > whole = parts;
	for( PointSet::Part& p : whole )
		if( p.fractured )
			p.count = 0;
	job.ranges.assign( parts.size(), { 0u, 0u } );
	if( look == 3 )
	{
		// neon: every feature edge once (welded), as the two corners of a triangle that has it
		std::vector< uint8_t > flags;
		BuildEdgeFlags( g, flags );
		const std::vector< uint32_t > weld = WeldByPosition( g );
		std::unordered_set< uint64_t > seen;
		const float bc[ 3 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
		for( size_t p = 0; p < whole.size(); ++p )
		{
			job.ranges[ p ][ 0 ] = uint32_t( job.refs.size() / 4 );
			const size_t t0p = whole[ p ].first / 3, t1p = t0p + whole[ p ].count / 3;
			for( size_t t = t0p; t < t1p && t < flags.size(); ++t )
				for( int k = 0; k < 3; ++k )
				{
					if( !( flags[ t ] & ( 1u << k ) ) )
						continue;
					const int c0 = ( k + 1 ) % 3, c1 = ( k + 2 ) % 3;
					uint32_t a = weld[ g.idx[ t * 3 + c0 ] ], b = weld[ g.idx[ t * 3 + c1 ] ];
					if( !seen.insert( ( uint64_t( std::min( a, b ) ) << 32 ) | std::max( a, b ) ).second )
						continue;
					for( int c : { c0, c1 } )
						job.refs.insert( job.refs.end(), { float( t ), bc[ c ][ 0 ], bc[ c ][ 1 ], 0.0f } );
				}
			job.ranges[ p ][ 1 ] = uint32_t( job.refs.size() / 4 ) - job.ranges[ p ][ 0 ];
		}
	}
	else
	{
		// plexus: a subset of the Point Source set; light paint: of Surface 25k. Kept with probability count / total (a hash: stable)
		PointSet::Job all;
		BuildPointRefs( g, look == 1 ? source : 2, whole, all ); // W9: light paint, spikes (4), copies (5): Surface 25k
		const size_t total = all.refs.size() / 4;
		const double keep  = total ? std::min( 1.0, double( count ) / double( total ) ) : 0.0;
		for( size_t p = 0; p < whole.size(); ++p )
		{
			job.ranges[ p ][ 0 ] = uint32_t( job.refs.size() / 4 );
			for( uint32_t i = all.ranges[ p ][ 0 ]; i < all.ranges[ p ][ 0 ] + all.ranges[ p ][ 1 ]; ++i )
			{
				const uint64_t h = Mix64( 77, i );
				if( double( h >> 11 ) / double( 1ull << 53 ) < keep )
					job.refs.insert( job.refs.end(), &all.refs[ i * 4 ], &all.refs[ i * 4 ] + 4 );
			}
			job.ranges[ p ][ 1 ] = uint32_t( job.refs.size() / 4 ) - job.ranges[ p ][ 0 ];
		}
		if( look == 1 && reach > 0.0f )
		{
			// rest positions (fitted), then the 8 nearest within the reach per spot (a grid of cell = reach); pairs i < j
			const size_t n = job.refs.size() / 4;
			std::vector< float > P( n * 3 );
			for( size_t i = 0; i < n; ++i )
			{
				const size_t t = size_t( job.refs[ i * 4 ] );
				const float b1 = job.refs[ i * 4 + 1 ], b2 = job.refs[ i * 4 + 2 ], b0 = 1.0f - b1 - b2;
				for( int k = 0; k < 3; ++k )
				{
					const float v = g.pos[ g.idx[ t * 3 ] * 3 + k ] * b0 + g.pos[ g.idx[ t * 3 + 1 ] * 3 + k ] * b1 + g.pos[ g.idx[ t * 3 + 2 ] * 3 + k ] * b2;
					P[ i * 3 + k ] = ( v - g.fitCenter[ k ] ) * g.fitScale;
				}
			}
			auto cell = [ & ]( float v ) { return int64_t( std::floor( v / reach ) ); };
			auto key  = []( int64_t x, int64_t y, int64_t z ) { return uint64_t( ( x & 0x1FFFFF ) | ( ( y & 0x1FFFFF ) << 21 ) | ( ( z & 0x1FFFFF ) << 42 ) ); };
			std::unordered_map< uint64_t, std::vector< uint32_t > > grid;
			grid.reserve( n );
			for( size_t i = 0; i < n; ++i )
				grid[ key( cell( P[ i * 3 ] ), cell( P[ i * 3 + 1 ] ), cell( P[ i * 3 + 2 ] ) ) ].push_back( uint32_t( i ) );
			const float r2 = reach * reach;
			std::vector< std::pair< float, uint32_t > > near;
			for( size_t i = 0; i < n && !job.cancel.load(); ++i )
			{
				near.clear();
				const int64_t cx = cell( P[ i * 3 ] ), cy = cell( P[ i * 3 + 1 ] ), cz = cell( P[ i * 3 + 2 ] );
				for( int dz = -1; dz <= 1; ++dz )
					for( int dy = -1; dy <= 1; ++dy )
						for( int dx = -1; dx <= 1; ++dx )
						{
							auto it = grid.find( key( cx + dx, cy + dy, cz + dz ) );
							if( it == grid.end() )
								continue;
							for( uint32_t j : it->second )
							{
								if( j == i )
									continue;
								const float ex = P[ j * 3 ] - P[ i * 3 ], ey = P[ j * 3 + 1 ] - P[ i * 3 + 1 ], ez = P[ j * 3 + 2 ] - P[ i * 3 + 2 ];
								const float d2 = ex * ex + ey * ey + ez * ez;
								if( d2 < r2 && d2 > 1e-12f )
									near.push_back( { d2, j } );
							}
						}
				const size_t k = std::min< size_t >( 8, near.size() );
				std::partial_sort( near.begin(), near.begin() + k, near.end() );
				for( size_t q = 0; q < k; ++q ) // a pair either end keeps (deduped below)
					job.pairs.insert( job.pairs.end(), { std::min( uint32_t( i ), near[ q ].second ), std::max( uint32_t( i ), near[ q ].second ) } );
			}
			// a pair both ends keep is in twice: dedupe
			std::vector< uint64_t > keys( job.pairs.size() / 2 );
			for( size_t q = 0; q < keys.size(); ++q )
				keys[ q ] = ( uint64_t( job.pairs[ q * 2 ] ) << 32 ) | job.pairs[ q * 2 + 1 ];
			std::sort( keys.begin(), keys.end() );
			keys.erase( std::unique( keys.begin(), keys.end() ), keys.end() );
			job.pairs.resize( keys.size() * 2 );
			for( size_t q = 0; q < keys.size(); ++q )
				job.pairs[ q * 2 ] = uint32_t( keys[ q ] >> 32 ), job.pairs[ q * 2 + 1 ] = uint32_t( keys[ q ] );
		}
	}
	job.ms = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
}
}

GLuint Renderer::CaptureTwin( GLuint p ) const
{
	for( const auto& t : captureTwins )
		if( t.first == p )
			return t.second;
	return p;
}

bool Renderer::InitLines( std::string& error )
{
	const std::string lib = kLineLib;
	progLinePair   = Link( lib + kLinePairVS, kLinePairFS, error );
	progLineRibbon = Link( lib + kRibbonVS, kRibbonFS, error );
	progLineDot    = Link( lib + kLineDotVS, kLineDotFS, error );
	progLineLight  = Link( kQuadVS, kLineLightFS, error );
	if( !progLinePair || !progLineRibbon || !progLineDot || !progLineLight )
		return false;
	for( GLuint p : { progLinePair, progLineRibbon, progLineDot } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uCapPos" ), 0 );
		glUniform1i( U( p, "uCapNrm" ), 1 );
		glUniform1i( U( p, "uCapCol" ), 2 );
		glUniform1i( U( p, "uPairs" ), 3 );
		glUniform1i( U( p, "uRing" ), 3 );
	}
	glUseProgram( progLineLight );
	glUniform1i( U( progLineLight, "uLight" ), 0 );
	glUniform1i( U( progLineLight, "uScene" ), 1 );
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseLines()
{
	auto delTex = []( GLuint& t ) { if( t ) glDeleteTextures( 1, &t ), t = 0; };
	auto delBuf = []( GLuint& b ) { if( b ) glDeleteBuffers( 1, &b ), b = 0; };
	for( LineSet& s : lineSets )
	{
		delTex( s.refTex ), delBuf( s.refBuf ), delTex( s.pairTex ), delBuf( s.pairBuf );
		if( s.job )
			s.job->cancel.store( true );
		s = LineSet();
	}
	for( Capture& c : capture )
	{
		for( int k = 0; k < 3; ++k )
			delTex( c.tex[ k ] ), delBuf( c.buf[ k ] );
		delTex( c.ringTex ), delBuf( c.ringBuf );
		c = Capture();
	}
	for( int i = 0; i < 2; ++i )
	{
		delTex( lineBlur[ i ] );
		if( lineBlurFBO[ i ] )
			glDeleteFramebuffers( 1, &lineBlurFBO[ i ] ), lineBlurFBO[ i ] = 0;
	}
	lineBlurW = lineBlurH = 0;
	for( GLuint* p : { &progLinePair, &progLineRibbon, &progLineDot, &progLineLight } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
}

// the look's spot set for this model: the one matching the dials, else (while that builds) the newest one built for the model and look
const Renderer::LineSet* Renderer::LinesFor( GpuModel* model, const LinesState& L )
{
	if( !model || !model->geometry || L.look == 0 )
		return nullptr;
	static uint64_t frame = 0;
	++frame;
	const int count     = L.look == 3 ? 0 : L.points;
	const int source    = L.look == 1 ? L.source : 0;
	const float reach   = L.look == 1 ? std::round( L.reach * 200.0f ) / 200.0f : 0.0f; // plexus pairs: rebuilt per 0.005 of reach
	uint64_t key        = Mix64( Mix64( Mix64( Mix64( model->serial, uint64_t( L.look ) ), uint64_t( source ) ), uint64_t( count ) ), uint64_t( reach * 1000.0f ) );
	LineSet* want = nullptr, *stale = nullptr;
	bool building = false;
	for( LineSet& s : lineSets )
	{
		if( s.key == key )
			want = &s;
		if( s.serial == model->serial && s.look == L.look )
		{
			if( s.ready && ( !stale || s.used > stale->used ) )
				stale = &s;
			building = building || ( s.job && !s.ready );
		}
	}
	if( !want && !building )
	{
		// the entry asked for longest ago
		LineSet* e = &lineSets[ 0 ];
		for( LineSet& s : lineSets )
			if( s.used < e->used && !( s.job && !s.ready ) )
				e = &s;
		if( e->refTex )
			glDeleteTextures( 1, &e->refTex );
		if( e->refBuf )
			glDeleteBuffers( 1, &e->refBuf );
		if( e->pairTex )
			glDeleteTextures( 1, &e->pairTex );
		if( e->pairBuf )
			glDeleteBuffers( 1, &e->pairBuf );
		if( e->job )
			e->job->cancel.store( true );
		*e        = LineSet();
		e->key    = key;
		e->serial = model->serial;
		e->look   = L.look;
		e->used   = frame;
		std::vector< PointSet::Part > parts;
		for( const GpuPart& gp : model->parts )
			parts.push_back( { gp.firstIndex, gp.indexCount, gp.fractured } );
		auto job = std::make_shared< LineSet::Job >();
		e->job   = job;
		auto geo  = model->geometry;
		auto name = LogName( model->path );
		const int look = L.look;
		std::thread( [ job, geo, look, source, count, reach, parts, name ]() {
			BuildLineSet( *geo, look, source, count, reach, parts, *job );
			Log( "LINES %s on %s: %zu spots, %zu pairs (reach %.3f), %.0f ms (worker)", look == 1 ? "plexus" : look == 2 ? "light paint" : look == 3 ? "neon tubes" : look == 4 ? "spikes" : "copies", name.c_str(),
			     job->refs.size() / 4, job->pairs.size() / 2, reach, job->ms );
			job->done.store( true );
		} ).detach();
		want = e;
	}
	if( want && !want->ready && want->job && want->job->done.load() )
	{
		const LineSet::Job& j = *want->job;
		auto upload = []( GLuint& buf, GLuint& tex, const void* data, size_t bytes, GLenum fmt ) {
			glGenBuffers( 1, &buf );
			glBindBuffer( GL_TEXTURE_BUFFER, buf );
			glBufferData( GL_TEXTURE_BUFFER, std::max< size_t >( 16, bytes ), bytes ? data : nullptr, GL_STATIC_DRAW );
			glGenTextures( 1, &tex );
			glBindTexture( GL_TEXTURE_BUFFER, tex );
			glTexBuffer( GL_TEXTURE_BUFFER, fmt, buf ); // after glBufferData (Apple GL keeps reading old storage otherwise)
		};
		upload( want->refBuf, want->refTex, j.refs.data(), j.refs.size() * sizeof( float ), GL_RGBA32F );
		upload( want->pairBuf, want->pairTex, j.pairs.data(), j.pairs.size() * sizeof( uint32_t ), GL_RG32UI );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
		want->ranges = j.ranges;
		want->count  = j.refs.size() / 4;
		want->lines  = L.look == 1 ? j.pairs.size() / 2 : want->count / 2;
		want->ready  = true;
		want->job.reset();
	}
	LineSet* use = want && want->ready ? want : stale;
	if( use )
		use->used = frame;
	return use;
}

// this frame's spots of `set` through the model's own soup program (its CAPTURE twin) into capture[ side ]
bool Renderer::CaptureSpots( const FrameState& fs, GpuModel* model, GLuint prog, const LineSet& set, int side )
{
	Capture& c = capture[ side ];
	if( set.count == 0 )
		return false;
	if( c.cap < set.count )
	{
		c.cap = std::max< size_t >( set.count, 1024 );
		for( int k = 0; k < 3; ++k )
		{
			if( !c.buf[ k ] )
				glGenBuffers( 1, &c.buf[ k ] ), glGenTextures( 1, &c.tex[ k ] );
			glBindBuffer( GL_TEXTURE_BUFFER, c.buf[ k ] );
			glBufferData( GL_TEXTURE_BUFFER, c.cap * 16, nullptr, GL_DYNAMIC_COPY );
			glBindTexture( GL_TEXTURE_BUFFER, c.tex[ k ] );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, c.buf[ k ] );
		}
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	}
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, c.buf[ k ] );
	const LinesState& L = fs.lines;
	captureNow    = true;
	captureRefTex = set.refTex;
	captureRanges = &set.ranges;
	capTime       = L.time;
	capSwirl      = L.look == 2 ? L.swirlR : 0.0f;
	capPhase      = L.swirl;
	capAxis       = L.axis;
	DrawModel( prog, fs, model, false, true, 0 );
	captureNow    = false;
	captureRanges = nullptr;
	for( int k = 0; k < 3; ++k )
		glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, 0 );
	if( L.look == 2 )
	{
		// light paint: a sample into the ring each time the show clock passes a 1/60 s tick (frame-rate independent)
		const size_t heads = set.count;
		if( !c.ringBuf || c.ringHeads != heads || c.ringFor != set.key )
		{
			if( !c.ringBuf )
				glGenBuffers( 1, &c.ringBuf ), glGenTextures( 1, &c.ringTex );
			glBindBuffer( GL_TEXTURE_BUFFER, c.ringBuf );
			glBufferData( GL_TEXTURE_BUFFER, size_t( kRingRows ) * heads * 16, nullptr, GL_DYNAMIC_COPY );
			glBindTexture( GL_TEXTURE_BUFFER, c.ringTex );
			glTexBuffer( GL_TEXTURE_BUFFER, GL_RGBA32F, c.ringBuf );
			glBindTexture( GL_TEXTURE_BUFFER, 0 );
			glBindBuffer( GL_TEXTURE_BUFFER, 0 );
			c.ringHeads = heads, c.ringFor = set.key, c.ringHead = 0, c.ringFilled = 0, c.lastTick = -1;
		}
		const int64_t tick = int64_t( std::floor( double( L.time ) * 60.0 ) );
		if( tick != c.lastTick )
		{
			const int64_t period = 3600 * 60, step = ( ( tick - c.lastTick ) % period + period ) % period; // the stamps wrap at 3600 s
			if( c.lastTick >= 0 && step > 600 )
				c.ringFilled = 0; // the clock jumped (a pause of over 10 s, a restart): start the trails again
			c.lastTick = tick;
			c.ringHead = ( c.ringHead + 1 ) % kRingRows;
			// the sample: the same capture again, its positions straight into the ring's row (Apple GL has no glCopyBufferSubData:
			// "gldCopyBufferSubData: NEEDS IMPLEMENTATION"), the normals / colours into the live buffers (unchanged)
			glBindBufferRange( GL_TRANSFORM_FEEDBACK_BUFFER, 0, c.ringBuf, GLintptr( size_t( c.ringHead ) * heads * 16 ), GLsizeiptr( heads * 16 ) );
			for( int k = 1; k < 3; ++k )
				glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, c.buf[ k ] );
			captureNow = true, captureRanges = &set.ranges;
			DrawModel( prog, fs, model, false, true, 0 );
			captureNow = false, captureRanges = nullptr;
			for( int k = 0; k < 3; ++k )
				glBindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, k, 0 );
			c.ringFilled = std::min( c.ringFilled + 1, kRingRows );
		}
	}
	return true;
}

// the pair's lines: each side captures its own spots through the program it is drawn with (the outgoing model's lines fade out and the
// incoming one's in across the hop's middle), then they draw as light into the bound main target
void Renderer::RenderLines( const FrameState& fsA, GpuModel* a, GLuint progA, const FrameState* fsB, GpuModel* b, GLuint progB, float t )
{
	lineDraws.clear();
	const LinesState& L = fsA.lines;
	if( L.look == 0 )
		return;
	const float w = fsB ? std::min( 1.0f, std::max( 0.0f, ( t - 0.35f ) / 0.3f ) ) : 0.0f;
	const float wb = w * w * ( 3.0f - 2.0f * w );
	const int keep = activeSide;
	for( int side = 0; side < ( fsB ? 2 : 1 ); ++side )
	{
		const FrameState& f = side == 0 ? fsA : *fsB;
		GpuModel* m         = side == 0 ? a : b;
		const float weight  = fsB ? ( side == 0 ? 1.0f - wb : wb ) : 1.0f;
		if( weight <= 0.0f )
			continue;
		activeSide = fsB ? side : keep;
		EnsureVAO( m );
		const LineSet* set = LinesFor( m, L );
		if( set && CaptureSpots( f, m, side == 0 ? progA : progB, *set, side ) )
			lineDraws.push_back( { &f, set, side, weight, f.modelScale / std::max( m->fitScale, 1e-12f ) } );
	}
	activeSide = keep;
	EnsureVAO( a );
	if( lineDraws.empty() )
		return;
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE ); // light: rgb added, alpha kept (W23)
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_FALSE );
	for( const LineDraw& d : lineDraws )
		DrawLines( d, false );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LESS );
	glBlendFunc( GL_ONE, GL_ZERO );
	glDisable( GL_BLEND );
}

void Renderer::DrawLines( const LineDraw& d, bool glow )
{
	const FrameState& fs = *d.fs;
	const LinesState& L  = fs.lines;
	const Capture& c     = capture[ d.side ];
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V;
	// strength: Line Amount for the neon; the glow pass x 0.35 x Wire Glow in AddWireLight (the main pass shows the lines themselves)
	const float gain = L.look == 3 ? 0.4f + 1.6f * L.amount : 1.0f;
	auto common = [ & ]( GLuint p ) {
		glUseProgram( p );
		glUniformMatrix4fv( U( p, "uViewProj" ), 1, GL_FALSE, VP.m );
		glUniform3fv( U( p, "uCamPos" ), 1, fs.camPos );
		glUniform2f( U( p, "uViewport" ), float( std::max( 1, fs.width ) ), float( std::max( 1, fs.height ) ) );
		glUniform1f( U( p, "uProjY" ), P.m[ 5 ] );
		glUniform1f( U( p, "uWidthPx" ), std::max( 1.0f, L.width ) * float( std::max( 1, fs.height ) ) / 1080.0f );
		glUniform3fv( U( p, "uLineColor" ), 1, L.color );
		glUniform1f( U( p, "uColorMix" ), L.colorMix );
		glUniform1f( U( p, "uWeight" ), d.weight );
		glUniform1f( U( p, "uGain" ), gain );
		for( int k = 0; k < 3; ++k )
		{
			glActiveTexture( GL_TEXTURE0 + k );
			glBindTexture( GL_TEXTURE_BUFFER, c.tex[ k ] );
		}
	};
	glBindVertexArray( quadVAO );
	if( L.look == 1 || L.look == 3 )
	{
		common( progLinePair );
		glUniform1i( U( progLinePair, "uLook" ), L.look );
		glUniform1f( U( progLinePair, "uTube" ), L.amount );
		glUniform1f( U( progLinePair, "uReachW" ), L.reach * d.worldPerFitted );
		glUniform1fv( U( progLinePair, "uBands" ), 16, L.bands );
		glUniform1f( U( progLinePair, "uAudio" ), L.audio );
		glUniform1f( U( progLinePair, "uFlow" ), L.flow );
		glUniform1f( U( progLinePair, "uSpeed" ), L.speed );
		glUniform1f( U( progLinePair, "uFlicker" ), L.flicker );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_BUFFER, d.set->pairTex );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, GLsizei( d.set->lines ) );
		if( L.look == 1 )
		{
			// the nodes
			common( progLineDot );
			glEnable( GL_PROGRAM_POINT_SIZE );
			glDrawArrays( GL_POINTS, 0, GLsizei( d.set->count ) );
			glDisable( GL_PROGRAM_POINT_SIZE );
		}
	}
	else if( c.ringTex && c.ringFilled > 0 )
	{
		common( progLineRibbon );
		glUniform1i( U( progLineRibbon, "uRows" ), kRingRows );
		glUniform1i( U( progLineRibbon, "uHeads" ), int( c.ringHeads ) );
		glUniform1i( U( progLineRibbon, "uHeadRow" ), c.ringHead );
		glUniform1i( U( progLineRibbon, "uFilled" ), c.ringFilled );
		glUniform1f( U( progLineRibbon, "uNow" ), L.time );
		glUniform1f( U( progLineRibbon, "uTrail" ), L.reach );
		glUniform1fv( U( progLineRibbon, "uBands" ), 16, L.bands );
		glUniform1f( U( progLineRibbon, "uAudio" ), L.audio );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_BUFFER, c.ringTex );
		float loud = 0.0f; // the longest a band can stretch a ribbon this frame
		for( float b : L.bands )
			loud = std::max( loud, b );
		const int verts = 2 * ( std::min( c.ringFilled, int( std::ceil( L.reach * ( 1.0f + 0.5f * L.audio * loud ) * 60.0f ) ) + 2 ) + 1 );
		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, verts, GLsizei( c.ringHeads ) );
	}
	(void)glow;
	for( int k = 3; k >= 0; --k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
	}
	glBindVertexArray( 0 );
}

// neon: the tubes' light (the HDR wire buffer, blurred wide by Line Reach) added onto the model's picture: light at alpha 0
void Renderer::AddLineLight( const FrameState& fs, int w, int h )
{
	if( !wireGlowOk || lineDraws.empty() || fs.lines.look != 3 )
		return;
	const int bw = std::max( 1, w / 4 ), bh = std::max( 1, h / 4 );
	if( bw != lineBlurW || bh != lineBlurH || !lineBlurFBO[ 0 ] )
	{
		for( int i = 0; i < 2; ++i )
		{
			if( !lineBlur[ i ] )
				glGenTextures( 1, &lineBlur[ i ] );
			glBindTexture( GL_TEXTURE_2D, lineBlur[ i ] );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, bw, bh, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			if( !lineBlurFBO[ i ] )
				glGenFramebuffers( 1, &lineBlurFBO[ i ] );
			glBindFramebuffer( GL_FRAMEBUFFER, lineBlurFBO[ i ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lineBlur[ i ], 0 );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		lineBlurW = bw, lineBlurH = bh;
	}
	GLint target = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &target );
	glDisable( GL_BLEND );
	glBindVertexArray( quadVAO );
	const int taps     = 24;
	const float stride = ( 0.5f + 3.5f * fs.lines.reach ) * float( h ) / 1080.0f; // quarter-res texels per tap
	Blur1D( wireTex, lineBlurFBO[ 0 ], bw, bh, stride * 4.0f / float( w ), 0.0f, taps );
	Blur1D( lineBlur[ 0 ], lineBlurFBO[ 1 ], bw, bh, 0.0f, stride / float( bh ), taps );
	glBindFramebuffer( GL_FRAMEBUFFER, GLuint( target ) );
	glViewport( 0, 0, w, h );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	glUseProgram( progLineLight );
	glUniform1f( U( progLineLight, "uGain" ), 0.5f + 1.5f * fs.lines.amount );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, resolveTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, lineBlur[ 1 ] );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
}
}
