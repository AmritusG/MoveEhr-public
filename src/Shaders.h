#pragma once
// GLSL 4.10 core sources. The main fragment stage is linked with two vertex stages: the indexed
// one (normal drawing) and the triangle-soup one (used only while Wireframe or Explode is non-zero).

namespace mehr
{
static const char* kMainVS = R"GLSL(
layout( location = 0 ) in vec3 aPos;
layout( location = 1 ) in vec2 aUV;
layout( location = 2 ) in vec4 aNormal;
layout( location = 3 ) in vec4 aTangent;
#ifdef ANIM
layout( location = 4 ) in vec3 aRest; // MoveEhr W1: the bind position: patterns / dissolve stay on the moving surface
#endif

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat3 uNormalMat;
uniform vec3 uFitCenter;
uniform float uFitScale;   // raw units -> fitted units (independent of Auto Fit)
uniform float uModelScale; // raw units -> world units along the model matrix (for displacement)
uniform sampler2D uTex4;   // height
uniform int uHas4;
uniform vec4 uSel4;
uniform float uHeightAmt;
uniform float uOutlineWidth; // > 0 only in the outline pass
// instances (clones / ghosts, program variants built with INSTANCED): matrix, hue offset, alpha
uniform int uInstBase;
uniform mat4 uInst[ 64 ];
uniform vec2 uInstHA[ 64 ]; // hue offset, alpha
flat out vec2 vInstHA;
#ifdef INSTANCED
uniform vec4 uInstX[ 64 ]; // keep (dissolve), fog keep, trail age (ghosts), -
flat out vec4 vInstX;
uniform vec3 uTrailDriftW; // ghost pass: where a fully dusty ghost has drifted at the trail's end (world)
uniform float uTrailDustV; // ghost pass: Trail Dust (0 for clones)
vec3 TrailDrift( float age ) { return uTrailDriftW * ( uTrailDustV * smoothstep( 0.05, 0.8, age ) * age ); }
// ghost pass, Trail Smear: vertices facing the way the mask moved slide toward the next newer pose, so each ghost
// stretches over the gap to the one in front of it (one continuous streak, no extra draws)
uniform mat4 uInstNext[ 64 ];
uniform float uTrailSmear;
vec3 TrailSmear( vec3 w, vec3 p, vec3 wn )
{
	vec3 mv = ( uInstNext[ uInstBase + gl_InstanceID ] * vec4( p, 1.0 ) ).xyz - w;
	return dot( wn, mv ) > 0.0 ? mv * uTrailSmear : vec3( 0.0 );
}
#endif
uniform float uGlitchAmt; // hologram glitch on the beat: horizontal slices jump sideways
uniform float uTime;
vec3 Glitch( vec3 w )
{
	float band = floor( w.y * 14.0 ) + floor( uTime * 9.0 ) * 7.13;
	float r    = fract( sin( band * 91.7 ) * 43758.5453 );
	if( r > 0.72 )
		w.x += ( r - 0.86 ) * uGlitchAmt * 0.8;
	return w;
}
#ifdef INSTTBO
// W9 Skin Of Copies: one copy per captured surface spot (units 33 / 34: world position, normal + rest height): standing on the surface
// (its up = the normal), turned round it by a hash, wobbling (Surf Speed), sized by uSkinSize (world per fitted unit of the copy)
uniform samplerBuffer uSkinPos, uSkinNrm;
uniform vec4 uSkinFit;     // the copy's fit: centre xyz (raw), scale (raw -> fitted)
uniform float uSkinSize;   // world size of a fitted unit of the copy
uniform float uSkinPhase;  // wobble cycles, wrapped at 1000 (each copy: whole cycles per 1000)
uniform float uSkinWobble; // radians
mat4 SkinMatrix( int i )
{
	vec3 P  = texelFetch( uSkinPos, i ).xyz;
	vec3 n  = normalize( texelFetch( uSkinNrm, i ).xyz + vec3( 1e-6 ) );
	float h = fract( sin( float( i ) * 12.9898 + 4.1 ) * 43758.5453 );
	vec3 t  = normalize( cross( n, abs( n.y ) < 0.9 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 ) ) );
	vec3 b  = cross( t, n );
	float a = 6.2831853 * h, c = cos( a ), s = sin( a );
	vec3 t2 = t * c + b * s, b2 = cross( t2, n );
	float rt = floor( ( 0.7 + 0.6 * fract( h * 7.31 ) ) * 1000.0 + 0.5 ) / 1000.0;
	float w  = uSkinWobble * sin( 6.2831853 * ( fract( uSkinPhase * rt ) + h ) );
	vec3 n2  = normalize( n * cos( w ) + t2 * sin( w ) ), t3 = normalize( cross( n2, b2 ) );
	float k  = uSkinSize * uSkinFit.w;
	mat3 R   = mat3( t3 * k, n2 * k, b2 * k );
	vec3 o   = P + n2 * uSkinSize - R * uSkinFit.xyz; // standing on the spot (the copy's fitted half height up the normal)
	return mat4( vec4( R[ 0 ], 0.0 ), vec4( R[ 1 ], 0.0 ), vec4( R[ 2 ], 0.0 ), vec4( o, 1.0 ) );
}
#endif

out VData
{
	vec3 wpos;
	vec3 wnrm;
	vec4 wtan;
	vec2 uv;
	vec3 opos;
	vec3 bary;
} vo;

void main()
{
	vec3 p = aPos;
	vec3 n = normalize( aNormal.xyz );
	if( uHas4 == 1 && uHeightAmt != 0.0 )
	{
		vec4 hs = textureLod( uTex4, aUV, 1.0 );
		float h = dot( hs, uSel4 );
		p += n * ( h - 0.5 ) * uHeightAmt / max( uModelScale, 1e-6 );
	}
#ifdef INSTANCED
	// clone / ghost: rigid transform + uniform scale, so mat3(M) is a valid normal matrix up to length
#ifdef INSTTBO
	mat4 M   = SkinMatrix( gl_InstanceID );
#else
	mat4 M   = uInst[ uInstBase + gl_InstanceID ];
#endif
	vec4 w   = M * vec4( p, 1.0 );
	vo.wnrm  = normalize( mat3( M ) * n );
	vo.wtan  = vec4( normalize( mat3( M ) * aTangent.xyz ), aTangent.w < 0.0 ? -1.0 : 1.0 );
#ifdef INSTTBO
	vInstHA  = vec2( 0.0, 1.0 );
	vInstX   = vec4( 1.0, 1.0, 0.0, 0.0 );
#else
	vInstHA  = uInstHA[ uInstBase + gl_InstanceID ];
	vInstX   = uInstX[ uInstBase + gl_InstanceID ];
#endif
	if( uTrailSmear > 0.0 )
		w.xyz += TrailSmear( w.xyz, p, vo.wnrm );
	w.xyz += TrailDrift( vInstX.z );
#else
	vec4 w   = uModel * vec4( p, 1.0 );
	vo.wnrm  = normalize( uNormalMat * n );
	vo.wtan  = vec4( normalize( mat3( uModel ) * aTangent.xyz ), aTangent.w < 0.0 ? -1.0 : 1.0 );
	vInstHA  = vec2( 0.0, 1.0 );
#endif
	if( uOutlineWidth > 0.0 )
		w.xyz += vo.wnrm * uOutlineWidth;
	if( uGlitchAmt > 0.0 )
		w.xyz = Glitch( w.xyz );
	vo.wpos     = w.xyz;
	vo.uv       = aUV;
#ifdef ANIM
	vo.opos     = ( aRest + ( p - aPos ) - uFitCenter ) * uFitScale;
#else
	vo.opos     = ( p - uFitCenter ) * uFitScale;
#endif
	vo.bary     = vec3( 1.0 );
	gl_Position = uViewProj * w;
}
)GLSL";

// W26 Stage 7: the lava crack network. A jittered 3D lattice in fitted object space (Crack Scale cells per mask unit); the
// crack = the Voronoi border, measured as the true distance to the bisector planes (two passes over 3^3 cells: Quilez).
// Linked only into the CRACKS / CRACK_PASS programs.
static const char* kCrackLib = R"GLSL(
uniform float uCrackScale;
uniform vec3 uCrackSeed;   // fitted: where the cracks start
uniform float uCrackFront; // fitted distance the cracks have spread
vec3 CrackHash( vec3 c ) { return fract( sin( vec3( dot( c, vec3( 127.1, 311.7, 74.7 ) ), dot( c, vec3( 269.5, 183.3, 246.1 ) ), dot( c, vec3( 113.5, 271.9, 124.6 ) ) ) ) * 43758.5453 ); }
// x = distance to the nearest Voronoi border (cell units), yzw = the nearest site (cell units)
vec4 CrackCell( vec3 q )
{
	vec3 i = floor( q ), f = q - i, mr = vec3( 0.0 ), mg = vec3( 0.0 );
	float md = 8.0;
	for( int k = 0; k < 27; ++k )
	{
		vec3 g = vec3( k % 3, ( k / 3 ) % 3, k / 9 ) - 1.0;
		vec3 r = g + 0.1 + 0.8 * CrackHash( i + g ) - f;
		float d = dot( r, r );
		if( d < md )
			md = d, mr = r, mg = g;
	}
	md = 8.0;
	for( int k = 0; k < 27; ++k )
	{
		vec3 g = mg + vec3( k % 3, ( k / 3 ) % 3, k / 9 ) - 1.0;
		vec3 r = g + 0.1 + 0.8 * CrackHash( i + g ) - f;
		if( dot( mr - r, mr - r ) > 1e-5 )
			md = min( md, dot( 0.5 * ( mr + r ), normalize( r - mr ) ) );
	}
	return vec4( md, i + f + mr );
}
)GLSL";

// Triangle-soup vertex stage for Wireframe / Explode: draws glDrawArrays(3 * triangles) and fetches
// the indexed vertices from texture-buffer views of the model's buffers, so each corner knows its
// whole triangle (face normal, centroid, barycentric) without a geometry shader.
static const char* kSoupVS = R"GLSL(
uniform usamplerBuffer uIdxBuf; // R32UI view of the index buffer
uniform usamplerBuffer uVtxBuf; // R32UI view of the vertex buffer, 7 words per vertex
uniform int uFirstIndex;
#ifdef WIRE
uniform usamplerBuffer uEdgeMask; // W5 Feature Edges: per triangle, bit k = draw the edge opposite corner k
uniform int uWireEdges;
flat out vec3 vEdge;
#endif
#ifdef POINTS
// W6 Points: vertex i = point i, a spot on a triangle (global triangle, barycentric b1, b2): the whole stage below runs at that spot,
// so the point rides its triangle through every deform, letter move, shard flight, clone and morph; drawn as a sprite
uniform samplerBuffer uPointRef;
uniform float uPointPx;      // sprite diameter, pixels
uniform float uPointLift;    // world: a hair toward the camera, so the solid's own depth (Back Hidden) does not eat the sprite
uniform float uTwinkle;      // Point Twinkle: flicker depth
uniform float uTwinklePhase; // twinkles, wrapped at 1000 (each point's rate is a whole number of twinkles per 1000)
uniform vec3 uCamPos;
flat out float vTwinkle;
#ifdef PTSHADE
// MoveEhr W7 Point Shading Per Point (PTSHADE twins): pass 1 shades every point ONCE, as a 1-px point on its own texel of the shade
// buffer (the main fragment stage, its surface part); pass 2 draws the sprites, each with its texel's colour (flat): the fragment stage
// then only cuts the shape and its alpha. A point's texel = instance x points + (vertex - first)
uniform int uPtPass;           // 1 shade, 2 sprites
uniform ivec4 uPtShade;        // first vertex of the draw, points per instance, buffer width, buffer height
uniform vec2 uPtShadeVP;       // the frame (px): pass 1's screen-space looks see the sprite centre's window position
uniform sampler2D uPtShadeTex; // unit 66: the shade buffer (pass 2; a dummy in pass 1)
flat out vec4 vPsC;            // pass 2: the point's shaded colour, its keep (< 0: a debug view)
flat out vec4 vPsFrag;         // pass 1: the window position of the point's sprite centre
#endif
#ifdef PTFX
// MoveEhr W4 (S7 / S8): the point looks. A separate twin (#define PTFX, linked on demand): Round + None + Size Var 0 draw through the
// W6 POINTS twins untouched. Every motion is a pure function of (Amount, the motion clock, the point's hash): scrubbing back retraces
uniform int uPtShape;      // 0 Round 1 Square 2 Diamond 3 Cross 4 Plus 5 Ring 6 Star 5 7 Star 6 8 Triangle 9 Hexagon 10 Heart 11 Spark
                           // 12 Soft Glow 13 Sphere 14 Cube 15 Glyph 16 Pixel 17 Bokeh
uniform int uPtMotion;     // 0 None 1 Drift 2 Curl Swirl 3 Gravity Fall 4 Attract 5 Repel 6 Orbit 7 Vortex 8 Wave 9 Jitter 10 Beat Burst
                           // 11 Trail Off 12 Wind 13 Breathe 14 Explode
uniform float uPtStr;      // the motion's strength: Amount x (1 + 3 Amount^3) (x4 at the top)
uniform float uPtDis;      // transient motions: how far (Speed 0: the strength; else the strength x the out-and-back cycle)
uniform float uPtPhase;    // the motion clock, turns wrapped at 1000 (per-point rates: whole cycles per 1000)
uniform float uPtBurst;    // Beat Burst: 1 = held (Speed 0), else the envelope's position in its beat (0 = the hit)
uniform float uPtSizeVar;  // Point Size Var
uniform vec3 uPtCentre;    // the model's centre (world)
uniform float uPtRadius;   // the fitted radius 1 in world units
uniform vec2 uPtViewport;  // pixels (Pixel snaps to them)
uniform int uPtTrailOn;    // Trail Off: the earlier pose is bound (unit 42)
uniform usamplerBuffer uTrailVtx; // unit 42: the slot's pose 0.35 s earlier (the posed layout, 7 words per vertex)
flat out vec4 vPtA;        // hash, the sprite px, the luma's share of the size variation, alpha
flat out vec4 vPtB;        // per-point rotation (Cube) / glyph cell, the bokeh brightness
vec4 PtHash( int i )
{
	uvec4 r = uvec4( uint( i ) ) * uvec4( 1597334677u, 3812015801u, 2798796415u, 2246822519u ) + uvec4( 12345u, 54321u, 777u, 99991u );
	r       = ( r ^ ( r >> 15 ) ) * 2654435769u;
	r       = r ^ ( r >> 13 );
	return vec4( r >> 8 ) / 16777216.0;
}
float PtRate( float h, float lo, float hi ) { return floor( ( lo + ( hi - lo ) * h ) * 1000.0 + 0.5 ) / 1000.0; } // whole cycles per 1000
vec3 PtTrail( int v )
{
	int b = v * 7;
	return vec3( uintBitsToFloat( texelFetch( uTrailVtx, b ).r ), uintBitsToFloat( texelFetch( uTrailVtx, b + 1 ).r ), uintBitsToFloat( texelFetch( uTrailVtx, b + 2 ).r ) );
}
vec3 PtRotY( vec3 d, float a ) { float c = cos( a ), s = sin( a ); return vec3( c * d.x + s * d.z, d.y, -s * d.x + c * d.z ); }
// the world offset of point i (home w, normal n); trailW = where its spot was 0.35 s ago minus where it is (world), alpha out
vec3 PtMotion( vec3 w, vec3 n, vec4 h, vec3 trailW, out float alpha )
{
	const float T = 6.2831853;
	alpha   = 1.0;
	float S = uPtStr, R = uPtRadius, ph = uPtPhase, d = uPtDis;
	vec3 rel = w - uPtCentre;
	if( uPtMotion == 1 ) // DRIFT
		return R * 0.15 * S * vec3( sin( T * ( ph * PtRate( h.x, 0.3, 0.8 ) + h.y ) ), 0.6 * sin( T * ( ph * PtRate( h.y, 0.3, 0.8 ) + h.z ) ),
		                            sin( T * ( ph * PtRate( h.z, 0.3, 0.8 ) + h.w ) ) );
	if( uPtMotion == 2 ) // CURL SWIRL: the curl of a moving sine field (divergence free: the points swirl, they do not bunch)
	{
		vec3 q = rel / R * 2.2, a = q * 1.7;
		float t = T * ph;
		vec3 c1 = vec3( cos( q.z + 2.0 * t ), cos( q.x + 3.0 * t ), cos( q.y + t ) );
		vec3 c2 = vec3( cos( a.y - t ), cos( a.z - 2.0 * t ), cos( a.x + 2.0 * t ) );
		return R * 0.12 * S * ( c1 + 0.6 * c2 );
	}
	if( uPtMotion == 3 ) // GRAVITY FALL: each point drops, fades and is born again at its spot
	{
		float u = fract( ph * PtRate( h.x, 0.5, 1.0 ) + h.y );
		float k = clamp( S * 4.0, 0.0, 1.0 );
		alpha   = mix( 1.0, smoothstep( 0.0, 0.06, u ) * ( 1.0 - smoothstep( 0.7, 1.0, u ) ), k );
		return vec3( ( h.z - 0.5 ) * 0.3 * R * S * u, -R * 2.2 * S * u * u, ( h.w - 0.5 ) * 0.3 * R * S * u );
	}
	if( uPtMotion == 4 ) // ATTRACT TO CENTRE (past 1: through the centre and out the other side)
		return -rel * 0.9 * d + R * 0.05 * d * vec3( sin( T * ( ph + h.x ) ), 0.0, cos( T * ( ph + h.x ) ) );
	if( uPtMotion == 5 ) // REPEL
		return normalize( rel + n * 0.2 * R + 1e-6 ) * R * 1.2 * d * ( 0.6 + 0.8 * h.x );
	if( uPtMotion == 6 ) // ORBIT: a share (Amount) of the points circles the up axis, 1 to 3 turns a cycle; past 1 the rings widen
	{
		if( h.y >= clamp( S * 1.25, 0.0, 1.0 ) )
			return vec3( 0.0 );
		vec3 o = PtRotY( rel, T * fract( ph * ( 1.0 + floor( h.z * 3.0 ) ) ) );
		o.xz *= 1.0 + 0.4 * max( S - 0.8, 0.0 );
		return o - rel;
	}
	if( uPtMotion == 7 ) // VORTEX: the floor plan turns, faster near the axis, and the points rise and fall round it
	{
		float r  = length( rel.xz ) / R;
		float th = T * fract( ph * PtRate( clamp( 1.0 / ( 0.5 + r ), 0.0, 2.0 ) * 0.5, 0.0, 2.0 ) );
		vec3 o   = mix( rel, PtRotY( rel, th ), clamp( S, 0.0, 1.0 ) );
		o.y += R * 0.25 * S * sin( th + T * h.x );
		return o - rel;
	}
	if( uPtMotion == 8 ) // WAVE ALONG SURFACE: along the normal, travelling up the body
		return n * R * 0.08 * S * sin( T * ( ph - 2.0 * rel.y / R ) );
	if( uPtMotion == 9 ) // JITTER: a new nudge 12 times a cycle
		return R * 0.06 * S * ( PtHash( int( floor( fract( ph ) * 12.0 ) ) * 7919 + int( h.x * 65536.0 ) ).xyz - 0.5 );
	if( uPtMotion == 10 ) // BEAT BURST: out on the hit, settling back over the beat (Speed 0: held by Amount)
	{
		float e = uPtBurst >= 1.0 ? 1.0 : exp( -uPtBurst * 6.0 );
		return normalize( rel + n * 0.3 * R + 1e-6 ) * R * 0.6 * S * e * ( 0.5 + h.x );
	}
	if( uPtMotion == 11 ) // TRAIL OFF: a share peels toward where its spot was (the pose 0.35 s earlier), each its own share of the lag
	{
		if( uPtTrailOn == 0 || h.y >= clamp( S * 1.5, 0.0, 1.0 ) )
			return vec3( 0.0 );
		return trailW * mix( 0.3, 1.0, h.x ) * 2.5 * S;
	}
	if( uPtMotion == 12 ) // WIND: gusts blow through along +X, the far side later; each point flutters
	{
		vec3 dir   = normalize( vec3( 1.0, 0.15, 0.3 ) );
		float gust = 0.5 + 0.5 * sin( T * ( 2.0 * ph - 0.5 * dot( rel, dir ) / R ) );
		return dir * R * 0.5 * S * ( 0.3 + 0.7 * h.x ) * gust + R * 0.04 * S * vec3( sin( T * ( ph * PtRate( h.y, 2.0, 4.0 ) + h.z ) ), 0.0, cos( T * ( ph * PtRate( h.y, 2.0, 4.0 ) + h.w ) ) );
	}
	if( uPtMotion == 13 ) // BREATHE
		return rel * 0.15 * S * sin( T * ph );
	if( uPtMotion == 14 ) // EXPLODE (+ return)
		return normalize( rel + n * 0.3 * R + ( h.xyz - 0.5 ) * 0.5 * R + 1e-6 ) * R * 1.5 * d * ( 0.5 + h.w );
	return vec3( 0.0 );
}
#endif
#endif
#ifdef CAPTURE
// W9 Stage A: the point's world position / normal / base colour into transform feedback (rasterizer off): the line looks read them.
// The spot runs the whole soup stage above, so it follows deforms, letters, shard flight and the morph exactly as its triangle does
uniform sampler2D uTex0;
uniform int uHas[ 7 ];
uniform vec4 uBaseFactor;
uniform float uCapTime;  // show time stamped into pos.w (light paint ring), seconds wrapped at 3600
uniform float uCapSwirl; // world radius of the light paint heads' circle in the tangent plane (0 = none)
uniform float uCapPhase; // turns, wrapped at 1000 (each head: a whole number of turns per 1000)
uniform int uCapHidden;  // this part is hidden: colour alpha 0 (lines touching it vanish)
uniform int uCapAxis;    // the rest height axis (Build / Deform axis) for the audio bands
out vec4 vCapPos, vCapNrm, vCapCol;
#endif

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat3 uNormalMat;
uniform vec3 uFitCenter;
uniform float uFitScale;
uniform float uModelScale;
uniform sampler2D uTex4;
uniform int uHas4;
uniform vec4 uSel4;
uniform float uHeightAmt;
uniform float uExplode;
uniform float uExplodeRandom;
uniform float uSeed;
uniform float uWorldScale;
// instances (clones / ghosts, program variants built with INSTANCED): matrix, hue offset, alpha
#ifdef DEFORM
uniform int uFoldN; // W2 Mirror Fold: every draw is uFoldN copies, the fold copy = gl_InstanceID % uFoldN
#define INST_ID ( gl_InstanceID / uFoldN )
#else
#define INST_ID gl_InstanceID
#endif
uniform int uInstBase;
uniform mat4 uInst[ 64 ];
uniform vec2 uInstHA[ 64 ]; // hue offset, alpha
flat out vec2 vInstHA;
#ifdef INSTANCED
uniform vec4 uInstX[ 64 ]; // keep (dissolve), fog keep, trail age (ghosts), -
flat out vec4 vInstX;
uniform vec3 uTrailDriftW; // ghost pass: where a fully dusty ghost has drifted at the trail's end (world)
uniform float uTrailDustV; // ghost pass: Trail Dust (0 for clones)
vec3 TrailDrift( float age ) { return uTrailDriftW * ( uTrailDustV * smoothstep( 0.05, 0.8, age ) * age ); }
// ghost pass, Trail Smear: vertices facing the way the mask moved slide toward the next newer pose, so each ghost
// stretches over the gap to the one in front of it (one continuous streak, no extra draws)
uniform mat4 uInstNext[ 64 ];
uniform float uTrailSmear;
vec3 TrailSmear( vec3 w, vec3 p, vec3 wn )
{
	vec3 mv = ( uInstNext[ uInstBase + INST_ID ] * vec4( p, 1.0 ) ).xyz - w;
	return dot( wn, mv ) > 0.0 ? mv * uTrailSmear : vec3( 0.0 );
}
#endif
uniform float uGlitchAmt; // hologram glitch on the beat: horizontal slices jump sideways
uniform float uTime;
vec3 Glitch( vec3 w )
{
	float band = floor( w.y * 14.0 ) + floor( uTime * 9.0 ) * 7.13;
	float r    = fract( sin( band * 91.7 ) * 43758.5453 );
	if( r > 0.72 )
		w.x += ( r - 0.86 ) * uGlitchAmt * 0.8;
	return w;
}

// ---- Stage 4 shape, in FITTED object space (the model spans -1..1 whatever its file units)
uniform int uShapeOn;
uniform usamplerBuffer uShardOf;  // per triangle: shard id
uniform samplerBuffer uShardData; // per shard: centroid (fitted) + seed
uniform int uShardsReady;
uniform float uShatterT;          // flight time in seconds, 0 = assembled
uniform float uShatterForce;
uniform float uShatterGravity;
uniform float uShardSpin;
#ifdef TRANS
uniform int uTransSide; // W16 Shatter transition (its own program): 1 the outgoing model, 2 the incoming one
uniform float uTransE, uTransStagger, uTransEase;
#endif
uniform float uMelt;
uniform float uDrip;
uniform int uSliceAxis;
uniform float uSliceCount;
uniform float uSliceGap;
uniform float uSliceWave;
uniform float uSlide; // transition Slice: every slab slides sideways by a staggered amount
uniform float uVoxel;
uniform float uVoxelSize;
uniform float uLowPoly;
uniform float uLowPolySize;
uniform mat3 uLowPolyGrid;  // mask (fitted) space -> the low-poly grid's space (a rotation)
// facet tumble: one facet per cell of the rotated grid, turning rigidly about the cell's centre
uniform int uTumble;        // 0 off, 1 together, 2 separate
uniform vec3 uTumbleAng;    // the steady turn, degrees (integrated, wrapped at 360)
uniform vec3 uTumblePh;     // the Sway rocking phase, degrees (integrated, wrapped at 360)
uniform float uTumbleRandom;
uniform float uTumbleSway;
uniform float uTumbleW;     // 1 = tumbling, gliding to 0 on Tumble Reset
uniform float uFacetGap;
uniform int uFlipOn;        // beat flips: base angle + one eased kick in progress
uniform float uFlipBase, uFlipT, uFlipDur, uFlipDeg;
uniform int uDeformAxis;
uniform float uTwist;
uniform float uBend;
uniform float uRipple;
uniform float uRippleFreq;
uniform float uRipplePhase;
uniform float uSlicePhase;  // Slice Wave, radians (wrapped)
#ifdef BOUNCE
uniform samplerBuffer uShardPath; // W26: per shard 241 frames x (offset, quaternion), fitted space
#endif
#ifdef CRACKS
uniform float uSplit; // W26 Lava Split (0..1)
#endif
#ifdef LETTERS
// W21 text / logos: every letter moves as one piece (Type On, Wave, Spin, Hop), before every other deform
uniform usamplerBuffer uLetterOf; // per triangle: letter id
uniform samplerBuffer uLetterXf;  // per letter, 3 texels: pivot (fitted) + scale, rotation (quaternion), offset (fitted)
#endif
#ifdef MORPH
// W6 Stage 2: the transition pack's vertex side (this program variant only). uMorphSide 1 = the hop's outgoing model, 2 = the
// incoming one; uMorphK = the style's progress on this side; Trans Stagger orders the pieces by a key, Trans Spread = how far / wide
uniform int uMorphVS;              // 2 pieces, 3 implode, 4 glitch, 5 ripple, 6 spin swap, 7 twist, 8 melt (W7: 1 shrinkwrap went)
uniform int uMorphSide;
uniform float uMorphK, uMorphE, uMorphStagger, uMorphSpread, uMorphScale, uMorphStep, uMorphFreq;
uniform mat4 uMorphFromOther;      // the other model's raw space -> this model's fitted space (both drawn at the frame's pose)
uniform samplerBuffer uPartnerData; // pieces: the other model's shard centroids (its fitted space) + seed (unit 28)
uniform samplerBuffer uRankSelf;   // per shard: (rank 0..1 bottom-up, the shard of rank i) of this model (unit 29) ...
uniform samplerBuffer uRankOther;  // ... and of the other one (unit 30)
uniform float uPartnerN;
uniform vec4 uOtherFit;            // the other model's fit: centre xyz, scale (raw -> fitted)
uniform float uMorphLead;          // W7 twist: which end winds first (> 0 the top, < 0 the bottom) and how far ahead (|lead| 0..1)
uniform vec2 uMorphRange;          // W7 twist: the model's extent along the deform axis (fitted min, max)
uniform vec4 uMorphPlane;          // W7 melt: the floor in fitted space: up (xyz), its height along up (w)
float MorphU( float k ) { return clamp( ( uMorphK - uMorphStagger * k ) / max( 1.0 - uMorphStagger, 1e-3 ), 0.0, 1.0 ); }
#endif

out VData
{
	vec3 wpos;
	vec3 wnrm;
	vec4 wtan;
	vec2 uv;
	vec3 opos;
	vec3 bary;
} vo;

vec4 UnpackSnorm( uint v )
{
	ivec4 i = ivec4( int( v << 22u ) >> 22, int( v << 12u ) >> 22, int( v << 2u ) >> 22, int( v ) >> 30 );
	return max( vec4( i ) / vec4( 511.0, 511.0, 511.0, 1.0 ), -1.0 );
}
vec3 FetchPos( int v )
{
	int b = v * 7;
	return vec3( uintBitsToFloat( texelFetch( uVtxBuf, b ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 1 ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 2 ).r ) );
}
vec2 FetchUV( int v )
{
	int b = v * 7;
	return vec2( uintBitsToFloat( texelFetch( uVtxBuf, b + 3 ).r ), uintBitsToFloat( texelFetch( uVtxBuf, b + 4 ).r ) );
}
vec3 FetchNrm( int v ) { return normalize( UnpackSnorm( texelFetch( uVtxBuf, v * 7 + 5 ).r ).xyz ); }
#ifdef ANIM
uniform usamplerBuffer uRestVtx; // MoveEhr W1: the bind vertices (the static buffer, unit 38): patterns / dissolve stay on the moving surface
vec3 FetchRest( int v )
{
	int b = v * 7;
	return vec3( uintBitsToFloat( texelFetch( uRestVtx, b ).r ), uintBitsToFloat( texelFetch( uRestVtx, b + 1 ).r ), uintBitsToFloat( texelFetch( uRestVtx, b + 2 ).r ) );
}
#endif
// MoveEhr W2 A3: a shard turns about its POSED centroid in the ANIM twins (unit 39, this frame's pose); the kick direction stays the bind
// centroid's. In the plain programs the macro is the bind centroid, token for token what was there (LOADEHR-003: preprocessor only)
#ifdef ANIM
uniform samplerBuffer uShardPivot;
#define SHARD_PIVOT( s, sd ) texelFetch( uShardPivot, int( s ) ).xyz
#else
#define SHARD_PIVOT( s, sd ) sd.xyz
#endif
float Hash1( float n ) { return fract( sin( n * 12.9898 + uSeed * 78.233 ) * 43758.5453 ); }
vec3 Hash3v( float n ) { return fract( sin( vec3( n, n + 1.7, n + 3.3 ) * vec3( 12.9898, 78.233, 37.719 ) ) * 43758.5453 ); }

// axis helpers: a = the chosen axis, (u, v) = the other two in cyclic order
vec3 ToAxis( vec3 f, int a ) { return a == 0 ? f.yzx : ( a == 1 ? f.zxy : f ); }
vec3 FromAxis( vec3 q, int a ) { return a == 0 ? q.zxy : ( a == 1 ? q.yzx : q ); }
mat3 RotAxis( vec3 k, float a )
{
	float c = cos( a ), s = sin( a );
	return mat3( c + k.x * k.x * ( 1.0 - c ), k.y * k.x * ( 1.0 - c ) + k.z * s, k.z * k.x * ( 1.0 - c ) - k.y * s,
	             k.x * k.y * ( 1.0 - c ) - k.z * s, c + k.y * k.y * ( 1.0 - c ), k.z * k.y * ( 1.0 - c ) + k.x * s,
	             k.x * k.z * ( 1.0 - c ) + k.y * s, k.y * k.z * ( 1.0 - c ) - k.x * s, c + k.z * k.z * ( 1.0 - c ) );
}
float VNoise( vec2 p )
{
	vec2 i = floor( p ), f = fract( p );
	f      = f * f * ( 3.0 - 2.0 * f );
	float a = fract( sin( dot( i, vec2( 127.1, 311.7 ) ) ) * 43758.5453 ), b = fract( sin( dot( i + vec2( 1, 0 ), vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
	float c = fract( sin( dot( i + vec2( 0, 1 ), vec2( 127.1, 311.7 ) ) ) * 43758.5453 ), d = fract( sin( dot( i + vec2( 1, 1 ), vec2( 127.1, 311.7 ) ) ) * 43758.5453 );
	return mix( mix( a, b, f.x ), mix( c, d, f.x ), f.y );
}

#ifdef MORPH
// W7 TWIST (uMorphVS 7): the model winds round the deform axis, the leading end first (uMorphLead): its ends turn +-uMorphScale
// radians at full wind (Trans Spread), wrung thin across the axis as it winds (uMorphFreq); uMorphK = the wind 0..1
vec3 MorphTwist( vec3 f )
{
	vec3 q    = ToAxis( f, uDeformAxis );
	float h   = clamp( ( q.z - uMorphRange.x ) / max( uMorphRange.y - uMorphRange.x, 1e-4 ), 0.0, 1.0 );
	float lag = min( abs( uMorphLead ), 0.95 );
	float u   = clamp( ( uMorphK - lag * ( uMorphLead >= 0.0 ? 1.0 - h : h ) ) / ( 1.0 - lag ), 0.0, 1.0 );
	float w   = u * u * ( 3.0 - 2.0 * u );
	float z   = h * 2.0 - 1.0;                         // -1 .. 1 over this model, whatever its proportions
	float a   = uMorphScale * z * w, c = cos( a ), s = sin( a );
	q.xy      = vec2( c * q.x - s * q.y, s * q.x + c * q.y ) * ( 1.0 - uMorphFreq * w * ( 1.0 - 0.5 * z * z ) );
	return FromAxis( q, uDeformAxis );
}
// W7 MELT (uMorphVS 8): sags, the drip columns (Drip, Trans Spread = runny) run ahead, every point lands in a puddle on the floor
// plane: a lens of radius uMorphScale (fitted) whose shape depends only on the direction and the radius share, so every model's
// puddle is the same and the swap is a cross-fade inside it. uMorphK = the melt 0..1 (1 = all puddle)
vec3 MorphMelt( vec3 f, out float u )
{
	vec3 up  = uMorphPlane.xyz;
	float y  = dot( f, up ) - uMorphPlane.w;           // height above the floor
	vec3 hz  = f - up * dot( f, up );                  // across the floor, from the model's centre
	float r  = length( hz );
	vec3 dir = r > 1e-5 ? hz / r : normalize( cross( up, abs( up.x ) < 0.9 ? vec3( 1, 0, 0 ) : vec3( 0, 0, 1 ) ) );
	float dn = pow( VNoise( f.xz * 7.0 + 3.1 ), 3.0 ) * uDrip * 2.5;
	u        = clamp( uMorphK * ( 1.0 + dn * ( 0.4 + 0.6 * uMorphSpread ) ), 0.0, 1.0 );
	// the puddle: the angle round the centre wobbles the rim, the radius share sets the thickness (a lens), both model-free
	vec3 side  = normalize( cross( up, abs( up.y ) < 0.9 ? vec3( 0, 1, 0 ) : vec3( 1, 0, 0 ) ) );
	float ang  = atan( dot( dir, cross( up, side ) ), dot( dir, side ) );
	float rim  = 1.0 + 0.12 * sin( ang * 5.0 + 1.3 ) + 0.07 * sin( ang * 11.0 + 4.1 );
	float rs   = sqrt( min( r, 1.0 ) );
	float pr   = uMorphScale * rs * rim;
	float py   = 0.015 + 0.09 * ( 1.0 - rs * rs ) / max( uMorphScale, 0.5 );
	float fall = u * u;                                 // gravity: slow, then fast
	float flow = smoothstep( 0.45, 1.0, u );            // the spread happens on the floor
	float yy   = mix( y, py, fall );
	vec3 hh    = mix( hz, dir * pr, flow );
	hh *= 1.0 + 0.25 * sin( 3.14159265 * u ) * u * ( 1.0 - flow ); // the sag bulges out on the way down (from rest gently: x u)
	return hh + up * ( yy + uMorphPlane.w );
}
vec3 MorphField( vec3 f )
{
	float u;
	return uMorphVS == 7 ? MorphTwist( f ) : MorphMelt( f, u );
}
#endif
#ifdef DEFORM
// W2 Deform group (this program variant only: with every deform at 0 the plain programs draw). Fitted space; the fixed order:
// Taper, Twist, Bend, Squash, Shear, Bulge, Spherize, Noise Warp, Ripple, Melt / Drip, Glitch, Mirror Fold, Jelly. The values
// arrive already scaled by Deform Amount (+ Audio > Deform).
uniform float uTaper, uSquash, uShear, uBulge, uSpherize;
uniform int uShapeTarget; // W9: what Spherize morphs toward: 0 sphere, 1 cube (W11: torus / crystal retired)
uniform float uWarp, uWarpScale, uWarpPhase;       // the noise drifts at Warp Speed lattice cells / s (phase wraps at 256)
uniform float uGlitchShift, uGlitchBands, uGlitchStep; // the step index wraps at 1000 (a wrap = one more random step)
uniform vec3 uFoldAxes;                           // 1 = folded across that fitted centre plane
uniform float uFoldK;                             // 0..1: how far folded (Deform Amount)
uniform vec3 uJellyLin, uJellyAng;                // the jelly's lag: offset and rotation (axis x angle) at a fitted radius of 1
uniform float uJellyScale;
// W9: the deform state that changes with time, as globals: main() sets them from the uniforms, or (Slit-Scan) from the history at this
// vertex's delay
float gWarpPhase = 0.0, gRipplePhase = 0.0, gGlitchStep = 0.0, gSquash = 0.0, gJellyScale = 0.0;
vec3 gJellyLin = vec3( 0.0 ), gJellyAng = vec3( 0.0 );
// W9 Build (the looks in the DEFORM twins; uXLook 0 = none of them)
uniform int uXLook;            // 1 slit-scan, 2 print on, 3 exploded, 4 black hole
uniform int uXAxis;            // Build Axis
uniform float uXAmt;           // the look's progress / strength (Build Amount, run by Build Speed / On Beat)
uniform float uXSwirl;         // black hole: turns round the axis at the full fall
uniform samplerBuffer uXGroup; // exploded: per triangle its group's centre (fitted), unit 31
uniform samplerBuffer uXHist;  // slit-scan: newest first, one entry per 1/60 s: pose (4 texels, columns), phases, jelly (2), unit 32
uniform mat4 uXPoseInv;        // slit-scan: the pose now, inverted
uniform float uXHistN;         // entries in the history
uniform float uXSlitPhase;     // slit-scan waves (Build Speed), cycles, wrapped at 1 (< 0: no waves, the delay grows along the axis)
out float vXFall;              // black hole: how far this point has fallen (0..1)
float gXFall = 0.0;
mat3 gXRot = mat3( 1.0 );      // slit-scan: the past pose's turn (normals, tangents)
// BLACK HOLE: every point spirals into the centre round the Build Axis, the near points first (so the model stretches as it falls),
// shrinking to nothing; slider-driven (scrubbing back spits it out along the same path)
vec3 BlackHole( vec3 f )
{
	vec3 q   = ToAxis( f, uXAxis );
	float r  = length( f );
	float u  = clamp( uXAmt * 2.2 - r / 1.6, 0.0, 1.0 );
	u        = u * u * ( 3.0 - 2.0 * u );
	gXFall   = max( gXFall, u );
	float rr = pow( 1.0 - u, 1.6 );
	float a  = 6.2831853 * uXSwirl * u * u * ( 1.0 + 0.6 / ( r * rr + 0.15 ) );
	float c = cos( a ), s = sin( a );
	q.xy = vec2( c * q.x - s * q.y, s * q.x + c * q.y );
	q *= rr;
	q.z *= 1.0 - 0.6 * u * ( 1.0 - u ) * 4.0 * 0.5; // flattened toward the disc on the way in
	return FromAxis( q, uXAxis );
}
// W9 SHAPE MORPH: the point's place on the cube, along its direction from the centre (onto the faces; W11: torus / crystal retired)
vec3 ShapeTarget( vec3 f )
{
	vec3 q = ToAxis( f, uDeformAxis ) + vec3( 1e-6 );
	q      = q / max( abs( q.x ), max( abs( q.y ), abs( q.z ) ) ) * 0.72;
	return FromAxis( q, uDeformAxis );
}
vec3 gFoldS = vec3( 1.0 );                        // this copy's side per folded axis (+1 / -1)
bool gFoldFlip = false;                           // this triangle is reflected an odd number of times (winding reversed)
bool gNormalPass = false;                         // the normal's finite differences: no Glitch (a band's jump is a translation)
float DHash( vec3 c )
{
	c = mod( c, 64.0 ); // 64-periodic lattice: the drift wraps seamlessly
	return fract( sin( dot( c, vec3( 127.1, 311.7, 74.7 ) ) ) * 43758.5453 );
}
float DNoise( vec3 p )
{
	vec3 i = floor( p ), f = fract( p );
	f      = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( mix( DHash( i ), DHash( i + vec3( 1, 0, 0 ) ), f.x ), mix( DHash( i + vec3( 0, 1, 0 ) ), DHash( i + vec3( 1, 1, 0 ) ), f.x ), f.y ),
	            mix( mix( DHash( i + vec3( 0, 0, 1 ) ), DHash( i + vec3( 1, 0, 1 ) ), f.x ), mix( DHash( i + vec3( 0, 1, 1 ) ), DHash( i + vec3( 1, 1, 1 ) ), f.x ), f.y ), f.z );
}
vec3 DeformPre( vec3 f, vec3 n )
{
	vec3 q = ToAxis( f, uDeformAxis ); // q.z runs along the axis
	q.xy *= max( 1.0 + uTaper * q.z * 0.5, 0.0 );
	if( uTwist != 0.0 )
	{
		float a = uTwist * q.z, c = cos( a ), s = sin( a );
		q.xy    = vec2( c * q.x - s * q.y, s * q.x + c * q.y );
	}
	if( abs( uBend ) > 1e-4 )
	{
		float R  = 1.0 / uBend;
		float th = uBend * q.z;
		q        = vec3( R - ( R - q.x ) * cos( th ), q.y, ( R - q.x ) * sin( th ) );
	}
	float sa = exp( -gSquash ); // squash: shorter along the axis, wider across it (volume kept)
	q.z *= sa;
	q.xy *= inversesqrt( sa );
	q.x += uShear * q.z * 0.5;
	q.xy *= 1.0 + uBulge * 0.6 * max( 1.0 - q.z * q.z, 0.0 ); // round the middle of the axis out (negative: pinch)
	f = FromAxis( q, uDeformAxis );
	if( uSpherize > 0.0 )
		f = mix( f, uShapeTarget == 0 ? normalize( f + vec3( 1e-6 ) ) * 0.8 : ShapeTarget( f ), uSpherize );
	if( uWarp > 0.0 )
	{
		vec3 p = f * uWarpScale + gWarpPhase * vec3( 1.0, 0.5, 0.25 );
		f += uWarp * 0.5 * ( vec3( DNoise( p ), DNoise( p + vec3( 17.3, 9.1, 31.7 ) ), DNoise( p + vec3( 41.9, 23.3, 7.7 ) ) ) - 0.5 );
	}
	if( uRipple > 0.0 )
		f += n * uRipple * sin( uRippleFreq * length( f ) - gRipplePhase );
	if( uMelt > 0.0 )
	{
		float h    = f.y + 1.0;
		float drip = pow( VNoise( f.xz * 7.0 + 3.1 ), 3.0 ) * uDrip * 2.5;
		float sag  = uMelt * h * ( 0.45 + drip ) * ( 0.6 + 0.4 * VNoise( f.xz * 2.3 ) );
		f.y -= sag;
		float floorY = -1.0 - uMelt * 0.15;
		float under  = max( floorY - f.y, 0.0 );
		f.y          = max( f.y, floorY - under * 0.08 );
		f.xz *= 1.0 + uMelt * 0.35 * smoothstep( 0.4, 0.0, f.y - floorY );
	}
	if( uGlitchShift > 0.0 && !gNormalPass )
	{
		// bands across the axis jump sideways on a stepped clock (a hash per band and step: still while the clock is paused)
		vec3 g     = ToAxis( f, uDeformAxis );
		float band = floor( ( g.z + 1.0 ) * 0.5 * uGlitchBands );
		float h    = fract( sin( band * 91.7 + gGlitchStep * 7.13 ) * 43758.5453 );
		float h2   = fract( sin( band * 17.3 + gGlitchStep * 3.71 + 5.0 ) * 24634.6345 );
		if( h > 0.55 )
			g.x += ( h2 - 0.5 ) * uGlitchShift;
		f = FromAxis( g, uDeformAxis );
	}
	return f;
}
vec3 DeformPost( vec3 f )
{
	if( uFoldK > 0.0 ) // kaleidoscope: this copy keeps its own side of each folded centre plane
		f = mix( f, mix( f, gFoldS * abs( f ), uFoldAxes ), uFoldK );
	float w = min( length( f ), 1.5 ); // the jelly: the far parts lag the most
	float a = length( gJellyAng );
	if( a > 1e-6 )
		f = RotAxis( gJellyAng / a, a * w ) * f;
	f = f * ( 1.0 + gJellyScale * w ) + gJellyLin * w;
	return uXLook == 4 ? BlackHole( f ) : f;
}
#endif
// smooth deforms of one corner (fitted space)
vec3 Continuous( vec3 f, vec3 n )
{
#ifdef DEFORM
	f = DeformPost( DeformPre( f, n ) );
#else
	if( uTwist != 0.0 || uBend != 0.0 )
	{
		vec3 q = ToAxis( f, uDeformAxis ); // q.z runs along the axis
		if( uTwist != 0.0 )
		{
			float a = uTwist * q.z, c = cos( a ), s = sin( a );
			q.xy    = vec2( c * q.x - s * q.y, s * q.x + c * q.y );
		}
		if( abs( uBend ) > 1e-4 )
		{
			// Barr bend of the axis toward q.x, radius 1/bend
			float R  = 1.0 / uBend;
			float th = uBend * q.z;
			q        = vec3( R - ( R - q.x ) * cos( th ), q.y, ( R - q.x ) * sin( th ) );
		}
		f = FromAxis( q, uDeformAxis );
	}
	if( uRipple > 0.0 )
		f += n * uRipple * sin( uRippleFreq * length( f ) - uRipplePhase );
	if( uMelt > 0.0 )
	{
		float h    = f.y + 1.0;                                         // 0 at the bottom, 2 at the top
		float drip = pow( VNoise( f.xz * 7.0 + 3.1 ), 3.0 ) * uDrip * 2.5; // some columns run much further
		float sag  = uMelt * h * ( 0.45 + drip ) * ( 0.6 + 0.4 * VNoise( f.xz * 2.3 ) );
		f.y -= sag;
		float floorY = -1.0 - uMelt * 0.15;
		float under  = max( floorY - f.y, 0.0 );
		f.y          = max( f.y, floorY - under * 0.08 );
		f.xz *= 1.0 + uMelt * 0.35 * smoothstep( 0.4, 0.0, f.y - floorY ); // puddle spreading on the floor
	}
#endif
	if( uVoxel > 0.0 )
		f = mix( f, ( floor( f / uVoxelSize ) + 0.5 ) * uVoxelSize, uVoxel );
	return f;
}

// ---- low poly: corners snap to the rotated grid; each facet (the grid cell holding the triangle's centroid)
// can shrink toward its centre (Facet Gap) and tumble about it
vec4 QAxis( vec3 a, float ang ) { return vec4( a * sin( ang * 0.5 ), cos( ang * 0.5 ) ); }
vec4 QMul( vec4 a, vec4 b ) { return vec4( a.w * b.xyz + b.w * a.xyz + cross( a.xyz, b.xyz ), a.w * b.w - dot( a.xyz, b.xyz ) ); }
vec3 QRot( vec4 q, vec3 v )
{
	vec3 t = 2.0 * cross( q.xyz, v );
	return v + q.w * t + cross( q.xyz, t );
}
vec4 QEuler( vec3 deg )
{
	vec3 r = radians( deg );
	return QMul( QAxis( vec3( 0, 0, 1 ), r.z ), QMul( QAxis( vec3( 0, 1, 0 ), r.y ), QAxis( vec3( 1, 0, 0 ), r.x ) ) );
}
vec4 QNlerp( vec4 a, vec4 b, float t ) { return normalize( mix( a, dot( a, b ) < 0.0 ? -b : b, t ) ); }
// steady turn + rocking about the rest pose (Sway 1 = rocking only); a facet's whole-number speed multiplies both
vec3 Sway( vec3 a, vec3 ph ) { return a + uTumbleSway * 35.0 * sin( radians( ph ) ); }

struct Facet
{
	vec3 c;  // centre (fitted space)
	vec4 q;  // rotation about it
};
Facet MakeFacet( vec3 mid )
{
	vec3 cell = floor( ( uLowPolyGrid * mid ) / uLowPolySize );
	Facet fc;
	fc.c = transpose( uLowPolyGrid ) * ( ( cell + 0.5 ) * uLowPolySize );
	fc.q = vec4( 0.0, 0.0, 0.0, 1.0 );
	if( uTumble == 0 && uFlipOn == 0 )
		return fc;
	float id = dot( cell, vec3( 1.0, 57.0, 113.0 ) );
	vec3 h   = Hash3v( id * 0.1337 + 3.7 ), h2 = Hash3v( id * 0.0731 + 11.3 );
	if( uTumble == 1 )
		fc.q = QEuler( Sway( uTumbleAng, uTumblePh ) );
	else if( uTumble == 2 )
	{
		// own axis frame and whole-number speed factors (-2, -1, 1, 2 per axis), so the angles wrap seamlessly;
		// Tumble Random blends from Together (0) to fully separate (1)
		vec3 v   = floor( h * 4.0 );
		vec3 sp  = v - 2.0 + step( 2.0, v );
		vec4 Q   = QAxis( normalize( h2 - 0.5 + 1e-4 ), h2.x * 6.2831853 );
		vec4 qS  = QMul( Q, QMul( QEuler( Sway( uTumbleAng * sp, uTumblePh * sp ) ), vec4( -Q.xyz, Q.w ) ) );
		fc.q     = QNlerp( QEuler( Sway( uTumbleAng, uTumblePh ) ), qS, uTumbleRandom );
	}
	if( uFlipOn == 1 )
	{
		bool sep    = uTumble == 2;
		float delay = sep ? h.z * 0.6 * uFlipDur * uTumbleRandom : 0.0;
		float u     = clamp( ( uFlipT - delay ) / max( uFlipDur, 1e-3 ), 0.0, 1.0 );
		u           = u * u * ( 3.0 - 2.0 * u );
		vec3 ax     = vec3( 1.0, 0.0, 0.0 );
		if( sep && h2.y < uTumbleRandom )
			ax = h2.z < 0.333 ? vec3( 1.0, 0.0, 0.0 ) : ( h2.z < 0.667 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 0.0, 0.0, 1.0 ) );
		fc.q = QMul( QAxis( ax, radians( uFlipBase + uFlipDeg * u ) ), fc.q );
	}
	fc.q = QNlerp( vec4( 0.0, 0.0, 0.0, 1.0 ), fc.q, uTumbleW );
	return fc;
}
vec3 LowPoly( vec3 f, Facet fc )
{
	vec3 q = uLowPolyGrid * f;
	q      = floor( q / uLowPolySize + 0.5 ) * uLowPolySize;
	f      = mix( f, transpose( uLowPolyGrid ) * q, uLowPoly );
	return fc.c + QRot( fc.q, ( f - fc.c ) * ( 1.0 - 0.9 * uFacetGap ) );
}

void main()
{
#ifdef POINTS
	vec4 pref  = texelFetch( uPointRef, gl_VertexID );
	int tri    = int( pref.x );
	int corner = 0;
	int base   = tri * 3;
#else
	int tri    = gl_VertexID / 3;
	int corner = gl_VertexID - tri * 3;
	int base   = uFirstIndex + tri * 3;
#endif
	int i0 = int( texelFetch( uIdxBuf, base ).r ), i1 = int( texelFetch( uIdxBuf, base + 1 ).r ), i2 = int( texelFetch( uIdxBuf, base + 2 ).r );
#if defined( WIRE ) && !defined( POINTS )
	uint em = uWireEdges == 1 ? texelFetch( uEdgeMask, base / 3 ).r : 7u;
	vEdge   = vec3( float( em & 1u ), float( ( em >> 1u ) & 1u ), float( ( em >> 2u ) & 1u ) );
#elif defined( WIRE )
	vEdge   = vec3( 1.0 );
#endif
#ifdef DEFORM
	if( uFoldN > 1 )
	{
		// Mirror Fold: copy c keeps the side bit k of c says on its k-th folded axis
		int c = gl_InstanceID % uFoldN, k = 0;
		for( int a = 0; a < 3; ++a )
			if( uFoldAxes[ a ] > 0.5 )
				gFoldS[ a ] = ( ( c >> k++ ) & 1 ) == 1 ? -1.0 : 1.0;
		if( uFoldK > 0.5 )
		{
			// a triangle reflected an odd number of times turns its winding: swap two corners so it stays front-facing
			vec3 c3 = ( DeformPre( ( FetchPos( i0 ) - uFitCenter ) * uFitScale, FetchNrm( i0 ) ) + DeformPre( ( FetchPos( i1 ) - uFitCenter ) * uFitScale, FetchNrm( i1 ) ) +
			            DeformPre( ( FetchPos( i2 ) - uFitCenter ) * uFitScale, FetchNrm( i2 ) ) ) / 3.0;
			vec3 r     = step( 0.5, uFoldAxes ) * step( 0.0, -gFoldS * c3 );
			gFoldFlip  = mod( r.x + r.y + r.z, 2.0 ) > 0.5;
#ifndef POINTS
			if( gFoldFlip )
			{
				int sw = i1;
				i1 = i2, i2 = sw;
			}
#endif
		}
	}
#endif
	int me = corner == 0 ? i0 : ( corner == 1 ? i1 : i2 );

	int b   = me * 7;
	vec3 p  = FetchPos( me );
	vec2 uv = FetchUV( me );
	vec3 n  = FetchNrm( me );
	vec4 t  = UnpackSnorm( texelFetch( uVtxBuf, b + 6 ).r );
#ifdef POINTS
	{
		vec3 bw3 = vec3( 1.0 - pref.y - pref.z, pref.y, pref.z );
		p        = FetchPos( i0 ) * bw3.x + FetchPos( i1 ) * bw3.y + FetchPos( i2 ) * bw3.z;
		uv       = FetchUV( i0 ) * bw3.x + FetchUV( i1 ) * bw3.y + FetchUV( i2 ) * bw3.z;
		n        = normalize( FetchNrm( i0 ) * bw3.x + FetchNrm( i1 ) * bw3.y + FetchNrm( i2 ) * bw3.z + vec3( 1e-6 ) );
	}
#endif
#ifdef ANIM
	vec3 aRestP = FetchRest( me );
#ifdef POINTS
	aRestP = FetchRest( i0 ) * ( 1.0 - pref.y - pref.z ) + FetchRest( i1 ) * pref.y + FetchRest( i2 ) * pref.z;
#endif
	vec3 aPosed = p;
#endif

	if( uHas4 == 1 && uHeightAmt != 0.0 )
	{
		float h = dot( textureLod( uTex4, uv, 1.0 ), uSel4 );
		p += n * ( h - 0.5 ) * uHeightAmt / max( uModelScale, 1e-6 );
	}
#ifdef ANIM
	vec3 aOpos = ( aRestP + ( p - aPosed ) - uFitCenter ) * uFitScale;
#endif
	vec3 o0 = FetchPos( i0 ), o1 = FetchPos( i1 ), o2 = FetchPos( i2 );
	vec3 wn = normalize( uNormalMat * n );
	vec3 nObj = n;
	vec3 opos = ( p - uFitCenter ) * uFitScale;
#ifdef DEFORM
	gWarpPhase = uWarpPhase, gRipplePhase = uRipplePhase, gGlitchStep = uGlitchStep, gSquash = uSquash;
	gJellyLin = uJellyLin, gJellyAng = uJellyAng, gJellyScale = uJellyScale;
	vXFall = 0.0;
	mat4 xPast = mat4( 1.0 );
	if( uXLook == 1 && uXHistN > 1.5 )
	{
		// SLIT-SCAN: this point shows the model as it was Build Amount x up to 2 s ago, the delay growing along the Build Axis (Build Speed:
		// waves of delay travel along it); smooth in the height, so no tearing between slices
		float h  = clamp( ( ToAxis( opos, uXAxis ).z + 1.0 ) * 0.5, 0.0, 1.0 );
		float k  = uXSlitPhase >= 0.0 ? 0.5 - 0.5 * cos( 6.2831853 * ( h - uXSlitPhase ) ) : h;
		float e  = clamp( uXAmt * 2.0 * k * 60.0, 0.0, uXHistN - 1.001 );
		int i0   = int( floor( e ) ), i1 = i0 + 1;
		float fr = e - float( i0 );
		for( int c = 0; c < 4; ++c )
			xPast[ c ] = mix( texelFetch( uXHist, i0 * 8 + c ), texelFetch( uXHist, i1 * 8 + c ), fr );
		vec4 ph = mix( texelFetch( uXHist, i0 * 8 + 4 ), texelFetch( uXHist, i1 * 8 + 4 ), fr );
		vec4 j0 = mix( texelFetch( uXHist, i0 * 8 + 5 ), texelFetch( uXHist, i1 * 8 + 5 ), fr );
		vec4 j1 = mix( texelFetch( uXHist, i0 * 8 + 6 ), texelFetch( uXHist, i1 * 8 + 6 ), fr );
		// a phase that wrapped between the two samples is taken from the nearer one (no blend across the wrap)
		vec4 pn = texelFetch( uXHist, ( fr < 0.5 ? i0 : i1 ) * 8 + 4 );
		gWarpPhase = abs( texelFetch( uXHist, i0 * 8 + 4 ).x - texelFetch( uXHist, i1 * 8 + 4 ).x ) > 1.0 ? pn.x : ph.x;
		gRipplePhase = abs( texelFetch( uXHist, i0 * 8 + 4 ).y - texelFetch( uXHist, i1 * 8 + 4 ).y ) > 1.0 ? pn.y : ph.y;
		gGlitchStep = pn.z, gSquash = ph.w;
		gJellyLin = j0.xyz, gJellyScale = j0.w, gJellyAng = j1.xyz;
		xPast = xPast * uXPoseInv; // world now -> world then
		gXRot = mat3( xPast );
	}
#endif
#ifdef LETTERS
	vec3 lopos = opos; // patterns / dissolve stay stuck to the letter
	{
		int lid = int( texelFetch( uLetterOf, base / 3 ).r );
		vec4 lc = texelFetch( uLetterXf, lid * 3 ), lq = texelFetch( uLetterXf, lid * 3 + 1 ), lo = texelFetch( uLetterXf, lid * 3 + 2 );
#define LETTER( r ) ( ( lc.xyz + lo.xyz + QRot( lq, ( ( (r) - uFitCenter ) * uFitScale - lc.xyz ) * lc.w ) ) / uFitScale + uFitCenter )
		p  = LETTER( p );
		o0 = LETTER( o0 ), o1 = LETTER( o1 ), o2 = LETTER( o2 );
#undef LETTER
		n     = QRot( lq, n );
		t.xyz = QRot( lq, t.xyz );
		wn    = normalize( uNormalMat * n );
		nObj  = n;
		opos  = ( p - uFitCenter ) * uFitScale;
	}
#endif

	if( uShapeOn == 1 )
	{
		// every corner of the triangle takes the same path, so the triangle stays whole
		vec3 f  = Continuous( opos, n );
		vec3 f0 = Continuous( ( o0 - uFitCenter ) * uFitScale, FetchNrm( i0 ) );
		vec3 f1 = Continuous( ( o1 - uFitCenter ) * uFitScale, FetchNrm( i1 ) );
		vec3 f2 = Continuous( ( o2 - uFitCenter ) * uFitScale, FetchNrm( i2 ) );
#ifdef DEFORM
		{
			// the normal (and tangent) follow the deform: finite differences along the tangent / bitangent (a reflected copy's
			// differences turn inward: flipped back)
			vec3 Tn = t.xyz - n * dot( n, t.xyz );
			Tn      = dot( Tn, Tn ) > 1e-12 ? normalize( Tn ) : normalize( cross( n, abs( n.x ) < 0.9 ? vec3( 1, 0, 0 ) : vec3( 0, 1, 0 ) ) );
			vec3 Bn = cross( n, Tn );
			gNormalPass = true;
			vec3 f0n    = Continuous( opos, n );
			vec3 dT     = Continuous( opos + Tn * 0.01, n ) - f0n, dB = Continuous( opos + Bn * 0.01, n ) - f0n;
			gNormalPass = false;
			vec3 dn = cross( dT, dB );
			if( dot( dn, dn ) > 1e-16 )
				n = normalize( dn ) * ( gFoldFlip ? -1.0 : 1.0 );
			// W7: the tangent = the file's own tangent carried through the deform (the one squared to the normal changed normal-mapped
			// shading at a deform's first step: max 75 / 255)
			gNormalPass = true;
			vec3 dt     = Continuous( opos + t.xyz * 0.01, n ) - f0n;
			gNormalPass = false;
			if( dot( dt, dt ) > 1e-16 )
				t.xyz = normalize( dt );
		}
#endif
		if( uLowPoly > 0.0 )
		{
			// the facet is the grid cell of the triangle's centroid: all three corners agree on it
			Facet fct = MakeFacet( ( f0 + f1 + f2 ) / 3.0 );
			f         = LowPoly( f, fct );
			f0 = LowPoly( f0, fct ), f1 = LowPoly( f1, fct ), f2 = LowPoly( f2, fct );
			n         = QRot( fct.q, n );
			t.xyz     = QRot( fct.q, t.xyz );
		}
		vec3 fc = ( f0 + f1 + f2 ) / 3.0;
		vec3 fn = cross( f1 - f0, f2 - f0 );
#ifdef DEFORM
		float flatK = max( uVoxel, uLowPoly ); // the continuous deforms carry their own normals (above)
#else
		float flatK = max( max( uVoxel, uLowPoly ), ( uMelt > 0.0 || uTwist != 0.0 || uBend != 0.0 || uRipple > 0.0 ) ? 1.0 : 0.0 );
#endif
		vec3 nOut = n;
		if( dot( fn, fn ) > 1e-24 && flatK > 0.0 )
			nOut = normalize( mix( n, normalize( fn ), clamp( flatK, 0.0, 1.0 ) ) );
		if( uLowPoly > 0.5 )
			uv = ( FetchUV( i0 ) + FetchUV( i1 ) + FetchUV( i2 ) ) / 3.0; // flat-coloured facets
		// slices: whole triangles move with the slab their centroid falls in
		if( uSliceGap > 0.0 || uSliceWave > 0.0 || uSlide != 0.0 )
		{
			vec3 qc   = ToAxis( fc, uSliceAxis );
			float idx = clamp( floor( ( qc.z + 1.0 ) * 0.5 * uSliceCount ), 0.0, uSliceCount - 1.0 );
			vec3 off  = vec3( 0.0, 0.0, ( idx - 0.5 * ( uSliceCount - 1.0 ) ) * uSliceGap * 2.0 / uSliceCount );
			f += FromAxis( off, uSliceAxis );
			// wave and slide run across the screen: along X, or along Y for X slices
			vec3 side = uSliceAxis == 0 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
			f += side * ( sin( idx * 0.9 + uSlicePhase ) * uSliceWave * 0.25 + uSlide * 3.2 * ( 0.6 + 0.8 * fract( idx * 0.618 ) ) );
		}
#ifdef DEFORM
		if( uXLook == 3 )
		{
			// EXPLODED VIEW: every group (letter / part / island) slides out from the centre along its own direction, as one piece
			vec4 g  = texelFetch( uXGroup, base / 3 );
			vec3 mv = g.xyz * uXAmt * 1.4;
			f += mv, f0 += mv, f1 += mv, f2 += mv;
		}
#endif
		// shatter: whole shards fly, spin and fall, and retrace the path when they come back
		if( uShatterT > 0.0 && uShardsReady == 1 )
		{
			uint s   = texelFetch( uShardOf, base / 3 ).r;
			vec4 sd  = texelFetch( uShardData, int( s ) );
			vec3 h   = Hash3v( sd.w * 17.31 );
			vec3 dir = normalize( sd.xyz + ( h - 0.5 ) * 0.9 + vec3( 0.0, 0.0, 0.35 ) );
			vec3 vel = dir * uShatterForce * ( 0.5 + h.x );
			vec3 ax  = normalize( Hash3v( sd.w * 5.13 + 2.0 ) - 0.5 + 1e-4 );
#if defined( BOUNCE )
			// W26 Shatter Physics Bounce: the shard's recorded flight (60 Hz, 4 s: offset + rotation), read at the flight time,
			// so Reform (the time running back) retraces it
			float fr = clamp( uShatterT * 60.0, 0.0, 240.0 );
			int i0   = int( floor( fr ) ), i1 = min( i0 + 1, 240 );
			float a  = fr - float( i0 );
			int pb   = int( s ) * 482;
			vec3 off = mix( texelFetch( uShardPath, pb + i0 * 2 ).xyz, texelFetch( uShardPath, pb + i1 * 2 ).xyz, a );
			vec4 q   = QNlerp( texelFetch( uShardPath, pb + i0 * 2 + 1 ), texelFetch( uShardPath, pb + i1 * 2 + 1 ), a );
			mat3 R   = mat3( QRot( q, vec3( 1.0, 0.0, 0.0 ) ), QRot( q, vec3( 0.0, 1.0, 0.0 ) ), QRot( q, vec3( 0.0, 0.0, 1.0 ) ) );
			f        = SHARD_PIVOT( s, sd ) + off + R * ( f - SHARD_PIVOT( s, sd ) );
#elif defined( TRANS )
			// transition: the rim leaves first and the centre last (jittered); each shard flies over its own
			// stretch of the slider, eased at both ends, and shrinks away over the far half of its flight
			float k  = clamp( 0.9 - 0.6 * length( sd.xy ) + 0.5 * ( h.z - 0.5 ), 0.0, 1.0 );
			float u  = clamp( ( uTransE - uTransStagger * k ) / ( 1.0 - uTransStagger ), 0.0, 1.0 );
			float a  = uTransSide == 1 ? u : 1.0 - u;
			float T  = uShatterT * mix( a, a * a * ( 3.0 - 2.0 * a ), uTransEase );
			mat3 R   = RotAxis( ax, uShardSpin * T * ( 0.4 + h.y ) );
			f        = SHARD_PIVOT( s, sd ) + R * ( f - SHARD_PIVOT( s, sd ) ) * ( 1.0 - uTransEase * smoothstep( 0.4, 1.0, a ) ) + vel * T + vec3( 0.0, -0.5 * uShatterGravity * T * T, 0.0 );
#else
			mat3 R   = RotAxis( ax, uShardSpin * uShatterT * ( 0.4 + h.y ) );
			float T  = uShatterT;
			f        = SHARD_PIVOT( s, sd ) + R * ( f - SHARD_PIVOT( s, sd ) ) + vel * T + vec3( 0.0, -0.5 * uShatterGravity * T * T, 0.0 );
#endif
			nOut     = R * nOut;
			t.xyz    = R * t.xyz;
		}
#ifdef CRACKS
		// W26 lava split: every triangle moves with the crack cell of its centroid, out from the mask's centre, once the
		// cracks have reached that cell's site
		{
			vec4 cc  = CrackCell( fc * uCrackScale );
			vec3 sit = cc.yzw / uCrackScale;
			float on = 1.0 - smoothstep( uCrackFront - 0.15, uCrackFront, length( sit - uCrackSeed ) );
			vec3 mv  = ( normalize( sit + vec3( 0.0, 0.0, 0.25 ) ) * 0.12 + ( CrackHash( floor( cc.yzw * 7.0 ) ) - 0.5 ) * 0.05 ) * uSplit * on;
			f += mv, f0 += mv, f1 += mv, f2 += mv;
		}
#endif
#ifdef MORPH
		{
			const float PI_ = 3.14159265;
			vec3 cen = ( f0 + f1 + f2 ) / 3.0;
			vec3 rest = ( ( FetchPos( i0 ) + FetchPos( i1 ) + FetchPos( i2 ) ) / 3.0 - uFitCenter ) * uFitScale; // the triangle at rest
			if( uMorphVS == 2 )
			{
				// PIECES: each shard flies from its home to its partner's home on the other model (partners matched by rank, bottom-up),
				// shrinking away (outgoing) / growing in (incoming) over the far / near half of its flight, turning on the way
				uint sh   = texelFetch( uShardOf, base / 3 ).r;
				vec4 sd   = texelFetch( uShardData, int( sh ) );
				float rk  = texelFetch( uRankSelf, int( sh ) ).r;
				int pr    = int( min( floor( rk * uPartnerN ), uPartnerN - 1.0 ) );
				int ps    = int( texelFetch( uRankOther, pr ).g + 0.5 );
				vec3 tgt  = texelFetch( uPartnerData, ps ).xyz / uOtherFit.w + uOtherFit.xyz; // the partner's centroid, the other model's raw space
				tgt       = ( uMorphFromOther * vec4( tgt, 1.0 ) ).xyz;
				float u   = MorphU( rk );
				float s2  = u * u * ( 3.0 - 2.0 * u );
				vec3 home = SHARD_PIVOT( sh, sd );
				vec3 c    = uMorphSide == 1 ? mix( home, tgt, s2 ) : mix( tgt, home, s2 );
				c += ( vec3( 0.0, 0.5, 0.0 ) + 0.3 * normalize( home + vec3( 1e-4 ) ) ) * uMorphSpread * sin( PI_ * u );
				float sc  = uMorphSide == 1 ? 1.0 - smoothstep( 0.45, 1.0, u ) : smoothstep( 0.0, 0.55, u );
				vec3 hh   = Hash3v( sd.w * 17.31 );
				mat3 R    = RotAxis( normalize( Hash3v( sd.w * 5.13 + 2.0 ) - 0.5 + 1e-4 ), uShardSpin * 0.6 * sin( PI_ * u ) * ( 0.4 + hh.y ) );
				f         = c + R * ( f - home ) * sc;
				nOut      = R * nOut;
				t.xyz     = R * t.xyz;
			}
			else if( uMorphVS == 3 )
			{
				// IMPLODE: triangles fall into the centre, shrinking (the rim first), turning about the up axis; the incoming model bursts out
				float u  = MorphU( 1.0 - clamp( length( rest ) / 1.5, 0.0, 1.0 ) );
				float a  = u * u * ( 3.0 - 2.0 * u );
				a        = uMorphSide == 1 ? a : 1.0 - a;
				mat3 R   = RotAxis( vec3( 0.0, 1.0, 0.0 ), uMorphSpread * PI_ * a );
				f        = R * ( cen * ( 1.0 - a ) + ( f - cen ) * ( 1.0 - a ) );
				nOut     = R * nOut;
				t.xyz    = R * t.xyz;
			}
			else if( uMorphVS == 4 )
			{
				// GLITCH: the bands in flux (near their switch between the models) jump sideways on the stepped clock
				vec3 g     = ToAxis( rest, uDeformAxis );
				float band = floor( clamp( ( g.z + 1.0 ) * 0.5, 0.0, 0.999 ) * uMorphFreq );
				float h    = fract( sin( ( band + 1.37 ) * 91.7 + uMorphStep * 7.13 ) * 43758.5453 ); // (+ 1.37: band 0 at step 0 is not 0)
				float h2   = fract( sin( band * 17.3 + uMorphStep * 3.71 + 5.0 ) * 24634.6345 );
				float flux = 1.0 - smoothstep( 0.0, 0.2, abs( h - uMorphK ) );
				vec3 q     = ToAxis( f, uDeformAxis );
				q.x += ( h2 - 0.5 ) * 0.4 * uMorphSpread * flux * sin( PI_ * uMorphE );
				f = FromAxis( q, uDeformAxis );
			}
			else if( uMorphVS == 5 )
			{
				// RIPPLE: a wave rides the front (the radius where the models swap), out along the normal
				float x = length( opos ) - uMorphK;
				f += nOut * ( 0.1 * uMorphSpread * sin( PI_ * uMorphE ) * exp( -x * x / 0.02 ) * sin( uMorphFreq * x ) );
			}
			else if( uMorphVS == 6 )
			{
				// SPIN SWAP: spin about the up axis while shrinking (outgoing) / growing (incoming)
				mat3 R = RotAxis( vec3( 0.0, 1.0, 0.0 ), uMorphK );
				f      = R * f * uMorphScale;
				nOut   = R * nOut;
				t.xyz  = R * t.xyz;
			}
			else if( uMorphVS >= 7 )
			{
				// W7 TWIST / MELT: a field over fitted space; the normal follows it by finite differences (as DEFORM)
				vec3 Tn = t.xyz - nOut * dot( nOut, t.xyz );
				Tn      = dot( Tn, Tn ) > 1e-12 ? normalize( Tn ) : normalize( cross( nOut, abs( nOut.x ) < 0.9 ? vec3( 1, 0, 0 ) : vec3( 0, 1, 0 ) ) );
				vec3 Bn = cross( nOut, Tn );
				vec3 g  = MorphField( f );
				vec3 dT = MorphField( f + Tn * 0.01 ) - g, dB = MorphField( f + Bn * 0.01 ) - g;
				vec3 dn = cross( dT, dB );
				if( dot( dn, dn ) > 1e-16 )
					nOut = normalize( dn );
				// the tangent: the file's own tangent carried through the field (not the one squared to the normal above: at no
				// deform that one changed normal-mapped shading, max 75 / 255 at a hop's end)
				vec3 dt = MorphField( f + t.xyz * 0.01 ) - g;
				if( dot( dt, dt ) > 1e-16 )
					t.xyz = normalize( dt );
				if( uMorphVS == 8 )
				{
					float u;
					g    = MorphMelt( f, u );
					nOut = normalize( mix( nOut, uMorphPlane.xyz, smoothstep( 0.75, 1.0, u ) ) + vec3( 1e-6 ) ); // the puddle faces up
				}
				f = g;
			}
		}
#endif
		p    = f / uFitScale + uFitCenter;
		wn   = normalize( uNormalMat * nOut );
		nObj = nOut;
		o0 = f0 / uFitScale + uFitCenter, o1 = f1 / uFitScale + uFitCenter, o2 = f2 / uFitScale + uFitCenter;
	}

#ifdef INSTANCED
	// clone / ghost: its own model matrix (rigid + uniform scale, so mat3(M) is a normal matrix up to length)
	mat4 M  = uInst[ uInstBase + INST_ID ];
	wn      = normalize( mat3( M ) * nObj );
	vInstHA = uInstHA[ uInstBase + INST_ID ];
	vInstX  = uInstX[ uInstBase + INST_ID ];
#else
#define M uModel
	vInstHA = vec2( 0.0, 1.0 );
#endif
	vec3 w  = ( M * vec4( p, 1.0 ) ).xyz;
	vec3 w0 = ( M * vec4( o0, 1.0 ) ).xyz;
	vec3 w1 = ( M * vec4( o1, 1.0 ) ).xyz;
	vec3 w2 = ( M * vec4( o2, 1.0 ) ).xyz;
#ifdef DEFORM
	if( uXLook == 1 )
	{
		w  = ( xPast * vec4( w, 1.0 ) ).xyz;
		w0 = ( xPast * vec4( w0, 1.0 ) ).xyz, w1 = ( xPast * vec4( w1, 1.0 ) ).xyz, w2 = ( xPast * vec4( w2, 1.0 ) ).xyz;
		wn = normalize( gXRot * wn );
	}
	vXFall = gXFall;
#endif
	vec3 fn   = cross( w1 - w0, w2 - w0 );
	float len = length( fn );
	fn        = len > 1e-20 ? fn / len : wn;
	vec3 c    = ( w0 + w1 + w2 ) / 3.0;

	if( uExplode > 0.0 )
	{
		float id  = float( base / 3 );
		float r1  = Hash1( id );
		vec3 rdir = normalize( vec3( Hash1( id + 1.7 ), Hash1( id + 3.1 ), Hash1( id + 5.3 ) ) * 2.0 - 1.0 + 1e-4 );
		vec3 dir  = normalize( mix( fn, rdir, 0.6 * uExplodeRandom ) );
		float mag = uExplode * mix( 1.0, r1 * 2.0, uExplodeRandom ) * 0.35;
		float shrink = clamp( 1.0 - 0.15 * uExplode, 0.3, 1.0 );
		w = c + ( w - c ) * shrink + dir * mag * uWorldScale;
	}
#ifdef INSTANCED
	if( uTrailSmear > 0.0 )
		w += TrailSmear( w, p, wn );
	w += TrailDrift( vInstX.z );
#endif
	if( uGlitchAmt > 0.0 )
		w = Glitch( w );
	vo.wpos     = w;
	vo.wnrm     = wn;
#ifdef DEFORM
	vo.wtan     = vec4( normalize( gXRot * mat3( M ) * t.xyz ), t.w < 0.0 ? -1.0 : 1.0 );
#else
	vo.wtan     = vec4( normalize( mat3( M ) * t.xyz ), t.w < 0.0 ? -1.0 : 1.0 );
#endif
	vo.uv       = uv;
#if defined( ANIM )
	vo.opos     = aOpos;
#elif defined( LETTERS )
	vo.opos     = lopos;
#else
	vo.opos     = opos;
#endif
	vo.bary     = corner == 0 ? vec3( 1, 0, 0 ) : ( corner == 1 ? vec3( 0, 1, 0 ) : vec3( 0, 0, 1 ) );
#ifdef CAPTURE
	{
		vec3 cw = w;
		if( uCapSwirl > 0.0 )
		{
			vec3 T0 = vo.wtan.xyz - wn * dot( wn, vo.wtan.xyz );
			T0      = dot( T0, T0 ) > 1e-12 ? normalize( T0 ) : normalize( cross( wn, abs( wn.x ) < 0.9 ? vec3( 1, 0, 0 ) : vec3( 0, 1, 0 ) ) );
			vec3 B0 = cross( wn, T0 );
			float h = fract( sin( float( gl_VertexID ) * 78.233 + 1.7 ) * 43758.5453 );
			float rt = floor( ( 0.5 + h ) * 1000.0 + 0.5 ) / 1000.0 * ( h > 0.5 ? 1.0 : -1.0 ); // whole turns per 1000: seamless at the wrap
			float a  = 6.2831853 * ( fract( uCapPhase * rt ) + h );
			cw += ( T0 * cos( a ) + B0 * sin( a ) ) * uCapSwirl * ( 0.6 + 0.8 * fract( h * 7.31 ) );
		}
		vCapPos = vec4( cw, uCapTime );
		vCapNrm = vec4( wn, ToAxis( opos, uCapAxis ).z );
		vec4 bc = uBaseFactor;
		if( uHas[ 0 ] == 1 )
			bc *= textureLod( uTex0, uv, 2.0 );
		vCapCol = vec4( bc.rgb, uCapHidden == 1 ? 0.0 : 1.0 );
	}
#endif
#ifdef PTFX
	vec4 pth = PtHash( gl_VertexID );
	vec3 ptTrail = vec3( 0.0 );
	if( uPtTrailOn == 1 )
	{
		vec3 bt = vec3( 1.0 - pref.y - pref.z, pref.y, pref.z );
		ptTrail = mat3( M ) * ( PtTrail( i0 ) * bt.x + PtTrail( i1 ) * bt.y + PtTrail( i2 ) * bt.z - ( FetchPos( i0 ) * bt.x + FetchPos( i1 ) * bt.y + FetchPos( i2 ) * bt.z ) );
	}
	float ptAlpha = 1.0;
	w += PtMotion( w, wn, pth, ptTrail, ptAlpha );
#endif
#ifdef POINTS
	w += normalize( uCamPos - w + vec3( 1e-6 ) ) * uPointLift;
	float ph = fract( sin( float( gl_VertexID ) * 12.9898 + 4.1 ) * 43758.5453 );
	float rt = floor( ( 0.6 + 0.9 * fract( ph * 7.31 ) ) * 1000.0 + 0.5 ) / 1000.0; // whole twinkles per 1000: seamless at the wrap
	vTwinkle = 1.0 - uTwinkle * ( 0.5 + 0.5 * cos( 6.2831853 * ( fract( uTwinklePhase * rt ) + ph ) ) );
	gl_PointSize = uPointPx;
#endif
#ifdef PTFX
	{
		// the sprite: the shape's own scale x the size variation (random x nearness here; the surface's luma in the fragment stage)
		float refD = length( uCamPos - uPtCentre ), dist = length( uCamPos - w );
		float near = clamp( refD / max( dist, 1e-6 ), 0.3, 3.0 );
		float px   = uPointPx * mix( 1.0, ( 0.35 + 1.3 * pth.z ) * near * 1.6, uPtSizeVar );
		float bright = 1.0;
		if( uPtShape == 11 )
			px *= 3.0;
		else if( uPtShape == 12 )
			px *= 2.5;
		else if( uPtShape == 17 ) // BOKEH: out of the model's depth = bigger and fainter
		{
			float s = 1.0 + 3.0 * clamp( abs( dist - refD ) / max( uPtRadius, 1e-6 ), 0.0, 1.0 );
			px *= 1.4 * s, bright = 1.0 / s;
		}
		else if( uPtShape == 16 )
			px = max( 1.0, floor( px + 0.5 ) );
		px           = clamp( px, 1.0, 256.0 );
		gl_PointSize = px;
		vPtA         = vec4( pth.x, px, uPtSizeVar, ptAlpha );
		vPtB         = vec4( pth.y, pth.z, pth.w, bright );
	}
#endif
	gl_Position = uViewProj * vec4( w, 1.0 );
#ifdef PTFX
	if( uPtShape == 16 ) // PIXEL: the centre on a pixel centre (odd sizes) / corner (even), hard edges
	{
		vec2 sp  = ( gl_Position.xy / gl_Position.w * 0.5 + 0.5 ) * uPtViewport;
		float odd = mod( vPtA.y, 2.0 );
		sp       = odd > 0.5 ? floor( sp ) + 0.5 : floor( sp + 0.5 );
		gl_Position.xy = ( sp / uPtViewport * 2.0 - 1.0 ) * gl_Position.w;
	}
#endif
#ifdef PTSHADE
	{
		int ai   = gl_InstanceID * uPtShade.y + gl_VertexID - uPtShade.x;
		ivec2 at = ivec2( ai % uPtShade.z, ai / uPtShade.z );
		vPsC     = vec4( 0.0 );
		vPsFrag  = vec4( 0.0 );
		if( uPtPass == 1 )
		{
			vec3 nd      = gl_Position.xyz / gl_Position.w;
			vPsFrag      = vec4( ( nd.xy * 0.5 + 0.5 ) * uPtShadeVP, nd.z * 0.5 + 0.5, 1.0 / gl_Position.w );
			gl_Position  = vec4( ( vec2( at ) + 0.5 ) / vec2( uPtShade.zw ) * 2.0 - 1.0, 0.0, 1.0 );
			gl_PointSize = 1.0;
		}
		else
			vPsC = texelFetch( uPtShadeTex, at, 0 );
	}
#endif
}
)GLSL";


// Extra lights shared by the surface shader, the fog march and the motes: up to 6 spot / point lights
// (0-1 = the volume lights A/B, which may carry a shadow map; 2-5 = the sweeping spots).
static const char* kLightLib = R"GLSL(
const int MAX_XL = 8;
uniform int uXLCount;
uniform vec3 uXLPos[ MAX_XL ];
uniform vec3 uXLDir[ MAX_XL ];
uniform vec3 uXLColor[ MAX_XL ];  // colour x intensity
uniform vec2 uXLCone[ MAX_XL ];   // cos(outer half angle), cos(inner half angle); x <= -1.0 = point light
uniform float uXLRange;           // shaft length: distance window
uniform mat4 uXLightVP[ 4 ];      // volume lights 0..3: A, B, C left, C right (as enabled)
uniform int uXShadowMode[ 4 ];    // 0 none, 1 spot (2D), 2 point (cube: lights 0 / 1 only)
uniform sampler2D uXShadow2D0;
uniform sampler2D uXShadow2D1;
uniform sampler2D uXShadow2D2;
uniform sampler2D uXShadow2D3;
uniform samplerCube uXShadowCube0;
uniform samplerCube uXShadowCube1;

float XLWindow( float d )
{
	float x = d / max( uXLRange, 1e-3 );
	float w = clamp( 1.0 - x * x * x * x, 0.0, 1.0 );
	return w * w / ( 1.0 + d * d );
}
float XLCone( int i, vec3 l ) // l: unit vector light -> point
{
	vec2 c = uXLCone[ i ];
	return c.x <= -1.0 ? 1.0 : smoothstep( c.x, c.y, dot( l, uXLDir[ i ] ) );
}
// 1 = lit. The shadow maps store the distance to the light.
float XShadow( int i, vec3 p, float d )
{
	if( i > 3 )
		return 1.0;
	int m = uXShadowMode[ i ];
	if( m == 0 )
		return 1.0;
	float bias = 0.02 + 0.01 * d;
	if( m == 2 )
	{
		vec3 v  = p - uXLPos[ i ];
		float s = i == 0 ? textureLod( uXShadowCube0, v, 0.0 ).r : textureLod( uXShadowCube1, v, 0.0 ).r;
		return step( d - bias, s );
	}
	vec4 c = uXLightVP[ i ] * vec4( p, 1.0 );
	if( c.w <= 1e-5 )
		return 1.0;
	vec2 uv = c.xy / c.w * 0.5 + 0.5;
	if( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThan( uv, vec2( 1.0 ) ) ) )
		return 1.0;
	float s = i == 0 ? textureLod( uXShadow2D0, uv, 0.0 ).r
	        : i == 1 ? textureLod( uXShadow2D1, uv, 0.0 ).r
	        : i == 2 ? textureLod( uXShadow2D2, uv, 0.0 ).r
	                 : textureLod( uXShadow2D3, uv, 0.0 ).r;
	return step( d - bias, s );
}
)GLSL";

static const char* kMainFS = R"GLSL(
in VData
{
	vec3 wpos;
	vec3 wnrm;
	vec4 wtan;
	vec2 uv;
	vec3 opos;
	vec3 bary;
} vi;

out vec4 fragColor;
flat in vec2 vInstHA; // clone hue offset, ghost alpha
#ifdef WIRE
// W5: the WIRE twins. uWirePass 0 depth only, 1 dark fill (+ lines), 2 lines only, 3 over the surface (the W1 formula), 4 the lines'
// glow into the HDR wire buffer (light at alpha 0). W6: the POINTS twins draw passes 2 and 4 as round sprites
flat in vec3 vEdge;
#ifdef POINTS
uniform float uPointPx;
flat in float vTwinkle;
#ifdef PTSHADE
uniform int uPtPass;   // MoveEhr W7: 1 = shade the point into its texel, 2 = the sprite with that colour
flat in vec4 vPsC;
flat in vec4 vPsFrag;
#endif
#ifdef PTFX
// MoveEhr W4: the point shapes (2D distance fields on the sprite, anti-aliased over one pixel), lit impostors, glyphs
uniform int uPtShape;
uniform vec3 uPtRight, uPtUp;   // the camera's right / up (world): the sprite's frame
uniform sampler2D uPtGlyph;     // unit 63: 8 x 8 glyphs (R8, row 0 = the top of glyph 0)
uniform int uPtGlyphN;          // glyphs in the atlas
flat in vec4 vPtA;              // hash, the sprite px, the luma's share of the size variation, alpha
flat in vec4 vPtB;              // hashes, the bokeh brightness
#endif
#endif
uniform int uWirePass;
uniform vec3 uWireColor;
uniform float uWireColorMix;
uniform float uWireFade, uWireFadeNear, uWireFadeFar;
#endif
#ifdef INSTANCED
flat in vec4 vInstX;   // keep, fog keep, trail age
uniform int uTrailPass; // 1 = the ghosts (trail copies)
uniform float uTrailDust;
uniform float uTrailGrain; // grain cell, fitted object units
uniform vec3 uTrailColor;
uniform float uTrailMix;
uniform float uTrailGlow;
#endif

// per part
uniform sampler2D uTex0; // base color (sRGB)
uniform sampler2D uTex1; // normal
uniform sampler2D uTex2; // roughness
uniform sampler2D uTex3; // metallic
uniform sampler2D uTex4; // height (VS)
uniform sampler2D uTex5; // emissive (sRGB)
uniform sampler2D uTex6; // AO
uniform int uHas[ 7 ];
uniform vec4 uSel[ 7 ];
uniform vec4 uBaseFactor;
uniform vec3 uEmisFactor;
uniform float uRoughFactor;
uniform float uMetalFactor;
uniform float uNormalScale;
uniform float uFlipGreen; // -1 or 1
uniform int uCutout;
uniform float uPartHue;
uniform float uPartGlow;
uniform int uPartSkin;
uniform int uPartId;

// global material
uniform vec3 uTint;
uniform float uTintMix;
uniform float uHue;
uniform float uSaturation;
uniform float uBrightness;
uniform int uMetalOverride;
uniform float uMetalValue;
uniform float uRoughScale;
uniform float uNormalStrength;
uniform float uEmissiveGlow;
uniform float uRimGlow;
uniform vec3 uRimColor;

// lighting
uniform vec3 uCamPos;
uniform vec3 uKeyDir;
uniform vec3 uKeyColor;
uniform vec3 uFillDir;
uniform vec3 uFillColor;
uniform float uAmbient;
uniform float uEnvIntensity;
uniform int uEnvSource; // 0 studio, 1 input, 2 off (W5: 3 HDRI file, ENVFILE twins only)
uniform float uEnvRotate;
uniform float uExposure;
uniform int uTonemap; // 0 ACES, 1 filmic, 2 none
uniform mat4 uView;

// input (FX)
uniform sampler2D uInputTex; // mipmapped copy, sRGB-encoded values
uniform float uInputMaxLod;
uniform int uInputUse;        // 0 off, 1 background, 2 skin flagged, 3 skin all
uniform int uInputProjection; // 0 front planar, 1 model uv, 2 spherical
uniform float uInputMix;

// fx
uniform float uWireMix;
uniform float uWireThickness;
uniform float uDissolve;
uniform float uDissolveEdge;
uniform float uToonSteps;
uniform float uFogDensity;
uniform float uFogStart;
uniform vec4 uBackground; // premultiplied, sRGB-encoded
uniform int uDebugView;
uniform int uOutlinePass;

// ---- Stage 3 surface effects (all scaled by the part's Surface amount)
uniform int uSurfaceOn;
uniform float uPartSurface;
uniform float uTime;
uniform float uBeat;
uniform float uAudioMid;
uniform float uFilm, uFilmThick, uFilmShift, uFilmSwirl;
uniform float uGlitter, uGlitterDensity, uGlitterSize, uGlitterMetal;
uniform float uGlitterPhase; // twinkles (integrated Glitter Twinkle), wrapped at 1000
uniform float uChrome, uChromeRipple;
uniform vec2 uChromeOff;     // ripple drift (y, z), wrapped at 64: Noise3P tiles every 64
uniform vec3 uFilmT;         // swirl drift, the same drift one loop back, cross-fade weight toward it
uniform float uGlass, uGlassIOR, uGlassDisp;
uniform vec3 uGlassTint;
uniform int uGlassInput; // FX build with an input: refract the incoming layer
uniform vec2 uViewport;
uniform float uHolo, uHoloScan, uHoloFlicker, uHoloSplit;
uniform float uHoloPhase;    // scanline crawl, radians (wrapped)
uniform vec3 uHoloColor;
uniform float uHeat, uHeatTemp, uHeatSpread;
// ---- Stage 4: contour lines + blow to dust (the mesh dissolves where the dust has left)
uniform float uContour;
uniform int uContourMode; // 0 X, 1 Y, 2 Z, 3 height map, 4 radial
uniform float uContourSpacing;
uniform float uContourWidth;
uniform float uContourScroll;
uniform vec3 uContourColor;
uniform float uDustCut; // > 0: fragments whose dust noise is below this have blown away
uniform int uDustInvert; // transition: the incoming model keeps exactly those fragments
uniform int uDissolveInvert;
uniform int uPattern; // 0 off, 1 stripes, 2 cells, 3 noise
uniform float uPatAmt, uPatScale, uPatBeat, uPatAudio;
uniform vec3 uPatT;          // crawl (integrated Pattern Speed), the same one loop back, cross-fade weight toward it
uniform vec3 uPatColor;
#ifdef MORPH
// W6 Stage 2: the transition pack's fragment side. uMorphMask 1 scan (a plane sweeps along the deform axis; a wire band where it is),
// 2 glitch (bands across the axis take one model or the other on the stepped clock), 3 ripple (a front out from the centre), 4 burn (a
// fire front from a point); side 1 = the outgoing model (shows ahead of the front), 2 = the incoming one (behind it)
uniform int uMorphMask;
uniform int uMorphSide;
uniform float uMorphK, uMorphE, uMorphStep, uMorphFreq, uMorphBand, uMorphGlow, uMorphTemp, uMorphFlame;
uniform vec3 uMorphColor, uMorphSeed;
uniform int uDeformAxis;
#endif
#ifdef DEFORM
// W9 (the DEFORM twins' fragment side): the Build / Cut looks' region, band and shading
uniform int uXLook;      // 1 slit-scan, 2 print on, 3 exploded, 4 black hole
uniform int uXCut;       // 0 no cut; 1 keep the kept side (the solid), 2 keep the removed side (drawn in the Wire Mode)
uniform vec4 uXPlane;    // world plane (n, d): kept where dot( n, p ) + d <= 0
uniform vec4 uXPlaneO;   // the same plane in fitted rest space (clones: their own pose)
uniform vec3 uXColor;    // Wire Color: the print head / the cut's band
uniform float uXGlow;    // the band's light
uniform float uXBand;    // the band's half width, world units
uniform float uXLayer;   // print on: the layer lines' spacing (fitted), 0 = none
uniform int uXRegion;    // the cut's region: 0 the plane (+ a slab under it: Cross-Section), 1 a rounded box (Reveal Box)
uniform vec2 uXSlab;     // cross-section slab thickness: world, fitted (0 = no lower plane)
uniform vec4 uXBox;      // reveal box (fitted rest space): centre, half size
uniform float uXRound;   // its corner radius share (0 box .. 1 sphere)
// W9 Stage D: the camera looks (shading modes of the object, display space): 3 X-Ray, 4 Thermal, 5 Night Vision, 6 LED Matrix
uniform int uXSurf;
uniform float uXSurfAmt, uXSurfScale, uXSurfPhase, uXColorMix;
uniform vec3 uXScreen;   // the model's centre on screen (px) + its radius (px): the night vision's vignette
in float vXFall;
#endif
#ifdef SHADOW_PASS
uniform vec3 uShadowLightPos;
uniform vec3 uShadowDir;   // ortho (directional) shadow: depth along the light's travel
uniform int uShadowOrtho;
#endif
#ifdef PHOTON_PASS
uniform int uPhotonOut;
#endif
#ifdef CRACK_PASS
uniform float uLavaGlow, uLavaTemp, uCrackWidth, uLavaTintMix, uCrackFlow;
uniform vec3 uLavaTint;
#endif
#ifdef PROJ_PASS
// W26: the cookie-light pass (its own program variants, drawn additively over the mask): up to 2 lights whose texture holds
// the projected picture (rgb) and the shadow distance (a) in the light's own projection (Projector; Stage 8 caustics)
uniform int uPLCount;
uniform mat4 uPLVP[ 2 ];
uniform vec3 uPLPos[ 2 ];
uniform vec3 uPLColor[ 2 ];
uniform sampler2D uPLTex0;
uniform sampler2D uPLTex1;
uniform int uPLExact[ 2 ]; // 1 = light only AT the stored distance (caustics land on their receiver), 0 = a shadow test
#endif

const float PI = 3.14159265;

float Sel( int i, vec4 s ) { return dot( s, uSel[ i ] ); }

vec3 HueRotate( vec3 c, float turns )
{
	float a       = turns * 6.2831853;
	const vec3 k  = vec3( 0.57735 );
	float ca      = cos( a );
	return c * ca + cross( k, c ) * sin( a ) + k * dot( k, c ) * ( 1.0 - ca );
}
float Luma( vec3 c ) { return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) ); }

// ---- noise for dissolve
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
// Noise3 on a lattice that repeats every 64 cells on each axis: a drift wrapped at 64 is seamless
float Noise3P( vec3 x )
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
// Cross-fade of two independent fields (mean 0.5) that keeps their contrast: w 0 = a, 1 = b
float LoopBlend( float a, float b, float w )
{
	return 0.5 + ( ( 1.0 - w ) * ( a - 0.5 ) + w * ( b - 0.5 ) ) / sqrt( ( 1.0 - w ) * ( 1.0 - w ) + w * w );
}

// ---- procedural studio environment
float Softbox( vec3 d, vec3 c, vec2 hs, float r )
{
	float facing = dot( d, c );
	if( facing <= 0.0 )
		return 0.0;
	vec3 t     = normalize( abs( c.y ) > 0.99 ? cross( c, vec3( 1, 0, 0 ) ) : cross( c, vec3( 0, 1, 0 ) ) );
	vec3 b     = cross( t, c );
	vec2 p     = vec2( dot( d, t ), dot( d, b ) ) / facing;
	float blur = 0.015 + r * r * 1.6;
	float s    = ( 1.0 - smoothstep( hs.x - blur, hs.x + blur, abs( p.x ) ) ) * ( 1.0 - smoothstep( hs.y - blur, hs.y + blur, abs( p.y ) ) );
	return s * ( hs.x * hs.y ) / ( ( hs.x + blur ) * ( hs.y + blur ) );
}
vec3 Studio( vec3 d, float r )
{
	float ca = cos( uEnvRotate ), sa = sin( uEnvRotate );
	d        = vec3( ca * d.x + sa * d.z, d.y, -sa * d.x + ca * d.z );
	vec3 col = mix( vec3( 0.015, 0.015, 0.018 ), vec3( 0.16, 0.17, 0.19 ), smoothstep( -0.25, 0.7, d.y ) );
	col += vec3( 0.06 ) * ( 1.0 - smoothstep( 0.0, 0.12 + r * 0.3, abs( d.y + 0.02 ) ) );
	col += Softbox( d, normalize( vec3( -0.65, 0.55, 0.55 ) ), vec2( 0.45, 0.3 ), r ) * vec3( 7.0, 6.8, 6.4 );
	col += Softbox( d, normalize( vec3( 0.85, 0.15, 0.25 ) ), vec2( 0.07, 0.5 ), r ) * vec3( 4.5, 4.7, 5.2 );
	col += Softbox( d, normalize( vec3( 0.1, 1.0, -0.1 ) ), vec2( 0.35, 0.35 ), r ) * vec3( 2.2 );
	col += Softbox( d, normalize( vec3( 0.2, 0.1, -1.0 ) ), vec2( 0.6, 0.12 ), r ) * vec3( 1.6, 1.4, 1.2 );
	return col;
}
vec3 SrgbToLinear( vec3 c ) { return pow( max( c, 0.0 ), vec3( 2.2 ) ); }
vec2 SphereMapUV( vec3 wdir )
{
	vec3 v  = mat3( uView ) * wdir;
	float m = 2.0 * sqrt( v.x * v.x + v.y * v.y + ( v.z + 1.0 ) * ( v.z + 1.0 ) );
	return v.xy / max( m, 1e-4 ) + 0.5; // host textures are bottom-up: +y view = +v
}
#ifdef ENVFILE
// MoveEhr W5 (S5): Env Source "HDRI File": an equirect .hdr (linear radiance, RGB16F, full mip chain, unit 64; row 0 = the top of
// the image), Env Rotate as Studio's; roughness picks the level as the Input env does (r 1 = 4 x 2 texels: the diffuse keeps a side)
uniform sampler2D uEnvFile;
uniform float uEnvFileLod; // the last level
vec3 EnvFile( vec3 d, float r )
{
	float ca = cos( uEnvRotate ), sa = sin( uEnvRotate );
	d        = normalize( vec3( ca * d.x + sa * d.z, d.y, -sa * d.x + ca * d.z ) );
	vec2 uv  = vec2( atan( d.x, -d.z ) * 0.15915494 + 0.5, 0.5 - asin( clamp( d.y, -1.0, 1.0 ) ) * 0.31830989 );
	return textureLod( uEnvFile, uv, r * max( 0.0, uEnvFileLod - 2.0 ) ).rgb;
}
#endif
vec3 Env( vec3 d, float r )
{
	if( uEnvSource == 2 )
		return vec3( 0.0 );
#ifdef ENVFILE
	if( uEnvSource == 3 )
		return EnvFile( d, r );
#endif
	if( uEnvSource == 1 )
		return SrgbToLinear( textureLod( uInputTex, SphereMapUV( d ), r * uInputMaxLod ).rgb ) * 2.0;
	return Studio( d, r );
}
vec2 EnvBRDF( float r, float nov )
{
	const vec4 c0 = vec4( -1.0, -0.0275, -0.572, 0.022 );
	const vec4 c1 = vec4( 1.0, 0.0425, 1.04, -0.04 );
	vec4 rr       = r * c0 + c1;
	float a004    = min( rr.x * rr.x, exp2( -9.28 * nov ) ) * rr.x + rr.y;
	return vec2( -1.04, 1.04 ) * a004 + rr.zw;
}
float DGGX( float noh, float a )
{
	float a2 = a * a;
	float d  = noh * noh * ( a2 - 1.0 ) + 1.0;
	return a2 / ( PI * d * d );
}
float VisSmith( float nov, float nol, float a )
{
	float k = a * 0.5;
	return 0.25 / ( ( nov * ( 1.0 - k ) + k ) * ( nol * ( 1.0 - k ) + k ) );
}
vec3 Tonemap( vec3 c )
{
	if( uTonemap == 0 )
	{
		c *= 0.6;
		return clamp( ( c * ( 2.51 * c + 0.03 ) ) / ( c * ( 2.43 * c + 0.59 ) + 0.14 ), 0.0, 1.0 );
	}
	if( uTonemap == 1 )
	{
		const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30, W = 11.2;
		vec3 x        = c * 2.0;
		vec3 m        = ( ( x * ( A * x + C * B ) + D * E ) / ( x * ( A * x + B ) + D * F ) ) - E / F;
		float w       = ( ( W * ( A * W + C * B ) + D * E ) / ( W * ( A * W + B ) + D * F ) ) - E / F;
		return clamp( m / w, 0.0, 1.0 );
	}
	return clamp( c, 0.0, 1.0 );
}
vec3 Encode( vec3 c ) { return pow( max( c, 0.0 ), vec3( 1.0 / 2.2 ) ); }
#ifdef DEFORM
// W9 Thermal: black - indigo - purple - red - orange - yellow - white (an iron-bow style ramp)
vec3 Thermal( float h )
{
	const vec3 c0 = vec3( 0.0, 0.0, 0.02 ), c1 = vec3( 0.12, 0.0, 0.45 ), c2 = vec3( 0.55, 0.0, 0.6 ), c3 = vec3( 0.95, 0.12, 0.1 );
	const vec3 c4 = vec3( 1.0, 0.55, 0.0 ), c5 = vec3( 1.0, 0.92, 0.2 ), c6 = vec3( 1.0, 1.0, 1.0 );
	float x = clamp( h, 0.0, 1.0 ) * 6.0;
	return x < 1.0 ? mix( c0, c1, x ) : x < 2.0 ? mix( c1, c2, x - 1.0 ) : x < 3.0 ? mix( c2, c3, x - 2.0 ) : x < 4.0 ? mix( c3, c4, x - 3.0 )
	     : x < 5.0 ? mix( c4, c5, x - 4.0 ) : mix( c5, c6, x - 5.0 );
}
#endif

// Kelvin -> linear rgb, max component 1 (Helland's fit)
vec3 Blackbody( float k )
{
	float t = clamp( k, 800.0, 15000.0 ) / 100.0;
	vec3 c;
	c.r = t <= 66.0 ? 1.0 : clamp( 1.292936 * pow( t - 60.0, -0.1332047592 ), 0.0, 1.0 );
	c.g = t <= 66.0 ? clamp( 0.3900815788 * log( t ) - 0.6318414438, 0.0, 1.0 ) : clamp( 1.129890861 * pow( t - 60.0, -0.0755148492 ), 0.0, 1.0 );
	c.b = t >= 66.0 ? 1.0 : ( t <= 19.0 ? 0.0 : clamp( 0.5432067891 * log( t - 10.0 ) - 1.19625408914, 0.0, 1.0 ) );
	return pow( c, vec3( 2.2 ) );
}
// thin-film interference of a soap / oil layer (n = 1.33), thickness in nm
vec3 ThinFilm( float cosV, float nm )
{
	float n    = 1.33;
	float cosT = sqrt( max( 1.0 - ( 1.0 - cosV * cosV ) / ( n * n ), 0.0 ) );
	float opd  = 2.0 * n * nm * cosT;
	return 0.5 + 0.5 * cos( 6.2831853 * opd / vec3( 650.0, 532.0, 450.0 ) );
}
vec2 Hash22( vec2 p )
{
	p = vec2( dot( p, vec2( 127.1, 311.7 ) ), dot( p, vec2( 269.5, 183.3 ) ) );
	return fract( sin( p ) * 43758.5453 );
}
// crawling patterns on one planar projection
float PatternAt( vec2 p, float t )
{
	if( uPattern == 1 )
	{
		float s = sin( ( p.x + p.y * 0.35 - t ) * 6.2831853 );
		return smoothstep( 0.2, 0.9, s );
	}
	if( uPattern == 2 )
	{
		p += vec2( t * 0.6, t * 0.25 );
		vec2 ip = floor( p ), fp = fract( p );
		float d1 = 8.0, d2 = 8.0;
		for( int y = -1; y <= 1; ++y )
			for( int x = -1; x <= 1; ++x )
			{
				vec2 o  = vec2( x, y );
				vec2 c  = o + 0.5 + 0.4 * sin( t + 6.2831853 * Hash22( ip + o ) ) - fp;
				float d = dot( c, c );
				if( d < d1 )
					d2 = d1, d1 = d;
				else if( d < d2 )
					d2 = d;
			}
		return 1.0 - smoothstep( 0.0, 0.12, sqrt( d2 ) - sqrt( d1 ) );
	}
	return smoothstep( 0.45, 0.75, Fbm( vec3( p, t * 0.5 ) ) );
}
// the pattern at the crawl phase; near the loop's end, cross-faded into the pattern one loop back (where it continues)
float PatternX( vec2 p )
{
	if( uPatT.z <= 0.0 )
		return PatternAt( p, uPatT.x );
	if( uPattern == 3 ) // noise: blend the fields before the threshold, so the blobs keep their contrast
		return smoothstep( 0.45, 0.75, LoopBlend( Fbm( vec3( p, uPatT.x * 0.5 ) ), Fbm( vec3( p, uPatT.y * 0.5 ) ), uPatT.z ) );
	return mix( PatternAt( p, uPatT.x ), PatternAt( p, uPatT.y ), uPatT.z );
}

// Host input textures are bottom-up (GL), unlike model textures (top row first), so every
// projection here returns v with +v = up.
vec2 InputUV()
{
	if( uInputProjection == 1 )
		return vec2( fract( vi.uv.x ), 1.0 - fract( vi.uv.y ) );
	if( uInputProjection == 2 )
	{
		vec3 d = normalize( vi.opos + 1e-6 );
		return vec2( atan( d.x, d.z ) / 6.2831853 + 0.5, 0.5 + asin( clamp( d.y, -1.0, 1.0 ) ) / PI );
	}
	return vec2( vi.opos.x * 0.5 + 0.5, vi.opos.y * 0.5 + 0.5 );
}

#if defined( POINTS ) && defined( PTFX )
float PtBox( vec2 p, vec2 b ) { vec2 d = abs( p ) - b; return length( max( d, 0.0 ) ) + min( max( d.x, d.y ), 0.0 ); }
float PtStar( vec2 p, float r, float n, float m ) // iq: a regular star, n points, inner ratio m
{
	float an = 3.141593 / n, en = 3.141593 / m;
	vec2 acs = vec2( cos( an ), sin( an ) ), ecs = vec2( cos( en ), sin( en ) );
	float bn = mod( atan( p.x, p.y ), 2.0 * an ) - an;
	p        = length( p ) * vec2( cos( bn ), abs( sin( bn ) ) );
	p -= r * acs;
	p += ecs * clamp( -dot( p, ecs ), 0.0, r * acs.y / ecs.y );
	return length( p ) * sign( p.x );
}
float PtHeart( vec2 p ) // iq
{
	p.x = abs( p.x );
	if( p.y + p.x > 1.0 )
		return sqrt( dot( p - vec2( 0.25, 0.75 ), p - vec2( 0.25, 0.75 ) ) ) - sqrt( 2.0 ) / 4.0;
	return sqrt( min( dot( p - vec2( 0.0, 1.0 ), p - vec2( 0.0, 1.0 ) ), dot( p - 0.5 * max( p.x + p.y, 0.0 ), p - 0.5 * max( p.x + p.y, 0.0 ) ) ) ) * sign( p.x - p.y );
}
// coverage of the shape at q (-1..1, y up); aa = one pixel in q units; lit = the impostor's light (1 = flat)
float PtCover( vec2 q, float aa, out vec3 lit )
{
	lit       = vec3( 1.0 );
	float r   = length( q );
	float sd  = r - 1.0;
	int sh    = uPtShape;
	if( sh == 1 )
		sd = PtBox( q, vec2( 0.8 ) );
	else if( sh == 2 )
		sd = ( abs( q.x ) + abs( q.y ) - 1.0 ) * 0.7071;
	else if( sh == 3 || sh == 4 )
	{
		vec2 p = sh == 3 ? vec2( q.x + q.y, q.x - q.y ) * 0.7071 : q;
		sd     = min( PtBox( p, vec2( 0.95, 0.2 ) ), PtBox( p, vec2( 0.2, 0.95 ) ) );
	}
	else if( sh == 5 )
		sd = abs( r - 0.75 ) - 0.2;
	else if( sh == 6 )
		sd = PtStar( q, 1.0, 5.0, 2.6 );
	else if( sh == 7 )
		sd = PtStar( q, 1.0, 6.0, 3.0 );
	else if( sh == 8 )
	{
		vec2 p = vec2( abs( q.x ), q.y + 0.25 );
		const float k = 1.7320508;
		sd = max( p.x * k * 0.5 + p.y * 0.5, -p.y ) - 0.62;
	}
	else if( sh == 9 )
	{
		vec2 p = abs( q );
		sd     = max( p.x * 0.8660254 + p.y * 0.5, p.y ) - 0.88;
	}
	else if( sh == 10 )
		sd = PtHeart( q * 1.15 + vec2( 0.0, 0.62 ) ) / 1.15;
	else if( sh == 11 ) // SPARK: a small hot core + four thin rays
		return clamp( exp( -r * r * 40.0 ) + exp( -abs( q.x ) * 28.0 ) * ( 1.0 - abs( q.y ) ) + exp( -abs( q.y ) * 28.0 ) * ( 1.0 - abs( q.x ) ), 0.0, 1.0 );
	else if( sh == 12 ) // SOFT GLOW
		return exp( -r * r * 4.0 ) * ( 1.0 - smoothstep( 0.8, 1.0, r ) );
	else if( sh == 13 || sh == 14 )
	{
		// lit impostors in the sprite's camera frame (x right, y up, z toward the camera)
		vec3 F = cross( uPtRight, uPtUp );
		vec3 nl;
		if( sh == 13 )
		{
			if( r > 1.0 )
				return 0.0;
			nl = vec3( q, sqrt( max( 0.0, 1.0 - r * r ) ) );
		}
		else
		{
			// a cube turned by the point's hash: the ray (0, 0, -1) through (q, 2) against the box
			float a = 6.2831853 * vPtB.x, b = 0.6 + 0.5 * vPtB.y;
			mat3 Ry = mat3( cos( a ), 0.0, -sin( a ), 0.0, 1.0, 0.0, sin( a ), 0.0, cos( a ) );
			mat3 Rx = mat3( 1.0, 0.0, 0.0, 0.0, cos( b ), sin( b ), 0.0, -sin( b ), cos( b ) );
			mat3 Rr = Rx * Ry, Ri = transpose( Rr );
			vec3 o  = Ri * vec3( q, 2.0 ), d = Ri * vec3( 0.0, 0.0, -1.0 );
			vec3 inv = 1.0 / ( d + vec3( 1e-9 ) );
			vec3 t0 = ( -0.56 - o ) * inv, t1 = ( 0.56 - o ) * inv;
			vec3 tn = min( t0, t1 ), tx = max( t0, t1 );
			float tN = max( max( tn.x, tn.y ), tn.z ), tF = min( min( tx.x, tx.y ), tx.z );
			if( tN > tF )
				return 0.0;
			vec3 nb = step( tn.yzx, tn.xyz ) * step( tn.zxy, tn.xyz ) * -sign( d );
			nl      = Rr * nb;
		}
		vec3 N = normalize( nl.x * uPtRight + nl.y * uPtUp + nl.z * F );
		vec3 V = F;
		vec3 c = vec3( uAmbient + 0.15 );
		c += uKeyColor * ( max( dot( N, uKeyDir ), 0.0 ) + 0.5 * pow( max( dot( N, normalize( uKeyDir + V ) ), 0.0 ), 40.0 ) );
		c += uFillColor * max( dot( N, uFillDir ), 0.0 );
		lit = c;
		return 1.0;
	}
	else if( sh == 15 ) // GLYPH: one of the atlas's characters per point
	{
		int g    = int( vPtB.y * float( max( uPtGlyphN, 1 ) ) ) % max( uPtGlyphN, 1 );
		vec2 uvg = ( vec2( float( g % 8 ), float( g / 8 ) ) + clamp( vec2( q.x, -q.y ) * 0.5 + 0.5, 0.0, 1.0 ) ) / 8.0;
		return texture( uPtGlyph, uvg ).r;
	}
	else if( sh == 16 ) // PIXEL: the whole (snapped) sprite, no antialiasing
		return 1.0;
	else if( sh == 17 ) // BOKEH: a disc with a brighter rim
	{
		lit = vec3( ( 0.55 + 0.45 * smoothstep( 0.6, 0.95, r ) ) * vPtB.w );
		sd  = r - 1.0;
	}
	return 1.0 - smoothstep( -aa, 0.0, sd );
}
#endif
#ifdef PTSHADE
// MoveEhr W7: pass 1 runs as a 1-px point among other points' texels, so what a sprite reads off its constant varyings is taken as such:
// no derivatives (0, as on a sprite), level 0 of every texture (a sprite's constant uv picks it), the sprite centre for gl_FragCoord
#define texture( s, c ) textureLod( s, c, 0.0 )
#define fwidth( x ) ( ( x ) * 0.0 )
#define PT_FRAGCOORD vPsFrag
#ifdef DEFORM
#define PT_KEEP ( wireCut * xKeep )
#else
#define PT_KEEP wireCut
#endif
#else
#define PT_FRAGCOORD gl_FragCoord
#endif
void main()
{
#ifdef PTSHADE
	vec3 outc;
	float surfAlpha = 1.0, wireCut = 1.0;
#ifdef DEFORM
	float xKeep = 1.0;
#endif
	if( uPtPass == 2 )
	{
		// the sprite: its point's colour from pass 1 (keep < 0: a debug view, the whole sprite square as the full shading draws it)
		if( vPsC.a < 0.0 )
		{
			fragColor = vec4( vPsC.rgb, 1.0 );
			return;
		}
		outc = vPsC.rgb, wireCut = vPsC.a;
	}
	else
	{
#endif
	// derivatives first: after a discard they are undefined on Apple's GL-over-Metal
	vec3 bw = max( fwidth( vi.bary ), vec3( 1e-6 ) );
	float cc = 0.0, cfw = 1.0;
	if( uContour > 0.0 )
	{
		cc  = uContourMode == 0 ? vi.opos.x : uContourMode == 1 ? vi.opos.y : uContourMode == 2 ? vi.opos.z
		    : uContourMode == 3 ? ( uHas[ 4 ] == 1 ? dot( texture( uTex4, vi.uv ), uSel[ 4 ] ) : Luma( texture( uTex0, vi.uv ).rgb ) ) * 2.0
		    : length( vi.opos );
		cfw = max( fwidth( cc ), 1e-6 );
	}
	if( uOutlinePass == 1 )
	{
		fragColor = vec4( 0.0, 0.0, 0.0, 1.0 );
		return;
	}

	if( uDustCut > 0.0 && ( ( Fbm( vi.opos * 3.0 + 17.0 ) < uDustCut * 1.08 - 0.04 ) != ( uDustInvert == 1 ) ) )
		discard;
#ifdef WIRE
	// W6: a discard here (the dissolve, the pack's fronts) corrupts later values in the WIRE twins on Apple's GL (edgeGlow read 1: a
	// white model mid-hop): they cut through the alpha at the very end instead
#ifndef PTSHADE
	float wireCut = 1.0;
#endif
#endif
#ifdef MORPH
	float morphEdge = 0.0, morphChar = 0.0, morphHeat = 0.0;
	if( uMorphMask > 0 )
	{
		vec3 ax = uDeformAxis == 0 ? vi.opos.yzx : ( uDeformAxis == 1 ? vi.opos.zxy : vi.opos ); // .z runs along the axis
		bool ahead; // the outgoing model's side of the front
		if( uMorphMask == 1 )
		{
			float k  = clamp( ( ax.z + 1.15 ) / 2.3, 0.0, 1.0 );
			ahead    = k >= uMorphK;
			float dz = uMorphSide == 1 ? k - uMorphK : uMorphK - k; // >= 0 on this side
			if( dz < uMorphBand )
			{
				// the wire band: only the lines, glowing in Wire Color; a bright ring where the plane cuts the surface
				vec3 e     = smoothstep( vec3( 0.0 ), bw * 1.4, vi.bary );
				float line = 1.0 - min( min( e.x, e.y ), e.z );
				float ring = uMorphSide == 2 ? 1.0 - smoothstep( 0.0, 0.012, dz ) : 0.0;
				if( max( line, ring ) < 0.4 && dz >= 0.0 )
#ifdef WIRE
					wireCut = 0.0;
#else
					discard;
#endif
				morphEdge = 1.0;
			}
		}
		else if( uMorphMask == 2 )
		{
			float band = floor( clamp( ( ax.z + 1.0 ) * 0.5, 0.0, 0.999 ) * uMorphFreq );
			ahead      = fract( sin( ( band + 1.37 ) * 91.7 + uMorphStep * 7.13 ) * 43758.5453 ) >= uMorphK; // the VS's band hash
		}
		else if( uMorphMask == 3 )
		{
			float r = length( vi.opos );
			ahead   = r >= uMorphK;
			morphEdge = 1.0 - smoothstep( 0.0, 0.035, abs( r - uMorphK ) );
		}
		else
		{
			float k   = length( vi.opos - uMorphSeed ) * 0.5 + 0.22 * ( Fbm( vi.opos * 2.5 + 3.0 ) - 0.5 );
			ahead     = k >= uMorphK;
			float x   = ( k - uMorphK ) / max( uMorphBand, 1e-3 ); // 0 at the front .. 1 at the far edge of the burning band
			morphHeat = uMorphSide == 1 ? 1.0 - smoothstep( 0.0, 1.0, x ) : 0.0;
			morphChar = uMorphSide == 1 ? 1.0 - smoothstep( 0.6, 2.0, x ) : 0.0;
		}
		if( ahead != ( uMorphSide == 1 ) )
#ifdef WIRE
			wireCut = 0.0;
#else
			discard;
#endif
	}
#endif
	// dissolve
	float edgeGlow = 0.0;
	if( uDissolve > 0.0 )
	{
		float n  = Fbm( vi.opos * 3.0 + 7.0 );
		float th = uDissolve * 1.08 - 0.04;
		if( ( n < th ) != ( uDissolveInvert == 1 ) )
#ifdef WIRE
			wireCut = 0.0;
#else
			discard;
#endif
		edgeGlow = uDissolveInvert == 1 ? smoothstep( th - 0.03, th, n ) : 1.0 - smoothstep( th, th + 0.03, n );
	}
#ifdef DEFORM
	// W9: which side of the cut this fragment is on (kept: the solid; removed: the Wire Mode's look, or nothing)
#ifdef PTSHADE
	float xEdge = 1e9;
#else
	float xKeep = 1.0, xEdge = 1e9;
#endif
	if( uXCut > 0 )
	{
		bool kept;
		if( uXRegion == 1 )
		{
			// a rounded box (superellipsoid-like: the corner radius grows to a sphere) in fitted rest space; band in fitted units
			float r = uXRound * uXBox.w;
			vec3 q  = abs( vi.opos - uXBox.xyz ) - vec3( uXBox.w - r );
			float d = length( max( q, 0.0 ) ) + min( max( q.x, max( q.y, q.z ) ), 0.0 ) - r;
			kept    = d <= 0.0;
			xEdge   = abs( d );
		}
		else
		{
#ifdef INSTANCED
			float sd = dot( uXPlaneO.xyz, vi.opos ) + uXPlaneO.w, slab = uXSlab.y;
#else
			float sd = dot( uXPlane.xyz, vi.wpos ) + uXPlane.w, slab = uXSlab.x;
#endif
			kept  = sd <= 0.0 && ( slab <= 0.0 || sd >= -slab );
			xEdge = slab > 0.0 ? min( abs( sd ), abs( sd + slab ) ) : abs( sd );
		}
		xKeep = kept == ( uXCut == 1 ) ? 1.0 : 0.0;
	}
#ifdef SHADOW_PASS
	if( xKeep < 0.5 )
		discard; // the removed part casts no shadow (a small pass: the end-of-shader rule is for the big variants)
#endif
#endif
#ifdef INSTANCED
	// W15: a tunnel ring fading in / out (the W1 dissolve noise; the shadow maps fade with it, fog keep included), and
	// a ghost turning to dust with age: a grain hash on its own surface (the grains travel with the ghost's drift)
	float keep = vInstX.x;
#ifdef SHADOW_PASS
	keep *= vInstX.y;
#endif
	if( keep < 1.0 && Fbm( vi.opos * 3.0 + 29.0 ) < ( 1.0 - keep ) * 1.08 - 0.04 )
		discard;
	if( uTrailPass == 1 && uTrailDust > 0.0 && Hash3( mod( floor( vi.opos / max( uTrailGrain, 1e-4 ) ), 256.0 ) ) < uTrailDust * smoothstep( 0.05, 0.8, vInstX.z ) )
		discard;
#endif

	vec2 uv   = vi.uv;
	vec4 base = uBaseFactor;
	if( uHas[ 0 ] == 1 )
		base *= texture( uTex0, uv );
	if( uCutout == 1 && base.a < 0.5 )
#ifdef WIRE
		wireCut = 0.0;
#else
		discard;
#endif
#ifdef SHADOW_PASS
	fragColor = vec4( uShadowOrtho == 1 ? dot( vi.wpos - uShadowLightPos, uShadowDir ) : length( vi.wpos - uShadowLightPos ) );
	return;
#else

	if( uInputUse == 3 || ( uInputUse == 2 && uPartSkin == 1 ) )
	{
		vec3 skin = SrgbToLinear( texture( uInputTex, InputUV() ).rgb );
		base.rgb  = mix( base.rgb, skin, uInputMix );
	}

	vec3 albedo = HueRotate( base.rgb, uHue + uPartHue + vInstHA.x );
	albedo      = max( mix( vec3( Luma( albedo ) ), albedo, uSaturation ), 0.0 );
	albedo      = mix( albedo, uTint * Luma( albedo ) * 2.0, uTintMix );
	albedo *= uBrightness;

	float rough = uRoughFactor;
	if( uHas[ 2 ] == 1 )
		rough *= Sel( 2, texture( uTex2, uv ) );
	rough       = clamp( rough * uRoughScale, 0.03, 1.0 );
	float metal = uMetalFactor;
	if( uHas[ 3 ] == 1 )
		metal *= Sel( 3, texture( uTex3, uv ) );
	metal    = uMetalOverride == 1 ? uMetalValue : clamp( metal, 0.0, 1.0 );
	float ao = uHas[ 6 ] == 1 ? Sel( 6, texture( uTex6, uv ) ) : 1.0;

	vec3 Ng = normalize( vi.wnrm );
	vec3 T  = normalize( vi.wtan.xyz - Ng * dot( Ng, vi.wtan.xyz ) );
	float w = vi.wtan.w;
	if( !gl_FrontFacing )
	{
		Ng = -Ng;
		T  = -T;
	}
	vec3 B = cross( Ng, T ) * w * ( gl_FrontFacing ? 1.0 : -1.0 );
	vec3 N = Ng;
	if( uHas[ 1 ] == 1 )
	{
		vec3 nt = texture( uTex1, uv ).rgb * 2.0 - 1.0;
		nt.y *= uFlipGreen;
		nt.xy *= uNormalScale * uNormalStrength;
		N = normalize( T * nt.x + B * nt.y + Ng * max( nt.z, 1e-3 ) );
	}

#ifdef PHOTON_PASS
	// W26 caustics: the refractor seen from the caustic light: world position (0) or shading normal (1), a = coverage
	fragColor = uPhotonOut == 0 ? vec4( vi.wpos, 1.0 ) : vec4( N, 1.0 );
	return;
#endif
	vec3 emis = uEmisFactor;
	if( uHas[ 5 ] == 1 )
		emis *= texture( uTex5, uv ).rgb;

	if( uDebugView > 0 )
	{
		vec3 d;
		if( uDebugView == 1 )
			d = Encode( albedo );
		else if( uDebugView == 2 )
			d = N * 0.5 + 0.5;
		else if( uDebugView == 3 )
			d = vec3( rough );
		else if( uDebugView == 4 )
			d = vec3( metal );
		else if( uDebugView == 5 )
			d = vec3( fract( uv ), 0.0 );
		else
		{
			const vec3 pal[ 8 ] = vec3[ 8 ]( vec3( 1, .25, .2 ), vec3( .2, .8, .3 ), vec3( .25, .45, 1 ), vec3( 1, .85, .2 ),
			                                 vec3( .85, .3, 1 ), vec3( .2, .9, .95 ), vec3( 1, .6, .2 ), vec3( .9, .9, .9 ) );
			d = pal[ uPartId % 8 ] * ( 0.55 + 0.45 * max( dot( N, normalize( uCamPos - vi.wpos ) ), 0.0 ) );
		}
		fragColor = vec4( d, 1.0 );
#ifdef PTSHADE
		fragColor.a = -1.0;
#endif
		return;
	}

	vec3 V    = normalize( uCamPos - vi.wpos );
	float nov = max( dot( N, V ), 1e-4 );
	float sAmt = uSurfaceOn == 1 ? uPartSurface : 0.0;
	vec3 filmTint = vec3( 1.0 );
	if( sAmt > 0.0 )
	{
		if( uChrome > 0.0 )
		{
			float k = uChrome * sAmt;
			if( uChromeRipple > 0.0 )
			{
				vec3 q   = vi.opos * 5.0 + vec3( 0.0, uChromeOff );
				float n0 = Noise3P( q );
				vec3 g   = vec3( Noise3P( q + vec3( 0.05, 0, 0 ) ), Noise3P( q + vec3( 0, 0.05, 0 ) ), Noise3P( q + vec3( 0, 0, 0.05 ) ) ) - n0;
				N        = normalize( N - ( g - N * dot( g, N ) ) * 20.0 * uChromeRipple * k );
				nov      = max( dot( N, V ), 1e-4 );
			}
			albedo = mix( albedo, vec3( 0.96, 0.95, 0.93 ), k );
			metal  = mix( metal, 1.0, k );
			rough  = mix( rough, 0.035, k );
		}
		if( uFilm > 0.0 )
		{
			float swirl = 0.0;
			if( uFilmSwirl > 0.0 )
			{
				swirl = Fbm( vi.opos * 2.5 + vec3( uFilmT.x ) );
				if( uFilmT.z > 0.0 )
					swirl = LoopBlend( swirl, Fbm( vi.opos * 2.5 + vec3( uFilmT.y ) ), uFilmT.z );
				swirl *= uFilmSwirl;
			}
			float nm    = uFilmThick * ( 250.0 + 700.0 * swirl );
			float cv    = mix( 1.0, nov, clamp( uFilmShift, 0.0, 2.0 ) * 0.5 );
			filmTint    = mix( vec3( 1.0 ), ThinFilm( cv, nm ) * 1.6, clamp( uFilm * sAmt, 0.0, 1.0 ) );
			albedo *= mix( vec3( 1.0 ), filmTint, 0.5 );
		}
	}
	vec3 F0   = mix( vec3( 0.04 ), albedo, metal ) * filmTint;
	vec3 diff = albedo * ( 1.0 - metal );
	float a   = rough * rough;

	vec3 color = vec3( 0.0 );
#if defined( CRACK_PASS )
	float surfAlpha = 1.0;
	{
		// W26 lava cracks: the Voronoi borders glow (blackbody), behind a spreading front that burns hottest
		vec4 cc    = CrackCell( vi.opos * uCrackScale );
		float e    = cc.x / uCrackScale;           // fitted units to the border
		float dS   = length( vi.opos - uCrackSeed );
		float on   = 1.0 - smoothstep( uCrackFront - 0.06, uCrackFront, dS );
		float fresh = exp( -max( uCrackFront - dS, 0.0 ) * 6.0 ); // the newest cracks are hottest
		float w    = uCrackWidth * 0.03 * ( 0.5 + 0.5 * on ) + 1e-4;
		float aa   = max( fwidth( e ), 1e-5 );
		float core = 1.0 - smoothstep( w * 0.5 - aa, w * 0.5 + aa, e );
		float halo = exp( -e / ( w * 1.5 ) ) * 0.35;
		float flow = Noise3P( vi.opos * 6.0 + vec3( 0.0, uCrackFlow, 0.0 ) );
		float T    = uLavaTemp * ( 0.55 + 0.45 * core ) * ( 0.9 + 0.2 * flow ) * ( 1.0 + 0.35 * fresh ); // W28: calmer flow (it read as breathing)
		vec3 bb    = Blackbody( T );
		bb         = mix( bb, uLavaTint * dot( bb, vec3( 0.3333 ) ) * 1.5, uLavaTintMix );
		color      = bb * pow( T / uLavaTemp, 4.0 ) * uLavaGlow * ( core + halo ) * on * 2.0;
		// the crust darkens round the cracks (light pass: only the glow is added here)
	}
#elif defined( PROJ_PASS )
	float surfAlpha = 1.0;
	for( int i = 0; i < uPLCount; ++i )
	{
		vec4 c = uPLVP[ i ] * vec4( vi.wpos, 1.0 );
		if( c.w <= 1e-5 )
			continue;
		vec2 puv = c.xy / c.w * 0.5 + 0.5;
		vec2 eg  = min( puv, 1.0 - puv );
		if( min( eg.x, eg.y ) <= 0.0 )
			continue;
		vec4 t    = i == 0 ? textureLod( uPLTex0, puv, 0.0 ) : textureLod( uPLTex1, puv, 0.0 );
		vec3 Lv   = uPLPos[ i ] - vi.wpos;
		float d   = length( Lv );
		vec3 L    = Lv / max( d, 1e-5 );
		float nol = max( dot( N, L ), 0.0 );
		float lit = ( uPLExact[ i ] == 1 ? 1.0 - smoothstep( 0.03 + 0.01 * d, 0.06 + 0.02 * d, abs( d - t.a ) ) : step( d - ( 0.02 + 0.01 * d ), t.a ) ) * smoothstep( 0.0, 0.004, min( eg.x, eg.y ) );
		if( nol * lit <= 0.0 )
			continue;
		vec3 H     = normalize( L + V );
		float noh  = max( dot( N, H ), 0.0 );
		float voh  = max( dot( V, H ), 0.0 );
		vec3 F     = F0 + ( 1.0 - F0 ) * pow( 1.0 - voh, 5.0 );
		float spec = DGGX( noh, a ) * VisSmith( nov, max( nol, 1e-4 ), a );
		color += ( diff / PI + F * spec ) * uPLColor[ i ] * t.rgb * ( nol * lit / ( 1.0 + d * d ) );
	}
#else
	for( int li = 0; li < 2; ++li )
	{
		vec3 L    = li == 0 ? uKeyDir : uFillDir;
		vec3 lc   = li == 0 ? uKeyColor : uFillColor;
		float nol = max( dot( N, L ), 0.0 );
		if( uToonSteps > 0.5 )
			nol = floor( nol * uToonSteps + 0.5 ) / uToonSteps;
		vec3 H    = normalize( L + V );
		float noh = max( dot( N, H ), 0.0 );
		float voh = max( dot( V, H ), 0.0 );
		vec3 F    = F0 + ( 1.0 - F0 ) * pow( 1.0 - voh, 5.0 );
		float spec = DGGX( noh, a ) * VisSmith( nov, max( nol, 1e-4 ), a );
		if( uToonSteps > 0.5 )
			spec = step( 1.0, spec * nol ) * 2.0;
		color += ( diff / PI + F * spec ) * lc * nol;
	}
	for( int i = 0; i < uXLCount; ++i )
	{
		vec3 Lv   = uXLPos[ i ] - vi.wpos;
		float d   = length( Lv );
		vec3 L    = Lv / max( d, 1e-5 );
		float nol = max( dot( N, L ), 0.0 );
		float k   = XLCone( i, -L ) * XLWindow( d ) * nol;
		if( k <= 0.0 )
			continue;
		k *= XShadow( i, vi.wpos + Ng * 0.01, d );
		vec3 H     = normalize( L + V );
		float noh  = max( dot( N, H ), 0.0 );
		float voh  = max( dot( V, H ), 0.0 );
		vec3 F     = F0 + ( 1.0 - F0 ) * pow( 1.0 - voh, 5.0 );
		float spec = DGGX( noh, a ) * VisSmith( nov, max( nol, 1e-4 ), a );
		color += ( diff / PI + F * spec ) * uXLColor[ i ] * k;
	}
	vec3 R       = reflect( -V, N );
	vec2 ab      = EnvBRDF( rough, nov );
	vec3 envDiff = Env( N, 1.0 );
	if( uToonSteps > 0.5 )
		envDiff = floor( envDiff * uToonSteps + 0.5 ) / uToonSteps;
	vec3 envSpec = Env( R, rough ) * ( F0 * ab.x + ab.y );
	color += ( diff * envDiff + envSpec ) * uEnvIntensity * ao;
	color += albedo * uAmbient * ao;

	float fres = pow( 1.0 - nov, 3.0 );
	color += uRimColor * uRimGlow * fres;
	color += emis * uEmissiveGlow;
	color += albedo * uPartGlow;
	color += uRimColor * edgeGlow * uDissolveEdge * 2.0;

	if( uContour > 0.0 )
	{
		float sp   = max( uContourSpacing, 1e-3 );
		float x    = fract( cc / sp - uContourScroll );
		float d    = min( x, 1.0 - x ) * sp;
		float hw   = uContourWidth * sp * 0.5;
		float line = 1.0 - smoothstep( hw - cfw, hw + cfw, d );
		float halo = exp( -d / ( sp * 0.15 ) ) * 0.35;
		color += uContourColor * ( line + halo ) * uContour;
	}
#ifndef PTSHADE
	float surfAlpha = 1.0;
#endif
	if( sAmt > 0.0 )
	{
		if( uGlitter > 0.0 )
		{
			vec3 cp = vi.opos * ( 36.0 / max( uGlitterSize, 0.05 ) );
			vec3 ci = floor( cp );
			float h = Hash3( ci );
			if( h < uGlitterDensity )
			{
				vec3 jn     = normalize( N + ( vec3( Hash3( ci + 1.31 ), Hash3( ci + 2.73 ), Hash3( ci + 4.19 ) ) - 0.5 ) * 1.4 );
				float flake = smoothstep( 0.5, 0.15, length( fract( cp ) - 0.5 ) );
				// Twinkle = twinkles per second per flake; each flake has its own phase and a +-20 % rate, rounded to a
				// whole number of twinkles per 1000 so the phase's wrap at 1000 is seamless
				float rate  = floor( ( 0.8 + 0.4 * Hash3( ci + 7.7 ) ) * 1000.0 + 0.5 ) / 1000.0;
				float tw    = 0.5 + 0.5 * sin( 6.2831853 * fract( uGlitterPhase * rate ) + h * 97.0 );
				vec3 rj     = reflect( -V, jn );
				float kspec = pow( max( dot( jn, normalize( V + uKeyDir ) ), 0.0 ), 28.0 );
				float espec = pow( max( dot( rj, normalize( vec3( -0.65, 0.55, 0.55 ) ) ), 0.0 ), 16.0 )
				            + pow( max( dot( rj, normalize( vec3( 0.85, 0.15, 0.25 ) ) ), 0.0 ), 16.0 )
				            + pow( max( dot( rj, vec3( 0.0, 0.0, 1.0 ) ), 0.0 ), 16.0 );
				float w     = mix( 1.0, metal, clamp( uGlitterMetal, 0.0, 1.0 ) );
				vec3 tint   = mix( vec3( 1.0 ), albedo / max( max( albedo.r, max( albedo.g, albedo.b ) ), 1e-3 ), metal * 0.7 );
				color += ( uKeyColor * kspec + vec3( 3.0 ) * espec ) * tint * flake * tw * tw * uGlitter * w * sAmt * 2.5;
			}
		}
		if( uHeat > 0.0 )
		{
			float hh = uHas[ 4 ] == 1 ? Sel( 4, texture( uTex4, uv ) ) : Luma( base.rgb );
			float T  = uHeatTemp * mix( 1.0, 0.3 + 1.4 * hh, clamp( uHeatSpread, 0.0, 1.0 ) );
			color += Blackbody( T ) * pow( T / 1600.0, 4.0 ) * 0.12 * uHeat * sAmt;
		}
		if( uPattern > 0 && uPatAmt > 0.0 )
		{
			vec3 q  = vi.opos * uPatScale;
			vec3 bw3 = pow( abs( Ng ), vec3( 4.0 ) );
			bw3 /= bw3.x + bw3.y + bw3.z + 1e-5;
			float pv = PatternX( q.yz ) * bw3.x + PatternX( q.xz ) * bw3.y + PatternX( q.xy ) * bw3.z;
			color += uPatColor * pv * uPatAmt * ( 1.0 + uPatBeat * uBeat * 2.0 + uPatAudio * uAudioMid ) * sAmt;
		}
		if( uGlass > 0.0 )
		{
			float k   = clamp( uGlass * sAmt, 0.0, 1.0 );
			float ior = max( uGlassIOR, 1.0001 );
			vec3 disp = vec3( -1.0, 0.0, 1.0 ) * uGlassDisp * 0.06;
			vec3 refr;
			if( uGlassInput == 1 )
			{
				vec2 suv = PT_FRAGCOORD.xy / uViewport;
				for( int c = 0; c < 3; ++c )
				{
					vec3 rd = refract( -V, N, 1.0 / ( ior + disp[ c ] ) );
					vec2 o  = ( mat3( uView ) * rd ).xy * 0.12 * ( ior - 1.0 ) * 2.0;
					refr[ c ] = SrgbToLinear( textureLod( uInputTex, suv + o, 1.0 ).rgb )[ c ];
				}
			}
			else
				for( int c = 0; c < 3; ++c )
					refr[ c ] = Env( refract( -V, N, 1.0 / ( ior + disp[ c ] ) ), 0.05 )[ c ];
			float fr   = 0.04 + 0.96 * pow( 1.0 - nov, 5.0 );
			vec3 glass = refr * uGlassTint * ( 1.0 - fr ) + Env( reflect( -V, N ), 0.02 ) * fr * uEnvIntensity;
			color      = mix( color, glass, k );
			if( uGlassInput == 0 )
				surfAlpha = mix( surfAlpha, clamp( 0.25 + fr + Luma( glass ) * 0.5, 0.0, 1.0 ), k );
		}
		if( uHolo > 0.0 )
		{
			float k   = clamp( uHolo * sAmt, 0.0, 1.0 );
			float fr  = pow( 1.0 - nov, 1.5 );
			float sp  = uHoloSplit * 0.004;
			vec3 scan = 0.55 + 0.45 * sin( ( vi.wpos.y + vec3( sp, 0.0, -sp ) ) * 140.0 * max( uHoloScan, 0.05 ) - uHoloPhase );
			float fl  = 1.0 - uHoloFlicker * 0.45 * step( 0.72, fract( sin( floor( uTime * 24.0 ) * 12.9898 ) * 43758.5453 ) );
			vec3 hc   = uHoloColor * scan * ( 0.2 + 2.2 * fr ) * fl * 2.0;
			color     = mix( color, hc, k );
			surfAlpha = mix( surfAlpha, clamp( ( 0.12 + 0.9 * fr ) * ( 0.6 + 0.4 * scan.g ) * fl, 0.0, 1.0 ), k );
		}
	}

#endif
#if defined( DEFORM ) && !defined( SHADOW_PASS ) && !defined( PHOTON_PASS )
	if( uXCut == 1 && uXGlow > 0.0 )
		color += uXColor * exp( -xEdge * xEdge / max( uXBand * uXBand, 1e-10 ) ) * uXGlow; // the print head / the cut's glowing band
	if( uXLayer > 0.0 && uXCut == 1 )
	{
		// printed layers: a fine ridge every layer
		float y = fract( ( dot( uXPlaneO.xyz, vi.opos ) ) / uXLayer );
		color *= 0.82 + 0.18 * smoothstep( 0.0, 0.25, y ) * ( 1.0 - smoothstep( 0.75, 1.0, y ) );
	}
	if( uXLook == 4 )
		color = color * ( 1.0 - 0.85 * vXFall ) + Blackbody( 1500.0 + 3000.0 * vXFall ) * vXFall * vXFall * 2.5; // infalling matter heats up
#endif
#ifdef MORPH
	if( uMorphMask == 1 || uMorphMask == 3 )
		color = mix( color, uMorphColor * ( 1.5 + uMorphGlow ), morphEdge ); // scan's wire band / ripple's front, in Wire Color
	else if( uMorphMask == 4 )
	{
		// burn: the crust chars toward the front, which glows (blackbody of Flame Temp, x Flame Glow)
		color *= 1.0 - 0.9 * morphChar;
		color += Blackbody( uMorphTemp * ( 0.55 + 0.45 * morphHeat ) ) * morphHeat * morphHeat * uMorphFlame * 1.5;
	}
#endif
#ifdef INSTANCED
	if( uTrailPass == 1 ) // ghosts take the trail colour (keeping their shading) and glow in it
		color = mix( color, uTrailColor * ( 0.15 + 1.2 * Luma( color ) ), uTrailMix ) + uTrailColor * uTrailGlow * 0.25;
#endif
	color *= exp2( uExposure );
#ifdef PTSHADE
	outc        = Encode( Tonemap( color ) );
#else
	vec3 outc   = Encode( Tonemap( color ) );
#endif
#if defined( DEFORM ) && !defined( SHADOW_PASS ) && !defined( PHOTON_PASS ) && !defined( CRACK_PASS ) && !defined( PROJ_PASS )
	if( uXSurf >= 3 )
	{
		// W9 Stage D: the camera looks, as shading modes of the object (they combine with the rest; Surf Amount = how much)
		float k  = clamp( uXSurfAmt, 0.0, 1.0 );
		vec3 lk  = outc;
		float gz = 1.0 - nov;
		if( uXSurf == 3 )
		{
			// X-RAY: see-through by the viewing angle (drawn additively, both faces, no depth): the edges (grazing = thick) glow
			float edge = pow( gz, 1.0 + 3.0 * ( 1.0 - clamp( uXSurfScale, 0.0, 1.0 ) ) );
			lk = mix( vec3( 0.55, 0.75, 1.0 ), uXColor, uXColorMix ) * ( 0.06 + 1.4 * edge ) * ( 0.85 + 0.15 * sin( 6.2831853 * uXSurfPhase + vi.opos.y * 8.0 ) );
			surfAlpha = mix( surfAlpha, 0.08 + 0.5 * edge, k );
		}
		else if( uXSurf == 4 )
		{
			// THERMAL: heat from the shaded brightness, the facing side warmer, a slow shimmer; Surf Scale = contrast
			float h = clamp( 0.05 + 0.6 * Luma( outc ) + 0.22 * ( 1.0 - gz * gz ) + 0.15 * ( Noise3P( vi.opos * 3.0 + vec3( 0.0, uXSurfPhase * 3.2, 0.0 ) ) - 0.5 ), 0.0, 1.0 ); // 64-periodic: 1000 x 3.2 = 50 tiles, seamless at the wrap
			lk = Thermal( pow( h, 0.4 + 1.6 * clamp( uXSurfScale, 0.0, 1.0 ) ) );
		}
		else if( uXSurf == 5 )
		{
			// NIGHT VISION: green phosphor, gain, grain (Surf Scale = its size; a new grain 30 times a second), scan lines, a vignette
			// round the object, highlights blooming
			float L  = Luma( outc ) * 1.8 + 0.04;
			vec2 px  = floor( PT_FRAGCOORD.xy / ( 1.0 + 3.0 * clamp( uXSurfScale, 0.0, 1.0 ) ) );
			float g  = fract( sin( dot( px + floor( uXSurfPhase * 30.0 ) * vec2( 17.3, 31.1 ), vec2( 12.9898, 78.233 ) ) ) * 43758.5453 );
			L       *= ( 0.75 + 0.5 * g ) * ( 0.85 + 0.15 * sin( PT_FRAGCOORD.y * 1.6 ) );
			float v  = length( ( PT_FRAGCOORD.xy - uXScreen.xy ) / max( uXScreen.z, 1.0 ) );
			L       *= 1.0 - 0.8 * smoothstep( 0.5, 1.3, v );
			lk = vec3( 0.25, 1.0, 0.35 ) * L + vec3( 0.6, 1.0, 0.6 ) * max( L - 0.8, 0.0 ) * 2.5;
		}
		else
		{
			// LED MATRIX: round LEDs on a screen grid (Surf Scale = the pitch, 4 .. 32 px), each lit by the shaded colour (quantised),
			// dark gaps (transparent), a slow brightness roll down the rows
			float pitch = 4.0 + 28.0 * clamp( uXSurfScale, 0.0, 1.0 );
			vec2 cell   = PT_FRAGCOORD.xy / pitch;
			float r     = length( fract( cell ) - 0.5 ), aa = 1.2 / pitch;
			float dotm  = 1.0 - smoothstep( 0.36 - aa, 0.36 + aa, r );
			float roll  = 0.85 + 0.15 * ( 0.5 + 0.5 * cos( 6.2831853 * ( floor( cell.y ) / 24.0 - uXSurfPhase ) ) );
			lk = min( floor( outc * 7.0 + 0.5 ) / 7.0 * 1.25 * roll, 1.0 ) * dotm;
			surfAlpha *= mix( 1.0, dotm, k );
		}
		outc = mix( outc, lk, k );
	}
#endif
#ifdef PTSHADE
	fragColor = vec4( outc, PT_KEEP ); // pass 1: the point's shaded colour into its texel
	return;
	}
#endif
	// Branch-free on purpose: a discard inside this block miscompiles on Apple's GL-over-Metal.
#ifdef WIRE
#ifdef POINTS
#ifdef PTFX
	// W4: the shape; Size Var's luma share shrinks the shape inside its sprite (the brighter the surface, the bigger)
	vec3 ptLit;
	float ptL   = dot( outc, vec3( 0.2126, 0.7152, 0.0722 ) );
	float ptS   = mix( 1.0, ( 0.4 + 1.2 * clamp( ptL, 0.0, 1.0 ) ) / 1.6, vPtA.z );
	vec2 ptQ    = vec2( gl_PointCoord.x * 2.0 - 1.0, 1.0 - gl_PointCoord.y * 2.0 ) / ptS;
	float wire  = PtCover( ptQ, 2.0 / max( vPtA.y * ptS, 1.0 ), ptLit ) * vTwinkle * vPtA.w;
#else
	// a round sprite, anti-aliased over its last pixel
	float pr    = length( gl_PointCoord * 2.0 - 1.0 );
	float wire  = ( 1.0 - smoothstep( 1.0 - 2.0 / max( uPointPx, 1.0 ), 1.0, pr ) ) * vTwinkle;
#endif
#else
	// an edge the flags leave out (Feature Edges) never draws: its barycentric distance counts as far
	vec3 e      = max( smoothstep( vec3( 0.0 ), bw * uWireThickness, vi.bary ), vec3( 1.0 ) - vEdge );
	float wire  = 1.0 - min( min( e.x, e.y ), e.z );
#endif
	wire       *= 1.0 - uWireFade * clamp( ( length( uCamPos - vi.wpos ) - uWireFadeNear ) / max( uWireFadeFar - uWireFadeNear, 1e-4 ), 0.0, 1.0 );
	vec3 lineC  = mix( min( outc * 1.6 + 0.08, 1.0 ), uWireColor, uWireColorMix );
#if defined( POINTS ) && defined( PTFX )
	lineC *= ptLit;
#endif
	float wa    = wire * uWireMix;
	float alpha = 1.0;
	if( uWirePass == 3 )
	{
		outc  = mix( outc, lineC, wa );
		alpha = mix( 1.0, wire, uWireMix ) * surfAlpha * vInstHA.y;
	}
	else if( uWirePass == 1 )
		outc = lineC * wa, alpha = vInstHA.y;
	else if( uWirePass == 2 || uWirePass == 4 )
		outc = lineC, alpha = wa * vInstHA.y;
	alpha *= wireCut;
#else
	vec3 e      = smoothstep( vec3( 0.0 ), bw * uWireThickness, vi.bary );
	float wire  = 1.0 - min( min( e.x, e.y ), e.z );
	outc        = mix( outc, min( outc * 1.6 + 0.08, 1.0 ), wire * uWireMix );
	float alpha = mix( 1.0, wire, uWireMix ) * surfAlpha * vInstHA.y;
#endif

#ifdef DEFORM
	alpha *= xKeep;
#endif
	vec4 result = vec4( outc * alpha, alpha );
	if( uFogDensity > 0.0 )
	{
		float dist = length( uCamPos - vi.wpos );
		float fog  = 1.0 - exp( -uFogDensity * max( dist - uFogStart, 0.0 ) );
#ifdef INSTANCED
		fog = max( fog, 1.0 - vInstX.y ); // the tunnel's far end: rings arrive out of the fog
#endif
		result     = mix( result, uBackground, fog );
	}
	fragColor = result;
#if defined( PROJ_PASS ) || defined( CRACK_PASS )
	fragColor = vec4( result.rgb, 0.0 ); // added light: the mask's coverage is already there
#endif
#ifdef WIRE
	if( uWirePass == 0 )
		fragColor = vec4( 0.0 ); // depth only (alpha stays 1 for the discard test: the depth is written)
	else if( uWirePass == 4 )
		fragColor = vec4( lineC * alpha, 0.0 ); // light, added into the wire buffer
#endif
	if( alpha < 0.004 )
		discard;
#endif
}
)GLSL";

static const char* kQuadVS = R"GLSL(
out vec2 vUV;
void main()
{
	vec2 p      = vec2( ( gl_VertexID << 1 ) & 2, gl_VertexID & 2 );
	vUV         = p;
	gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );
}
)GLSL";

// MoveEhr W5 (S5) Env Backdrop: the HDRI behind everything already composited (drawn "under": ONE_MINUS_DST_ALPHA, ONE), through
// the camera; exposure + tonemap + encode as the model's own output. Amount = its opacity.
static const char* kBackdropFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uEnvFile;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform float uEnvRotate, uExposure, uBackdrop;
uniform int uTonemap;
vec3 Tonemap( vec3 c )
{
	if( uTonemap == 0 )
	{
		c *= 0.6;
		return clamp( ( c * ( 2.51 * c + 0.03 ) ) / ( c * ( 2.43 * c + 0.59 ) + 0.14 ), 0.0, 1.0 );
	}
	if( uTonemap == 1 )
	{
		const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30, W = 11.2;
		vec3 x        = c * 2.0;
		vec3 m        = ( ( x * ( A * x + C * B ) + D * E ) / ( x * ( A * x + B ) + D * F ) ) - E / F;
		float w       = ( ( W * ( A * W + C * B ) + D * E ) / ( W * ( A * W + B ) + D * F ) ) - E / F;
		return clamp( m / w, 0.0, 1.0 );
	}
	return clamp( c, 0.0, 1.0 );
}
void main()
{
	vec4 q   = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 d   = normalize( q.xyz / q.w - uCamPos );
	float ca = cos( uEnvRotate ), sa = sin( uEnvRotate );
	d        = normalize( vec3( ca * d.x + sa * d.z, d.y, -sa * d.x + ca * d.z ) );
	vec2 uv  = vec2( atan( d.x, -d.z ) * 0.15915494 + 0.5, 0.5 - asin( clamp( d.y, -1.0, 1.0 ) ) * 0.31830989 );
	vec3 c   = textureLod( uEnvFile, uv, 0.0 ).rgb * exp2( uExposure );
	fragColor = vec4( pow( max( Tonemap( c ), 0.0 ), vec3( 1.0 / 2.2 ) ) * uBackdrop, uBackdrop );
}
)GLSL";

// Composites the resolved model (premultiplied) over the background into the host FBO, then the volume
// light (fog) on top: Add = premultiplied additive glow (alpha untouched), Over = alpha from the density.
// The fog may be rendered at half / quarter res: it is upsampled here, weighted by depth similarity.
static const char* kCompositeFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uScene;
uniform sampler2D uInput;
uniform vec2 uInputScale;
uniform int uUseInput;
uniform vec4 uBackground; // premultiplied
uniform int uFogOn;
uniform int uFogBlend;     // 0 add, 1 over
uniform int uFogDepthAware;
uniform sampler2D uFog;
uniform sampler2D uFogDepth;
uniform sampler2D uDepth;
uniform vec2 uFogSize;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
// floor / wall under the scene (Stage 2): shadow catcher + mirror
uniform int uFloorOn;
uniform vec4 uPlane;         // mirror floor: n.xyz, d with dot( n, p ) + d = 0
uniform vec4 uCatchPlane;    // shadow catcher: floor or wall
uniform float uShadowAmt;
uniform float uShadowSoft;
uniform float uFloorLight;      // x Shadow Catcher: a soft key-lit pool round the mask, so the shadow reads over black
uniform vec3 uFloorLightColor;
uniform mat4 uKeyVP;
uniform vec3 uKeyPos;
uniform vec3 uKeyDir;        // direction the key light travels
uniform sampler2D uKeyShadow;
uniform int uMirrorOn;
uniform sampler2D uMirror;
uniform float uMirrorAmt;
uniform float uMirrorBlur;
uniform float uMirrorFade;
uniform vec3 uMaskCenter;
// silhouette (Stage 2): black fill, glowing outline
uniform int uSilOn;
uniform float uSil;
uniform float uSilFill;
uniform float uSilGlow;
uniform vec3 uSilColor;
uniform sampler2D uSilBlur;  // blurred scene alpha
uniform vec2 uTexel;
// echo trails (Stage 5): under the live mask
uniform int uEchoOn;
uniform sampler2D uEcho;

vec4 FogSample()
{
	if( uFogDepthAware == 0 )
		return texture( uFog, vUV );
	float z  = texture( uDepth, vUV ).r;
	float tf = 1e4;
	if( z < 1.0 )
	{
		vec4 s = uInvViewProj * vec4( vUV * 2.0 - 1.0, z * 2.0 - 1.0, 1.0 );
		tf     = min( length( s.xyz / s.w - uCamPos ), 1e4 );
	}
	vec2 fc  = vUV * uFogSize - 0.5;
	vec2 i0  = floor( fc );
	vec2 fr  = fc - i0;
	vec4 acc = vec4( 0.0 );
	float ws = 0.0;
	for( int k = 0; k < 4; ++k )
	{
		vec2 o   = vec2( k & 1, k >> 1 );
		vec2 uv  = ( i0 + o + 0.5 ) / uFogSize;
		float bw = ( o.x > 0.5 ? fr.x : 1.0 - fr.x ) * ( o.y > 0.5 ? fr.y : 1.0 - fr.y );
		float tl = textureLod( uFogDepth, uv, 0.0 ).r;
		float w  = bw / ( 1e-3 + abs( tl - tf ) / max( tf, 1e-3 ) * 20.0 );
		acc += textureLod( uFog, uv, 0.0 ) * w;
		ws += w;
	}
	return ws > 1e-6 ? acc / ws : texture( uFog, vUV );
}

const vec2 kPoisson[ 12 ] = vec2[ 12 ]( vec2( -0.326, -0.406 ), vec2( -0.840, -0.074 ), vec2( -0.696, 0.457 ), vec2( -0.203, 0.621 ),
                                         vec2( 0.962, -0.195 ), vec2( 0.473, -0.480 ), vec2( 0.519, 0.767 ), vec2( 0.185, -0.893 ),
                                         vec2( 0.507, 0.064 ), vec2( 0.896, 0.412 ), vec2( -0.322, -0.933 ), vec2( -0.792, -0.598 ) );

// The shadow catcher and the mirror live on invisible planes under the scene.
bool PlaneHit( vec4 pl, out vec3 P )
{
	vec4 f     = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd    = normalize( f.xyz / f.w - uCamPos );
	float side = dot( pl.xyz, uCamPos ) + pl.w;
	float den  = dot( pl.xyz, rd );
	P          = uCamPos + rd * ( -side / min( den, -1e-5 ) );
	return side > 0.0 && den < -1e-5;
}
vec4 FloorLayer( vec4 bg )
{
	vec3 P;
	if( uMirrorOn == 1 && PlaneHit( uPlane, P ) )
	{
		float dist = length( ( P - uMaskCenter ) - uPlane.xyz * dot( P - uMaskCenter, uPlane.xyz ) );
		float fade = exp( -dist * ( 0.15 + 3.0 * uMirrorFade ) );
		float r    = uMirrorBlur * ( 0.004 + 0.03 * dist );
		vec4 m     = texture( uMirror, vUV );
		for( int i = 0; i < 12; ++i )
			m += texture( uMirror, vUV + kPoisson[ i ] * r * vec2( uTexel.y / uTexel.x, 1.0 ) );
		m /= 13.0;
		bg = m * ( uMirrorAmt * fade ) + bg * ( 1.0 - m.a * uMirrorAmt * fade );
	}
	if( uShadowAmt > 0.0 && PlaneHit( uCatchPlane, P ) )
	{
		vec4 c   = uKeyVP * vec4( P, 1.0 );
		vec2 uv  = c.xy / c.w * 0.5 + 0.5;
		float oc = 0.0;
		if( all( greaterThan( uv, vec2( 0.0 ) ) ) && all( lessThan( uv, vec2( 1.0 ) ) ) )
		{
			float d = dot( P - uKeyPos, uKeyDir );
			float r = 0.002 + uShadowSoft * 0.03;
			for( int i = 0; i < 12; ++i )
				oc += step( texture( uKeyShadow, uv + kPoisson[ i ] * r ).r, d - 0.03 );
			oc /= 12.0;
		}
		if( uFloorLight > 0.0 )
		{
			// the pool is lit by the key, so the shadow cuts it
			float dist = length( ( P - uMaskCenter ) - uCatchPlane.xyz * dot( P - uMaskCenter, uCatchPlane.xyz ) );
			float a    = uFloorLight * exp( -dist * dist * 0.35 ) * ( 1.0 - oc );
			bg         = vec4( uFloorLightColor * a, a ) + bg * ( 1.0 - a );
		}
		bg = bg * ( 1.0 - oc * uShadowAmt ) + vec4( 0.0, 0.0, 0.0, oc * uShadowAmt );
	}
	return bg;
}

#ifdef HAZE
// W26 caustics on the catcher plane (the same variant as the haze; uCausticOn gates it): the caustic map (light space, a =
// the receiver distance) projected onto the plane where the light reaches it, added (light at alpha 0)
uniform int uCausticOn;
uniform sampler2D uCaustic;
uniform mat4 uCausticVP;
uniform vec3 uCausticPos, uCausticColor;
vec3 CausticFloor()
{
	vec3 P;
	if( uCausticOn == 0 || !PlaneHit( uCatchPlane, P ) )
		return vec3( 0.0 );
	vec4 c = uCausticVP * vec4( P, 1.0 );
	if( c.w <= 1e-5 )
		return vec3( 0.0 );
	vec2 uv = c.xy / c.w * 0.5 + 0.5;
	vec2 e  = min( uv, 1.0 - uv );
	if( min( e.x, e.y ) <= 0.0 )
		return vec3( 0.0 );
	vec4 t  = texture( uCaustic, uv );
	float d = length( P - uCausticPos );
	return uCausticColor * t.rgb * step( d - 0.05 - 0.01 * d, t.a ) * smoothstep( 0.0, 0.02, min( e.x, e.y ) );
}
// W26 heat haze (its own composite variant): the picture shimmers where the lava cracks are (a half-res mask of their glow,
// read a little below each pixel too: the heat rises). The phase wraps at 1; it is read at x3 and x2 (whole cycles).
uniform sampler2D uHazeMask;
uniform float uHazeAmt, uHazePhase;
vec2 HazeOffset()
{
	float m = 0.0;
	for( int k = 0; k < 8; ++k )
	{
		vec2 o = vec2( float( k % 3 ) - 1.0, -float( k / 3 ) ) * vec2( 0.012, 0.018 );
		m += dot( texture( uHazeMask, vUV + o ).rgb, vec3( 0.3333 ) );
	}
	m = min( m * 0.25, 1.5 );
	float a = sin( 6.2831853 * ( vUV.y * 22.0 - 3.0 * uHazePhase ) + 2.0 * sin( vUV.x * 37.0 ) );
	float b = sin( 6.2831853 * ( vUV.x * 17.0 + 2.0 * uHazePhase ) + 2.0 * sin( vUV.y * 29.0 ) );
	return vec2( a, b ) * m * uHazeAmt * 0.004;
}
#endif
void main()
{
#ifdef HAZE
	vec2 hz = uHazeAmt > 0.0 ? HazeOffset() : vec2( 0.0 );
	vec4 s  = texture( uScene, vUV + hz );
	vec4 bg = uUseInput == 1 ? texture( uInput, ( vUV + hz ) * uInputScale ) : uBackground;
	bg.rgb += CausticFloor();
#else
	vec4 s  = texture( uScene, vUV );
	vec4 bg = uUseInput == 1 ? texture( uInput, vUV * uInputScale ) : uBackground;
#endif
	if( uFloorOn == 1 )
		bg = FloorLayer( bg );
	if( uEchoOn == 1 )
	{
		vec4 e = texture( uEcho, vUV );
		bg     = e + bg * ( 1.0 - e.a );
	}
	if( uSilOn == 1 )
	{
		// black fill (the Fill slider reveals the lit mask) + a glow outside the silhouette and a crisp edge
		float a   = s.a;
		float bl  = texture( uSilBlur, vUV ).a;
		float mn  = min( min( texture( uScene, vUV + vec2( uTexel.x, 0.0 ) ).a, texture( uScene, vUV - vec2( uTexel.x, 0.0 ) ).a ),
		                 min( texture( uScene, vUV + vec2( 0.0, uTexel.y ) ).a, texture( uScene, vUV - vec2( 0.0, uTexel.y ) ).a ) );
		float edge = clamp( a - mn, 0.0, 1.0 );
		float glow = clamp( bl * ( 1.0 - a ) * 2.0, 0.0, 1.0 );
		vec4 fill  = vec4( s.rgb * uSilFill, a );
		s          = mix( s, fill, uSil );
		vec3 g     = uSilColor * ( glow + edge ) * uSilGlow * uSil;
		s.rgb += g;
		s.a = max( s.a, clamp( max( g.r, max( g.g, g.b ) ), 0.0, 1.0 ) );
	}
	fragColor = s + bg * ( 1.0 - s.a );
	if( uFogOn == 1 )
	{
		vec4 f  = FogSample();
		vec3 fl = ToDisplay( f.rgb );
		if( uFogBlend == 1 )
		{
			// Over: the lit medium covers what is behind it; unlit haze stays clear
			float a   = clamp( max( fl.r, max( fl.g, fl.b ) ), 0.0, 1.0 );
			fragColor = vec4( fl, a ) + fragColor * ( 1.0 - a );
		}
		else
			fragColor.rgb += fl;
	}
}
)GLSL";

// Copies the host input (possibly padded to hardware size) into a mipmappable texture.
static const char* kCopyFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uInput;
uniform vec2 uInputScale;
void main() { fragColor = texture( uInput, vUV * uInputScale ); }
)GLSL";
}
