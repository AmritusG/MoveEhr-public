#pragma once
// W26 Stage 6: the firefly swarm. Transform feedback (as the W6 dust): state = position + velocity (world), each firefly owns a
// home point sampled on the mask (raw units: the dust sampler). Flocking without neighbour lists: every step the swarm is
// splatted into a 32^3 grid round the mask (additive points into a 2D atlas of z-slices, as the W20 sim: rgb = velocity sum,
// a = count). Alignment = steer to the cell's mean velocity; cohesion / separation = along the density gradient, toward the
// crowd when sparse, away when crowded; + curl-noise wander, + the pull home (Attraction, off while released).

namespace mehr
{
static const char* kSwarmGridLib = R"GLSL(
uniform vec3 uGridMin;
uniform float uGridCell;
const int GR = 32, GT = 6; // cells per edge, tiles per atlas row (atlas 192 x 192)
vec2 GridPix( ivec3 c ) { return vec2( ( c.z % GT ) * GR + c.x, ( c.z / GT ) * GR + c.y ) + 0.5; }
ivec3 GridCell( vec3 p ) { return ivec3( floor( ( p - uGridMin ) / uGridCell ) ); }
bool InGrid( ivec3 c ) { return all( greaterThanEqual( c, ivec3( 0 ) ) ) && all( lessThan( c, ivec3( GR ) ) ); }
)GLSL";

static const char* kSwarmSimVS = R"GLSL(
layout( location = 0 ) in vec3 aHome; // raw
layout( location = 1 ) in vec2 aUV;
layout( location = 2 ) in vec3 aPos;  // world
layout( location = 3 ) in vec3 aVel;
out vec3 oPos;
out vec3 oVel;
uniform sampler2D uGrid;
uniform mat4 uModel;
uniform vec3 uFitCenter;
uniform vec3 uCenter;   // the mask's centre, world
uniform float uUnit;    // world length of a mask unit
uniform float uDt, uAttract, uFlock, uWander, uOffset, uKick, uDrift;
uniform int uReset;
float H1( float n ) { return fract( sin( n * 12.9898 ) * 43758.5453 ); }
vec3 H3( float n ) { return fract( sin( vec3( n, n + 1.31, n + 2.73 ) * vec3( 12.9898, 78.233, 37.719 ) ) * 43758.5453 ); }
// smooth value noise on a 64-periodic lattice (the drift wraps at 64: seamless)
float Hh( vec3 p ) { p = fract( p * 0.3183099 + 0.1 ) * 17.0; return fract( p.x * p.y * p.z * ( p.x + p.y + p.z ) ); }
float NP( vec3 x )
{
	vec3 i = floor( x ), f = fract( x );
	f      = f * f * ( 3.0 - 2.0 * f );
	vec3 a = mod( i, 64.0 ), b = mod( i + 1.0, 64.0 );
	return mix( mix( mix( Hh( vec3( a.x, a.y, a.z ) ), Hh( vec3( b.x, a.y, a.z ) ), f.x ), mix( Hh( vec3( a.x, b.y, a.z ) ), Hh( vec3( b.x, b.y, a.z ) ), f.x ), f.y ),
	            mix( mix( Hh( vec3( a.x, a.y, b.z ) ), Hh( vec3( b.x, a.y, b.z ) ), f.x ), mix( Hh( vec3( a.x, b.y, b.z ) ), Hh( vec3( b.x, b.y, b.z ) ), f.x ), f.y ), f.z );
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
vec4 Cell( ivec3 c ) { return InGrid( c ) ? texelFetch( uGrid, ivec2( GridPix( c ) ), 0 ) : vec4( 0.0 ); }
void main()
{
	float id  = float( gl_VertexID );
	vec3 home = ( uModel * vec4( aHome, 1.0 ) ).xyz;
	vec3 out_ = normalize( home - uCenter + 1e-5 );
	home += out_ * uOffset * ( 0.6 + 0.8 * H1( id * 0.73 ) ); // Swarm Offset: a shell round the mask
	vec3 pos = aPos, vel = aVel;
	if( uReset == 1 )
	{
		oPos = home;
		oVel = vec3( 0.0 );
		return;
	}
	// flocking from the grid
	ivec3 c   = GridCell( pos );
	vec4 me   = Cell( c );
	vec3 grad = vec3( Cell( c + ivec3( 1, 0, 0 ) ).a - Cell( c - ivec3( 1, 0, 0 ) ).a, Cell( c + ivec3( 0, 1, 0 ) ).a - Cell( c - ivec3( 0, 1, 0 ) ).a,
	                  Cell( c + ivec3( 0, 0, 1 ) ).a - Cell( c - ivec3( 0, 0, 1 ) ).a );
	vec3 avg  = me.a > 0.5 ? me.rgb / me.a : vel;
	float crowd = clamp( me.a / 24.0, 0.0, 2.0 );
	vec3 acc  = uFlock * ( ( avg - vel ) * 1.5 + grad / max( length( grad ), 1.0 ) * uUnit * 2.0 * ( 1.0 - crowd ) );
	acc += Curl( pos / uUnit * 0.9 + vec3( 0.0, uDrift, 0.0 ) ) * uWander * uUnit * 1.6;
	// home: a spring (stiff, damped) while formed; the fireflies hover round their points
	vec3 toHome = home - pos;
	acc += toHome * uAttract * 6.0 - vel * ( 0.9 + uAttract * 1.4 );
	// a soft leash ~2.5 mask units out: a released swarm swirls round the mask instead of leaving the frame
	vec3 fromC = pos - uCenter;
	float rc   = length( fromC );
	acc -= fromC / max( rc, 1e-4 ) * max( rc - 2.5 * uUnit, 0.0 ) * 4.0;
	// release: one kick outward (each firefly its own way)
	vel += ( out_ * 0.9 + ( H3( id * 0.37 ) - 0.5 ) * 1.2 ) * uKick * uUnit;
	vel += acc * uDt;
	float vmax = 4.0 * uUnit, sp = length( vel );
	if( sp > vmax )
		vel *= vmax / sp;
	pos += vel * uDt;
	oPos = pos;
	oVel = vel;
}
)GLSL";

static const char* kSwarmSimFS = R"GLSL(
out vec4 fragColor;
void main() { fragColor = vec4( 0.0 ); }
)GLSL";

// splat: one point per firefly into its grid cell (additive): velocity sum + count
static const char* kSwarmSplatVS = R"GLSL(
layout( location = 2 ) in vec3 aPos;
layout( location = 3 ) in vec3 aVel;
out vec4 vV;
void main()
{
	ivec3 c = GridCell( aPos );
	vV      = vec4( aVel, 1.0 );
	gl_Position  = InGrid( c ) ? vec4( GridPix( c ) / vec2( float( GR * GT ) ) * 2.0 - 1.0, 0.0, 1.0 ) : vec4( 2.0, 2.0, 2.0, 1.0 );
	gl_PointSize = 1.0;
}
)GLSL";
static const char* kSwarmSplatFS = R"GLSL(
in vec4 vV;
out vec4 fragColor;
void main() { fragColor = vV; }
)GLSL";

// the fireflies: glowing points (core + soft halo), twinkling, additive after the composite, hidden behind the scene
static const char* kSwarmDrawVS = R"GLSL(
layout( location = 2 ) in vec3 aPos;
layout( location = 3 ) in vec3 aVel;
uniform mat4 uViewProj;
uniform float uSize;     // px
uniform float uTime;     // seconds, wrapped at kLoop
uniform float uTwinkle;
uniform vec3 uColor;
out vec3 vColor;
const float kLoop = 3600.0; // = kSwarmLoop in LoadEhrPlugin.cpp
float Cycles( float t, float perSec ) { return fract( t * ( floor( perSec * kLoop + 0.5 ) / kLoop ) ); }
vec3 H3( float n ) { return fract( sin( vec3( n, n + 1.31, n + 2.73 ) * vec3( 12.9898, 78.233, 37.719 ) ) * 43758.5453 ); }
void main()
{
	vec3 h  = H3( float( gl_VertexID ) * 0.618 );
	// a firefly's blink: mostly on, a soft dip once per 1-3 s each
	float b = 0.5 + 0.5 * sin( 6.2831853 * Cycles( uTime, 0.3 + 0.7 * h.x ) + h.y * 40.0 );
	float tw = mix( 1.0, smoothstep( 0.1, 0.9, b ), uTwinkle );
	vColor   = uColor * tw * ( 0.7 + 0.6 * h.z );
	vec4 c   = uViewProj * vec4( aPos, 1.0 );
	gl_Position  = c;
	gl_PointSize = uSize * ( 0.7 + 0.6 * h.z ) * 4.0;
}
)GLSL";
static const char* kSwarmDrawFS = R"GLSL(
in vec3 vColor;
out vec4 fragColor;
uniform sampler2D uDepth;
uniform vec2 uScreen;
void main()
{
	vec2 q  = gl_PointCoord * 2.0 - 1.0;
	float r = length( q ) * 4.0; // the sprite is 4 cores wide
	float k = exp( -2.0 * r * r ) * 2.0 + exp( -r * 1.6 ) * 0.25;
	float scene = texture( uDepth, gl_FragCoord.xy / uScreen ).r;
	if( gl_FragCoord.z > scene || k < 2e-3 )
		discard;
	fragColor = vec4( vColor * k, 0.0 ); // HDR, max-blended into a buffer, then added
}
)GLSL";
}
