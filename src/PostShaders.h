#pragma once
// GLSL 4.10 sources for Stage 2 post: bloom (bright pass, down / up chain), lens flare ghosts + halo,
// anamorphic streak, the final add into the host FBO, and a separable blur (silhouette glow).
// All run on kQuadVS. Inputs are the composited, premultiplied output.

namespace mehr
{
// bright pass with a soft knee, 4-tap box downsample from the full-res composite
static const char* kBrightFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uSrcTexel;
uniform float uThreshold;
void main()
{
	vec3 c = vec3( 0.0 );
	for( int i = 0; i < 4; ++i )
		c += texture( uSrc, vUV + ( vec2( i & 1, i >> 1 ) - 0.5 ) * uSrcTexel ).rgb;
	c *= 0.25;
	float l    = max( c.r, max( c.g, c.b ) );
	float knee = 0.15;
	float s    = clamp( l - uThreshold + knee, 0.0, 2.0 * knee );
	s          = s * s / ( 4.0 * knee + 1e-5 );
	float w    = max( s, l - uThreshold ) / max( l, 1e-4 );
	fragColor  = vec4( c * max( w, 0.0 ), 1.0 );
}
)GLSL";

// 13-tap downsample (Jimenez 2014, "Next generation post processing in Call of Duty")
static const char* kDownFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uSrcTexel;
void main()
{
	vec2 t  = uSrcTexel;
	vec3 a  = texture( uSrc, vUV + t * vec2( -2, 2 ) ).rgb, b = texture( uSrc, vUV + t * vec2( 0, 2 ) ).rgb, c = texture( uSrc, vUV + t * vec2( 2, 2 ) ).rgb;
	vec3 d  = texture( uSrc, vUV + t * vec2( -2, 0 ) ).rgb, e = texture( uSrc, vUV ).rgb, f = texture( uSrc, vUV + t * vec2( 2, 0 ) ).rgb;
	vec3 g  = texture( uSrc, vUV + t * vec2( -2, -2 ) ).rgb, h = texture( uSrc, vUV + t * vec2( 0, -2 ) ).rgb, i = texture( uSrc, vUV + t * vec2( 2, -2 ) ).rgb;
	vec3 j  = texture( uSrc, vUV + t * vec2( -1, 1 ) ).rgb, k = texture( uSrc, vUV + t * vec2( 1, 1 ) ).rgb;
	vec3 l  = texture( uSrc, vUV + t * vec2( -1, -1 ) ).rgb, m = texture( uSrc, vUV + t * vec2( 1, -1 ) ).rgb;
	vec3 o  = e * 0.125 + ( a + c + g + i ) * 0.03125 + ( b + d + f + h ) * 0.0625 + ( j + k + l + m ) * 0.125;
	fragColor = vec4( o, 1.0 );
}
)GLSL";

// tent upsample of the lower level, added to this level
static const char* kUpFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uLow;
uniform sampler2D uCur;
uniform vec2 uLowTexel;
uniform float uRadius;
void main()
{
	vec2 t = uLowTexel;
	vec3 s = texture( uLow, vUV + t * vec2( -1, 1 ) ).rgb + texture( uLow, vUV + t * vec2( 0, 1 ) ).rgb * 2.0 + texture( uLow, vUV + t * vec2( 1, 1 ) ).rgb
	       + texture( uLow, vUV + t * vec2( -1, 0 ) ).rgb * 2.0 + texture( uLow, vUV ).rgb * 4.0 + texture( uLow, vUV + t * vec2( 1, 0 ) ).rgb * 2.0
	       + texture( uLow, vUV + t * vec2( -1, -1 ) ).rgb + texture( uLow, vUV + t * vec2( 0, -1 ) ).rgb * 2.0 + texture( uLow, vUV + t * vec2( 1, -1 ) ).rgb;
	fragColor = vec4( texture( uCur, vUV ).rgb + s / 16.0 * uRadius, 1.0 );
}
)GLSL";

// Lens flare ghosts + halo (after John Chapman's "pseudo lens flare"), from the bright pass
static const char* kGhostFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform int uGhosts;
uniform float uAspect;
// only the brightest part of the bright pass makes ghosts
vec3 Hot( vec3 c ) { return max( c - 0.35, 0.0 ) * 1.6; }
vec3 Chroma( vec2 uv, vec2 dir, float amt )
{
	return Hot( vec3( texture( uSrc, uv + dir * amt ).r, texture( uSrc, uv ).g, texture( uSrc, uv - dir * amt ).b ) );
}
void main()
{
	vec2 tc      = 1.0 - vUV;
	vec2 ghostV  = ( 0.5 - tc ) * 0.37;
	vec2 dir     = normalize( ghostV + 1e-6 );
	vec3 acc     = vec3( 0.0 );
	for( int i = 0; i < uGhosts; ++i )
	{
		vec2 o   = fract( tc + ghostV * float( i ) );
		float w  = pow( max( 1.0 - length( 0.5 - o ) / 0.7071, 0.0 ), 10.0 );
		vec3 tint = 0.6 + 0.4 * cos( 6.2831853 * ( float( i ) * 0.23 + vec3( 0.0, 0.33, 0.67 ) ) );
		acc += Chroma( o, dir, 0.006 ) * w * tint;
	}
	vec2 haloV = dir * vec2( 1.0, uAspect ) * 0.45 / max( uAspect, 1e-3 );
	vec2 ho    = fract( tc + haloV );
	float hw   = pow( max( 1.0 - length( 0.5 - ho ) / 0.7071, 0.0 ), 5.0 );
	acc += Chroma( ho, dir, 0.012 ) * hw * 0.3;
	fragColor = vec4( acc, 1.0 );
}
)GLSL";

// 1D gaussian-ish blur along uDir (streak: horizontal, wide; silhouette glow: both axes). Carries alpha.
static const char* kBlur1DFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uDir; // texel step
uniform int uTaps;
void main()
{
	vec4 acc = texture( uSrc, vUV );
	float ws = 1.0;
	for( int i = 1; i <= uTaps; ++i )
	{
		float x = float( i ) / float( uTaps + 1 );
		float w = exp( -x * x * 4.0 );
		acc += ( texture( uSrc, vUV + uDir * float( i ) ) + texture( uSrc, vUV - uDir * float( i ) ) ) * w;
		ws += 2.0 * w;
	}
	fragColor = acc / ws;
}
)GLSL";

// final: composite + bloom + flares, premultiplied additive (alpha untouched, so glow adds over lower layers)
static const char* kPostFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uBase;
uniform sampler2D uBloom;
uniform sampler2D uGhost;
uniform sampler2D uStreak;
uniform float uBloomAmt;
uniform float uGhostAmt;
uniform float uStreakAmt;
void main()
{
	vec4 c = texture( uBase, vUV );
	vec3 g = texture( uBloom, vUV ).rgb * uBloomAmt * 0.25 + texture( uGhost, vUV ).rgb * uGhostAmt * 0.15
	       + texture( uStreak, vUV ).rgb * vec3( 0.75, 0.85, 1.0 ) * uStreakAmt * 0.6;
	fragColor = vec4( c.rgb + g, c.a );
}
)GLSL";

// Trail feedback (one pass per frame, two targets): the previous trail moves by this frame's shift (whole pixels, so
// copies stay crisp), dusty pixels drift, every pixel blurs by an amount that grows with its age, gets one frame
// older and dies at the end of the trail; then the live mask is stamped over it (Smear: several stamps spread back
// along the shift, so a fast trail has no gaps). Colour is premultiplied; age is stored x coverage, so every
// resample (shift, blur, drift, stamp) keeps it right.
static const char* kTrailFS = R"GLSL(
in vec2 vUV;
layout( location = 0 ) out vec4 outCol;
layout( location = 1 ) out vec4 outAge;
uniform sampler2D uPrevCol;
uniform sampler2D uPrevAge;
uniform sampler2D uLive;
uniform vec2 uTexel;
uniform vec2 uShift;  // uv this frame
uniform vec2 uDrift;  // uv this frame at full dustiness
uniform float uStep;  // age added per frame (1 = the whole trail)
uniform float uBlur;  // blur radius this frame at age 1, pixels
uniform float uDust;
uniform int uStamp;   // stamp the live mask this frame
uniform int uSub;     // stamps along this frame's shift
uniform int uFresh;   // no previous trail
float Dusty( float age ) { return uDust * smoothstep( 0.05, 0.8, age ); }
void main()
{
	vec4 c   = vec4( 0.0 );
	float ag = 0.0;
	if( uFresh == 0 )
	{
		vec2 uv = vUV - uShift;
		c       = texture( uPrevCol, uv );
		ag      = texture( uPrevAge, uv ).r;
		float a = c.a > 1e-4 ? ag / c.a : 0.0;
		if( uDust > 0.0 )
		{
			uv -= uDrift * Dusty( a );
			c  = texture( uPrevCol, uv );
			ag = texture( uPrevAge, uv ).r;
		}
		float r = uBlur * a;
		if( r > 0.05 )
		{
			vec2 o = uTexel * r;
			c  = 0.25 * c;
			ag = 0.25 * ag;
			for( int i = 0; i < 8; ++i )
			{
				vec2 d = vec2( i < 4 ? ( i == 0 ? 1.0 : i == 1 ? -1.0 : 0.0 ) : ( i < 6 ? 1.0 : -1.0 ), i < 4 ? ( i == 2 ? 1.0 : i == 3 ? -1.0 : 0.0 ) : ( i % 2 == 0 ? 1.0 : -1.0 ) );
				float wt = i < 4 ? 0.125 : 0.0625;
				c += wt * texture( uPrevCol, uv + d * o );
				ag += wt * texture( uPrevAge, uv + d * o ).r;
			}
		}
		ag += uStep * c.a;
		if( c.a < 1.0 / 512.0 || ag >= c.a )
			c = vec4( 0.0 ), ag = 0.0;
	}
	if( uStamp == 1 )
		for( int j = uSub - 1; j >= 0; --j ) // oldest first; j = 0 is the live mask itself
		{
			float f = float( j ) / float( uSub );
			vec4 L  = texture( uLive, vUV - uShift * f );
			ag      = f * uStep * L.a + ag * ( 1.0 - L.a );
			c       = L + c * ( 1.0 - L.a );
		}
	outCol = c;
	outAge = vec4( ag, 0.0, 0.0, 1.0 );
}
)GLSL";

// Trail resolve -> echoTex (under the live mask in the composite): fade by age (Fade Curve, Decay the master), dust
// grain (a drifting hash grid: grains drop out as they age, the rest flicker unevenly), colour mix and glow.
static const char* kEchoFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uCol;
uniform sampler2D uAge;
uniform float uDecay;
uniform float uCopies; // age 1 in copies (Delay frames each)
uniform int uCurve;    // 0 linear, 1 soft, 2 long tail
uniform float uDust;
uniform float uGrain;  // pixels
uniform vec2 uGrainPhase;
uniform vec3 uColor;
uniform float uMix;
uniform float uGlow;
float Hash2( vec2 p )
{
	vec3 q = fract( vec3( p.xyx ) * 0.1031 );
	q += dot( q, q.yzx + 33.33 );
	return fract( ( q.x + q.y ) * q.z );
}
void main()
{
	vec4 c = texture( uCol, vUV );
	if( c.a < 1e-4 )
	{
		fragColor = vec4( 0.0 );
		return;
	}
	float a = clamp( texture( uAge, vUV ).r / c.a, 0.0, 1.0 );
	float k = a * uCopies, d = max( uDecay, 1e-6 );
	float w = uCurve == 1 ? pow( d, k ) * ( 1.0 - smoothstep( 0.3, 1.0, a ) )
	        : uCurve == 2 ? pow( d, 0.5 * k ) * ( 1.0 - a ) * ( 1.0 - a )
	                      : pow( d, k ) * ( 1.0 - a );
	if( uDust > 0.0 )
	{
		float h    = Hash2( mod( floor( gl_FragCoord.xy / uGrain + uGrainPhase ), 256.0 ) );
		float dust = uDust * smoothstep( 0.05, 0.8, a );
		w *= step( dust, h ) * mix( 1.0, 0.4 + 1.2 * fract( h * 7.31 ), dust );
	}
	c *= w;
	vec3 straight = c.rgb / max( c.a, 1e-4 );
	float luma    = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
	c.rgb         = mix( c.rgb, uColor * ( 0.15 + 1.2 * luma ) * c.a, uMix ) + uColor * uGlow * 0.25 * c.a;
	fragColor     = c;
}
)GLSL";
}
