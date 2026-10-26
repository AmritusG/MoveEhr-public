#pragma once
// W26 Stage 8: caustics mapping (Shah, Konttinen, Pattanaik 2007). The caustic light sits along the key direction. From it:
// the receivers' distance map (the opaque mask; glass lets light through), the refractor's world positions and shading
// normals (a 512^2 photon grid: glass parts, or the whole mask). Each photon refracts (+ a moving ripple), finds where it lands
// by iterating against the receiver map (d -> project -> read receiver -> d) or on the catcher plane, and is splatted as a
// Gaussian point into the caustic map, in light space (rgb; a = the receiver distance). The map is then a cookie light.

namespace mehr
{
static const char* kPhotonVS = R"GLSL(
uniform sampler2D uPhotonPos;
uniform sampler2D uPhotonNrm;
uniform sampler2D uRecv;     // R32F: distance from the light to the first opaque receiver (1e6 = none)
uniform mat4 uLVP;
uniform vec3 uLPos;
uniform float uIOR;
uniform vec4 uPlane;         // catcher plane n.xyz, d
uniform int uPlaneOn;
uniform float uSize;         // splat size, px
uniform float uEnergy;
uniform float uRipple, uPhase;
uniform vec3 uChannel;
out vec3 vC;
const int R = 512;
float Hp( vec3 p ) { p = fract( p * 0.3183099 + 0.1 ) * 17.0; return fract( p.x * p.y * p.z * ( p.x + p.y + p.z ) ); }
float NP( vec3 x )
{
	vec3 i = floor( x ), f = fract( x );
	f      = f * f * ( 3.0 - 2.0 * f );
	vec3 a = mod( i, 64.0 ), b = mod( i + 1.0, 64.0 );
	return mix( mix( mix( Hp( vec3( a.x, a.y, a.z ) ), Hp( vec3( b.x, a.y, a.z ) ), f.x ), mix( Hp( vec3( a.x, b.y, a.z ) ), Hp( vec3( b.x, b.y, a.z ) ), f.x ), f.y ),
	            mix( mix( Hp( vec3( a.x, a.y, b.z ) ), Hp( vec3( b.x, a.y, b.z ) ), f.x ), mix( Hp( vec3( a.x, b.y, b.z ) ), Hp( vec3( b.x, b.y, b.z ) ), f.x ), f.y ), f.z );
}
void main()
{
	ivec2 c = ivec2( gl_VertexID % R, gl_VertexID / R );
	vec4 P4 = texelFetch( uPhotonPos, c, 0 );
	gl_PointSize = uSize;
	vC = vec3( 0.0 );
	gl_Position  = vec4( 2.0, 2.0, 2.0, 1.0 );
	if( P4.a < 0.5 )
		return;
	vec3 P  = P4.xyz;
	vec3 N  = normalize( texelFetch( uPhotonNrm, c, 0 ).xyz );
	vec3 Ld = normalize( P - uLPos );
	if( uRipple > 0.0 )
	{
		// a moving ripple on the glass (the 64-periodic noise, drifting with uPhase)
		vec3 q = P * 2.5 + vec3( 0.0, uPhase, 0.0 );
		float n0 = NP( q );
		vec3 g = vec3( NP( q + vec3( 0.1, 0, 0 ) ), NP( q + vec3( 0, 0.1, 0 ) ), NP( q + vec3( 0, 0, 0.1 ) ) ) - n0;
		N = normalize( N + ( g - N * dot( g, N ) ) * 12.0 * uRipple );
	}
	if( dot( N, Ld ) > 0.0 )
		N = -N;
	vec3 T = refract( Ld, N, 1.0 / uIOR );
	if( dot( T, T ) < 0.5 )
		return;
	float cosi = max( dot( -Ld, N ), 0.0 );
	float F    = 0.04 + 0.96 * pow( 1.0 - cosi, 5.0 );
	// the receivers: iterate along the refracted ray against the distance map
	float d = 0.2;
	vec3 X  = P + T * d;
	float best = 1e9;
	for( int i = 0; i < 6; ++i )
	{
		X       = P + T * d;
		vec4 cx = uLVP * vec4( X, 1.0 );
		if( cx.w <= 1e-5 )
			break;
		vec2 uv = cx.xy / cx.w * 0.5 + 0.5;
		if( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThan( uv, vec2( 1.0 ) ) ) )
			break;
		float r = texture( uRecv, uv ).r;
		if( r > 1e5 )
			break;
		vec3 Rp = uLPos + normalize( X - uLPos ) * r;
		d       = length( Rp - P );
		if( i == 5 && length( P + T * d - Rp ) < 0.05 + 0.05 * d )
			best = d;
	}
	if( uPlaneOn == 1 )
	{
		float dn = dot( uPlane.xyz, T );
		if( dn < -1e-4 )
		{
			float tp = -( dot( uPlane.xyz, P ) + uPlane.w ) / dn;
			if( tp > 0.0 )
				best = min( best, tp );
		}
	}
	if( best > 1e8 )
		return;
	X           = P + T * best;
	gl_Position = uLVP * vec4( X, 1.0 );
	vC          = uChannel * uEnergy * ( 1.0 - F );
}
)GLSL";

static const char* kPhotonFS = R"GLSL(
in vec3 vC;
out vec4 fragColor;
void main()
{
	vec2 q = gl_PointCoord * 2.0 - 1.0;
	float k = exp( -4.0 * dot( q, q ) );
	fragColor = vec4( vC * k, 0.0 );
}
)GLSL";

// the receiver distance into the caustic map's alpha (colour mask A), its rgb cleared
static const char* kCausticInitFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uRecv;
uniform mat4 uInvLVP;
uniform vec3 uLPos;
uniform vec4 uPlane;
uniform int uPlaneOn;
void main()
{
	// the nearer of the mask and the catcher plane along this texel's ray from the light
	float r = texture( uRecv, vUV ).r;
	if( uPlaneOn == 1 )
	{
		vec4 f  = uInvLVP * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
		vec3 rd = normalize( f.xyz / f.w - uLPos );
		float dn = dot( uPlane.xyz, rd );
		if( dn < -1e-5 )
		{
			float t = -( dot( uPlane.xyz, uLPos ) + uPlane.w ) / dn;
			if( t > 0.0 )
				r = min( r, t );
		}
	}
	fragColor = vec4( 0.0, 0.0, 0.0, r );
}
)GLSL";
}
