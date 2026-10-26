#pragma once
// W26 Stage 5: lightning. Each bolt segment is one instanced quad, expanded in screen space round the projected segment by
// Bolt Width + its glow; the fragment's distance to the 2D segment gives a white-hot core and a coloured glow. Additive after
// the composite (light at alpha 0), hidden behind the scene by a manual compare with its depth (crawling bolts sit a hair
// out of the surface).

namespace mehr
{
static const char* kBoltVS = R"GLSL(
layout( location = 0 ) in vec4 aA; // world end a, brightness
layout( location = 1 ) in vec4 aB; // world end b, width factor
uniform mat4 uViewProj;
uniform vec2 uScreen;
uniform float uWidth;  // core width, px
uniform float uReach;  // quad half-width, px (core + glow)
out vec2 vA, vB;       // the segment in pixels
out float vBright, vW;
void main()
{
	vec4 ca = uViewProj * vec4( aA.xyz, 1.0 ), cb = uViewProj * vec4( aB.xyz, 1.0 );
	vBright = aA.w;
	vW      = uWidth * aB.w;
	if( ca.w < 0.05 || cb.w < 0.05 )
	{
		gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );
		vA = vB = vec2( 0.0 );
		return;
	}
	vec2 pa = ( ca.xy / ca.w * 0.5 + 0.5 ) * uScreen, pb = ( cb.xy / cb.w * 0.5 + 0.5 ) * uScreen;
	vA = pa, vB = pb;
	vec2 d  = pb - pa;
	float l = length( d );
	vec2 t  = l > 1e-3 ? d / l : vec2( 1.0, 0.0 );
	vec2 n  = vec2( -t.y, t.x );
	float r = uReach * aB.w;
	int c   = gl_VertexID; // 0..3 strip: (a, -), (a, +), (b, -), (b, +)
	vec2 p  = ( c < 2 ? pa - t * r : pb + t * r ) + n * ( ( c & 1 ) == 1 ? r : -r );
	vec4 cc = c < 2 ? ca : cb;
	gl_Position = vec4( ( p / uScreen * 2.0 - 1.0 ) * cc.w, cc.z, cc.w );
}
)GLSL";

static const char* kBoltFS = R"GLSL(
in vec2 vA, vB;
in float vBright, vW;
out vec4 fragColor;
uniform sampler2D uDepth;
uniform vec2 uScreen;
uniform vec3 uColor;  // glow colour x intensity
uniform float uGlow;
uniform float uP10, uP14; // projection terms: eye depth from the depth buffer
uniform float uTol;       // world: how far behind the surface a bolt still shows
void main()
{
	vec2 p  = gl_FragCoord.xy;
	vec2 d  = vB - vA;
	float h = clamp( dot( p - vA, d ) / max( dot( d, d ), 1e-6 ), 0.0, 1.0 );
	float x = length( p - vA - d * h ) / max( vW, 0.3 );
	float core = exp( -2.0 * x * x ), glow = uGlow * 0.3 * exp( -x * 0.25 ); // ~0 at the quad's edge (24 widths)
	// hidden behind the scene, softly: a bolt on a curved surface passes a little behind it (eye-space depths, world units)
	float scene = texture( uDepth, gl_FragCoord.xy / uScreen ).r;
	float zs = uP14 / ( ( 2.0 * scene - 1.0 ) + uP10 ), zb = uP14 / ( ( 2.0 * gl_FragCoord.z - 1.0 ) + uP10 ); // -eye z
	float vis = scene >= 1.0 ? 1.0 : 1.0 - smoothstep( uTol, 2.5 * uTol, zs - zb );
	if( vis <= 0.0 || core + glow < 1e-3 )
		discard;
	vec3 L = ( uColor * glow + mix( uColor, vec3( max( uColor.r, max( uColor.g, uColor.b ) ) ), 0.8 ) * core * 3.0 ) * vBright * vis;
	fragColor = vec4( L, 0.0 ); // HDR, into the bolt buffer with MAX blending (overlapping segments do not pile up)
}
)GLSL";

// the bolt buffer added onto the composite (light at alpha 0)
static const char* kBoltAddFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uBolts;
void main() { fragColor = vec4( ToDisplay( texture( uBolts, vUV ).rgb ), 0.0 ); }
)GLSL";
}
