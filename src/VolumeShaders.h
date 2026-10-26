#pragma once
// GLSL 4.10 sources for the volumetric light (Stage 1): fog raymarch, temporal blend, screen-space rays,
// motes. All passes are full-screen triangles from kQuadVS except the motes (points).

namespace mehr
{
// Tonemap / encode shared by every pass that turns HDR light into the plugin's sRGB-ish output.
static const char* kToneLib = R"GLSL(
uniform float uExposure;
uniform int uTonemap; // 0 ACES, 1 filmic, 2 none
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
vec3 ToDisplay( vec3 hdr ) { return pow( Tonemap( hdr * exp2( uExposure ) ), vec3( 1.0 / 2.2 ) ); }
)GLSL";

// Front Only (eye lights): a light's medium only counts beyond the eye opening, uXLFront[ i ] along its aim.
// Negative = everywhere. Used by the medium passes and the motes, not by the surface shader.
static const char* kFrontLib = R"GLSL(
uniform float uXLFront[ MAX_XL ];
float XLFront( int i, vec3 lv ) // lv: light -> point
{
	float f = uXLFront[ i ];
	return f < 0.0 ? 1.0 : smoothstep( f - 0.03, f + 0.03, dot( lv, uXLDir[ i ] ) );
}
)GLSL";

// W24 Trip: the looks shared by the TRIP march variants (Noise and Sim medium). Compiled only with #define TRIP, so the
// plain programs are the W3 / W20 programs, unchanged. The medium calls TripFold (kaleido) and TripFractal on the lookup
// point, then TripShade, which tints the scattered light (palette, thin film) and sets gEmit, the smoke's own light
// (veins, contours, blacklight) that the march adds without any lamp.
static const char* kTripLib = R"GLSL(
uniform int uColourBy;   // 0 off, 1 density, 2 heat, 3 speed, 4 swirl, 5 age, 6 height
uniform int uPalette;    // 0 rainbow, 1 acid, 2 neon, 3 fire, 4 ice, 5 picker pair
uniform float uPalMix, uPalShift, uPalSpread;
uniform vec3 uPalA, uPalB;
uniform float uFilm, uFilmThick;
uniform float uVeins, uVeinWidth;
uniform vec3 uVeinColor;
uniform float uContours, uBands, uBandSharp, uBandScroll;
uniform float uBlack;
uniform int uBlackBy;
uniform vec3 uBlackColor;
uniform vec3 uTripCenter, uTripAxis, uTripRight, uTripUp;
uniform float uTripUnit;
uniform int uKaleido;
uniform float uKTwist, uKSpin;
uniform float uFractal, uFracScale;
uniform int uFracIter;
uniform mat3 uFracRot;
vec3 gEmit = vec3( 0.0 );
vec3 gRd   = vec3( 0.0, 0.0, -1.0 );

// four keys per palette, cycled smoothly: periodic in t with period 1 (Palette Shift wraps seamlessly)
const vec3 kPalKeys[ 16 ] = vec3[ 16 ]( vec3( 0.55, 1.0, 0.0 ), vec3( 1.0, 0.95, 0.0 ), vec3( 1.0, 0.0, 0.75 ), vec3( 0.0, 1.0, 0.85 ),  // acid
                                        vec3( 1.0, 0.1, 0.6 ), vec3( 0.55, 0.1, 1.0 ), vec3( 0.1, 0.35, 1.0 ), vec3( 0.0, 0.95, 1.0 ),  // neon
                                        vec3( 1.0, 0.12, 0.0 ), vec3( 1.0, 0.45, 0.0 ), vec3( 1.0, 0.85, 0.25 ), vec3( 1.0, 0.45, 0.0 ), // fire
                                        vec3( 0.1, 0.3, 1.0 ), vec3( 0.2, 0.75, 1.0 ), vec3( 0.85, 0.95, 1.0 ), vec3( 0.2, 0.75, 1.0 ) ); // ice
vec3 Palette( float t )
{
	t = fract( t );
	if( uPalette == 0 )
		return clamp( abs( fract( t + vec3( 0.0, 2.0 / 3.0, 1.0 / 3.0 ) ) * 6.0 - 3.0 ) - 1.0, 0.0, 1.0 );
	if( uPalette == 5 )
		return mix( uPalA, uPalB, 0.5 - 0.5 * cos( 6.2831853 * t ) );
	float x = t * 4.0;
	int i   = min( int( x ), 3 );
	float f = smoothstep( 0.0, 1.0, x - float( i ) );
	int b   = ( uPalette - 1 ) * 4;
	return mix( kPalKeys[ b + i ], kPalKeys[ b + ( ( i + 1 ) & 3 ) ], f );
}
// N-way mirror fold of the lookup round the mask's front axis: every sector reads the one above the mask (where the plume is)
vec3 TripFold( vec3 p )
{
	if( uKaleido < 2 )
		return p;
	vec3 d    = p - uTripCenter;
	float z   = dot( d, uTripAxis ), x = dot( d, uTripRight ), y = dot( d, uTripUp );
	float r   = length( vec2( x, y ) );
	float sec = 6.2831853 / float( uKaleido );
	float a   = atan( y, x ) - 1.5707963 + uKSpin + uKTwist * r;
	a         = mod( a + 0.5 * sec, sec ) - 0.5 * sec;
	a         = abs( a ) + 1.5707963;
	return uTripCenter + uTripAxis * z + uTripRight * ( r * cos( a ) ) + uTripUp * ( r * sin( a ) );
}
// a lattice of folded Menger sponges (the folds turn with Fractal Morph): the smoke is kept inside it, so geometry appears
float TripFractal( vec3 p )
{
	if( uFractal <= 0.0 )
		return 1.0;
	vec3 q  = ( p - uTripCenter ) / uFracScale;
	q       = q - 2.0 * floor( q * 0.5 + 0.5 );
	float s = 1.0;
	for( int i = 0; i < 6; ++i )
	{
		if( i >= uFracIter )
			break;
		q = abs( q );
		if( q.x < q.y )
			q.xy = q.yx;
		if( q.x < q.z )
			q.xz = q.zx;
		if( q.y < q.z )
			q.yz = q.zy;
		q = uFracRot * q;
		q = q * 3.0 - vec3( 2.0, 2.0, 0.0 );
		if( q.z > 1.0 )
			q.z -= 2.0;
		s *= 3.0;
	}
	vec3 b      = abs( q ) - 1.0;
	float de    = ( length( max( b, 0.0 ) ) + min( max( b.x, max( b.y, b.z ) ), 0.0 ) ) / s;
	float shape = 1.0 - smoothstep( 0.0, 0.03, de );
	return max( 1.0 + uFractal * ( shape * 1.8 - 1.0 ), 0.0 );
}
float TripHeight( vec3 p ) { return dot( p - uTripCenter, uTripUp ) / uTripUnit * 0.3; }
// den: the smoke density (may be re-shaped by the contour bands); field: the Colour By value; heat 0..1; grad 0..1 (veins)
void TripShade( vec3 p, inout float den, float field, float heat, float grad, inout vec3 tint )
{
	gEmit      = vec3( 0.0 );
	float pres = 1.0 - exp( -den * 3.0 );
	if( uPalMix > 0.0 )
		tint *= max( mix( vec3( 1.0 ), Palette( field * uPalSpread + uPalShift ) * 1.3, uPalMix ), vec3( 0.0 ) );
	if( uFilm > 0.0 )
	{
		// a thin film (n 1.33): a soap bubble round the mask (radius 2.5 mask radii) seen through each pixel. W25: the phase
		// is built from the point projected onto the plane across the ray, so it holds along the ray (the W24 film used the
		// sample's own radial normal and its density: ~12 colour cycles inside one dense ray, averaged to a pale grey core).
		// The density only nudges it (a soft-capped 0.15), enough for the smoke's edges to shimmer.
		vec3 q    = p - uTripCenter;
		vec3 qp   = q - dot( q, gRd ) * gRd;
		float rr  = min( length( qp ) / ( 2.5 * uTripUnit ), 1.0 );
		float ct  = sqrt( 1.0 - rr * rr );
		float cr  = sqrt( max( 1.0 - ( 1.0 - ct * ct ) / 1.77, 0.0 ) );
		float th  = uFilmThick * ( 0.5 + 0.15 * pres + 0.35 * dot( qp, uTripUp ) / uTripUnit );
		vec3 film = 0.5 + 0.5 * cos( 6.2831853 * 2.66 * th * cr / vec3( 0.65, 0.53, 0.45 ) );
		float lo  = min( film.r, min( film.g, film.b ) ), hi = max( film.r, max( film.g, film.b ) );
		film      = ( film - lo ) / ( hi - lo + 0.15 ); // only the interference colour: where all three peak it is not white
		tint *= max( mix( vec3( 1.0 ), film * 2.0, uFilm ), vec3( 0.0 ) );
	}
	if( uVeins > 0.0 )
	{
		float t0 = mix( 0.6, 0.12, uVeinWidth );
		gEmit += uVeinColor * ( uVeins * 0.35 * smoothstep( t0, t0 + 0.1, grad ) * pres );
	}
	if( uContours > 0.0 )
	{
		float y    = ( 1.0 - exp( -den * 0.8 ) ) * uBands + uBandScroll; // softer than pres: dense cores band too
		float fr   = fract( y );
		float line = ( 1.0 - smoothstep( 0.0, mix( 0.45, 0.025, uBandSharp ), min( fr, 1.0 - fr ) ) ) * smoothstep( 0.02, 0.1, pres );
		// the band's colour from its centre value: unchanged when Band Scroll's clock wraps (floor(y + 1) - 1 = floor(y))
		gEmit += Palette( ( floor( y ) - uBandScroll ) / uBands * uPalSpread + uPalShift ) * ( uContours * 0.6 * line );
		den *= mix( 1.0, 0.3 + 1.4 * line, min( uContours * 0.25, 1.0 ) );
	}
	if( uBlack > 0.0 )
		gEmit += uBlackColor * ( uBlack * 0.3 * ( uBlackBy == 1 ? heat : pres ) );
}
)GLSL";

// The participating medium: haze + drifting smoke (3D noise texture, domain-warped) + a glow cloud
// hugging each enabled volume light. Shared by the march and the screen-ray source pass.
static const char* kMediumLib = R"GLSL(
uniform sampler3D uNoise; // RGBA: R = fbm, GBA = smooth noise used as a warp vector
uniform vec3 uVolCenter;
uniform float uVolRadius;
uniform float uHaze;
uniform float uSmoke;
uniform float uSmokeFreq;
uniform float uTurb;
uniform vec3 uDrift;
uniform float uGlowCloud;
uniform vec3 uGlowPos[ 4 ];
uniform int uGlowOn[ 4 ];
float gSmokeDen  = 0.0;       // the smoke's share of the density at the last MediumAt (W23 smoke ambient / key)
vec3 gSmokeTint  = vec3( 1.0 );

float SmokeAt( vec3 p )
{
	vec3 q    = p * uSmokeFreq + uDrift;
	vec3 warp = texture( uNoise, q * 0.43 + vec3( 0.37, 0.11, 0.73 ) ).gba - 0.5;
	q += warp * uTurb * 1.2;
	float n = texture( uNoise, q ).r * 0.7 + texture( uNoise, q * 2.63 + vec3( 0.13 ) ).r * 0.3;
	return smoothstep( 0.40, 0.78, n );
}
// x = smoke + haze density (lit through the cone), y = glow-cloud density (lit all around the light)
#ifdef TRIP
vec3 gTint = vec3( 1.0 ); // the colour of the light the smoke scatters (the march multiplies the lamps by it)
vec2 MediumAt( vec3 p, out float smoke )
{
	float r    = length( p - uVolCenter );
	float edge = 1.0 - smoothstep( uVolRadius * 0.65, uVolRadius, r );
	vec3 pf    = TripFold( p );
	smoke      = SmokeAt( pf ) * TripFractal( pf );
	vec3 tint  = vec3( 1.0 );
	// the noise smoke has no heat, speed, swirl or age: Height, else its density; veins = its iso edge
	TripShade( pf, smoke, uColourBy == 6 ? TripHeight( pf ) : smoke, smoke, 4.0 * smoke * ( 1.0 - smoke ), tint );
	float dm   = ( uHaze + uSmoke * smoke ) * edge;
	gSmokeDen  = uSmoke * smoke * edge;
	gTint      = mix( vec3( 1.0 ), tint, gSmokeDen / max( dm, 1e-6 ) );
	gSmokeTint = tint;
	gEmit *= uSmoke * edge;
	float dg   = 0.0;
	for( int i = 0; i < 4; ++i )
	{
		if( uGlowOn[ i ] == 0 )
			continue;
		vec3 v = p - uGlowPos[ i ];
		dg += exp( -dot( v, v ) * 2.2 ) * ( 0.25 + 1.5 * smoke );
	}
	return vec2( dm, dg * uGlowCloud );
}
#else
vec2 MediumAt( vec3 p, out float smoke )
{
	float r    = length( p - uVolCenter );
	float edge = 1.0 - smoothstep( uVolRadius * 0.65, uVolRadius, r );
	smoke      = SmokeAt( p );
	float dm   = ( uHaze + uSmoke * smoke ) * edge;
	gSmokeDen  = uSmoke * smoke * edge;
	float dg   = 0.0;
	for( int i = 0; i < 4; ++i )
	{
		if( uGlowOn[ i ] == 0 )
			continue;
		vec3 v = p - uGlowPos[ i ];
		dg += exp( -dot( v, v ) * 2.2 ) * ( 0.25 + 1.5 * smoke );
	}
	return vec2( dm, dg * uGlowCloud );
}
#endif
)GLSL";

// Per-pixel raymarch through the bounded medium (Hillaire 2015, energy-conserving step).
// MRT: 0 = in-scattered light (rgb) + opacity (a), 1 = distance to the ray end (for the upsample).
static const char* kFogMarchFS = R"GLSL(
in vec2 vUV;
layout( location = 0 ) out vec4 outFog;
layout( location = 1 ) out vec4 outDepth;
uniform sampler2D uDepth; // scene depth, full res
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform int uSteps;
uniform float uG;
uniform float uFrame;
uniform int uVolLights; // extra lights 0..uVolLights-1 are volume lights (glow cloud, shadow maps)
uniform int uFogLights; // then the spots, up to uFogLights, scaled by uSpotBeams
uniform float uSpotBeams;
// W23: smoke lit without a volume light. Ambient = the key's colour from everywhere (isotropic, unshadowed); Key = the
// key light (directional, phase function, the mask's key shadow map where it reaches)
uniform int uSKOn;
uniform vec3 uSKAmb;
uniform vec3 uSKColor;
uniform vec3 uSKDir;    // direction the key light travels
uniform int uSKShadowOn;
uniform sampler2D uSKShadow;
uniform mat4 uSKVP;
uniform vec3 uSKPos;

float HG( float c, float g )
{
	float g2 = g * g;
	return ( 1.0 - g2 ) / pow( max( 1.0 + g2 - 2.0 * g * c, 1e-4 ), 1.5 ); // x 4pi: isotropic = 1
}
float IGN( vec2 p ) { return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) ); }
float SKLit( vec3 p )
{
	if( uSKShadowOn == 0 )
		return 1.0;
	vec4 c  = uSKVP * vec4( p, 1.0 );
	vec2 uv = c.xy / c.w * 0.5 + 0.5;
	if( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThan( uv, vec2( 1.0 ) ) ) )
		return 1.0;
	return step( dot( p - uSKPos, uSKDir ) - 0.03, texture( uSKShadow, uv ).r );
}

void main()
{
	vec4 f      = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd     = normalize( f.xyz / f.w - uCamPos );
	float z     = texture( uDepth, vUV ).r;
	float tEnd  = 1e6;
	if( z < 1.0 )
	{
		vec4 s = uInvViewProj * vec4( vUV * 2.0 - 1.0, z * 2.0 - 1.0, 1.0 );
		tEnd   = length( s.xyz / s.w - uCamPos );
	}
	// ray / volume sphere
	vec3 oc  = uCamPos - uVolCenter;
	float b  = dot( oc, rd );
	float c  = dot( oc, oc ) - uVolRadius * uVolRadius;
	float h  = b * b - c;
	outDepth = vec4( min( tEnd, 1e4 ) );
	if( h <= 0.0 )
	{
		outFog = vec4( 0.0 );
		return;
	}
	h        = sqrt( h );
	float t0 = max( -b - h, 0.0 );
	float t1 = min( -b + h, tEnd );
	if( t1 <= t0 )
	{
		outFog = vec4( 0.0 );
		return;
	}
	float dt    = ( t1 - t0 ) / float( uSteps );
	float t     = t0 + dt * IGN( gl_FragCoord.xy + uFrame * vec2( 5.588238, 3.1459 ) );
	vec3 L      = vec3( 0.0 );
	float T     = 1.0;
#ifdef TRIP
	gRd = rd;
#endif
	for( int s = 0; s < uSteps; ++s )
	{
		vec3 p = uCamPos + rd * t;
		float smoke;
		vec2 dens = MediumAt( p, smoke );
		float sig = dens.x + dens.y;
		if( sig > 1e-4 )
		{
			vec3 S = vec3( 0.0 );
			for( int i = 0; i < uFogLights; ++i )
			{
				vec3 lv  = p - uXLPos[ i ];
				float d  = length( lv );
				vec3 l   = lv / max( d, 1e-5 );
				float cn = XLCone( i, l );
				// the glow cloud is lit all around its light, the smoke only inside the cone
				bool vol = i < uVolLights;
				float k  = vol ? ( dens.x * cn + dens.y * max( cn, 0.6 ) ) * XLFront( i, lv ) : dens.x * cn * uSpotBeams;
				if( k <= 0.0 )
					continue;
				S += uXLColor[ i ] * ( k * XLWindow( d ) * XShadow( i, p, d ) * HG( dot( l, -rd ), uG ) );
			}
			if( uSKOn == 1 && gSmokeDen > 0.0 )
				S += gSmokeTint * gSmokeDen * ( uSKAmb + uSKColor * ( SKLit( p ) * HG( dot( uSKDir, -rd ), uG ) ) );
			float ext = exp( -sig * dt );
			L += T * S * ( 1.0 - ext ) / sig; // S already carries sigma: (S - S*ext)/sigma_t per unit sigma
#ifdef TRIP
			L += T * gEmit * ( 1.0 - ext ) / sig; // Trip: the smoke's own light (emission per unit length)
#endif
			T *= ext;
			if( T < 0.005 )
				break;
		}
		t += dt;
	}
	outFog = vec4( L, 1.0 - T );
}
)GLSL";

// Temporal blend with reprojection of the ray end and a 3x3 neighbourhood clamp.
static const char* kFogTemporalFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uCur;
uniform sampler2D uCurDepth;
uniform sampler2D uHist;
uniform int uHistValid;
uniform mat4 uInvViewProj;
uniform mat4 uPrevViewProj;
uniform vec3 uCamPos;
uniform vec2 uTexel;
uniform float uBlend;
void main()
{
	vec4 cur = texture( uCur, vUV );
	if( uHistValid == 0 )
	{
		fragColor = cur;
		return;
	}
	vec4 lo = cur, hi = cur;
	for( int y = -1; y <= 1; ++y )
		for( int x = -1; x <= 1; ++x )
		{
			vec4 n = texture( uCur, vUV + vec2( x, y ) * uTexel );
			lo     = min( lo, n );
			hi     = max( hi, n );
		}
	float t     = min( texture( uCurDepth, vUV ).r, 50.0 );
	vec4 f      = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd     = normalize( f.xyz / f.w - uCamPos );
	vec4 pc     = uPrevViewProj * vec4( uCamPos + rd * t, 1.0 );
	vec2 puv    = pc.w > 1e-5 ? pc.xy / pc.w * 0.5 + 0.5 : vec2( -1.0 );
	if( any( lessThan( puv, vec2( 0.0 ) ) ) || any( greaterThan( puv, vec2( 1.0 ) ) ) )
	{
		fragColor = cur;
		return;
	}
	vec4 hist = clamp( texture( uHist, puv ), lo, hi );
	fragColor = mix( hist, cur, uBlend );
}
)GLSL";

// W24 Trip post on the fog (fog resolution, after the temporal blend): Prism = the fog sampled in 7 taps along a direction
// (out from the mask's screen centre, turned by Prism Angle) with spectral weights, so smoke edges fringe into rainbows;
// Hue Echo = max( this frame, last output hue-turned and faded ): rainbow afterimages. rgb = additive light, a = coverage
// (the alpha contract: both stay premultiplied; max and the weighted mean keep that).
static const char* kTripPostFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uFogIn;
uniform sampler2D uHist;
uniform int uHistOk;
uniform float uPrism;
uniform float uPrismAngle;
uniform vec2 uCenterUV;
uniform float uAspect;
uniform float uEcho;    // this frame's persistence
uniform float uEchoHue; // turns this frame
uniform float uLines, uLineW, uLineGlow; // W25 contour lines: amount, core width (fog pixels), halo
vec3 RgbToHsv( vec3 c )
{
	vec4 K  = vec4( 0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0 );
	vec4 p  = mix( vec4( c.bg, K.wz ), vec4( c.gb, K.xy ), step( c.b, c.g ) );
	vec4 q  = mix( vec4( p.xyw, c.r ), vec4( c.r, p.yzx ), step( p.x, c.r ) );
	float d = q.x - min( q.w, q.y );
	return vec3( abs( q.z + ( q.w - q.y ) / ( 6.0 * d + 1e-10 ) ), d / ( q.x + 1e-10 ), q.x );
}
vec3 HsvToRgb( vec3 c ) { return c.z * mix( vec3( 1.0 ), clamp( abs( fract( c.xxx + vec3( 1.0, 2.0 / 3.0, 1.0 / 3.0 ) ) * 6.0 - 3.0 ) - 1.0, 0.0, 1.0 ), c.y ); }
void main()
{
	vec4 c = texture( uFogIn, vUV );
	if( uPrism > 0.0 )
	{
		vec2 d    = ( vUV - uCenterUV ) * vec2( uAspect, 1.0 );
		float r   = length( d );
		vec2 dir  = r > 1e-4 ? d / r : vec2( 1.0, 0.0 );
		float ca  = cos( uPrismAngle ), sa = sin( uPrismAngle );
		dir       = vec2( ca * dir.x - sa * dir.y, sa * dir.x + ca * dir.y );
		vec2 off  = dir / vec2( uAspect, 1.0 ) * uPrism * ( 0.006 + 0.05 * r );
		vec3 acc  = vec3( 0.0 ), ws = vec3( 0.0 );
		float a   = 0.0;
		for( int i = 0; i < 7; ++i )
		{
			float t = float( i ) / 6.0;
			vec3 w  = max( vec3( 1.0 ) - abs( vec3( t ) - vec3( 0.0, 0.5, 1.0 ) ) * 2.0, 0.0 );
			vec4 s  = texture( uFogIn, vUV + off * ( t - 0.5 ) * 2.0 );
			acc += s.rgb * w;
			ws += w;
			a += s.a;
		}
		c = vec4( acc / ws, a / 7.0 );
	}
	if( uLines > 0.0 )
	{
		// W25 contour lines: iso-lines of the optical depth tau = -ln(1 - coverage) (lightly blurred: the march is jittered),
		// anti-aliased by the field's screen-space derivative, so they stay one Line Width wide wherever the smoke thins or
		// thickens. Additive light (the alpha contract): the coverage is unchanged.
		vec2 px = 1.5 / vec2( textureSize( uFogIn, 0 ) );
		float a = 0.0;
		for( int j = -1; j <= 1; ++j )
			for( int i = -1; i <= 1; ++i )
				a += texture( uFogIn, vUV + vec2( i, j ) * px ).a * ( ( i == 0 ? 2.0 : 1.0 ) * ( j == 0 ? 2.0 : 1.0 ) / 16.0 );
		float y = -log( max( 1.0 - a, 1e-3 ) ) * uBands * 0.6 + uBandScroll;
		float fw = max( fwidth( y ), 1e-4 );
		float d  = abs( fract( y + 0.5 ) - 0.5 ) / fw; // pixels to the nearest level
		float ln = 1.0 - smoothstep( 0.5 * uLineW - 0.5, 0.5 * uLineW + 0.5, d );
		ln += uLineGlow * 0.3 * exp( -d / ( 1.0 + 2.0 * uLineW ) );
		ln *= smoothstep( 0.01, 0.05, a ) * ( 1.0 - smoothstep( 0.3, 0.6, fw ) ); // no lines in empty air or in pixel noise
		// the level's colour: unchanged when Band Scroll's clock wraps (floor(y + 1.5) - 1 = floor(y + 0.5))
		c.rgb += Palette( ( floor( y + 0.5 ) - uBandScroll ) / uBands * uPalSpread + uPalShift ) * ( uLines * 0.3 * ln );
	}
	if( uEcho > 0.0 && uHistOk == 1 )
	{
		vec4 h   = texture( uHist, vUV );
		vec3 hsv = RgbToHsv( max( h.rgb, vec3( 0.0 ) ) );
		// the echo turns its hue and gains saturation, so even white smoke leaves a rainbow
		h.rgb = HsvToRgb( vec3( fract( hsv.x + uEchoHue ), mix( hsv.y, 1.0, 0.12 ), hsv.z ) );
		c     = max( c, h * uEcho );
	}
	fragColor = c;
}
)GLSL";

// Screen Rays (Mitchell, GPU Gems 3 ch.13), pass 1: the occlusion source. Each light is a smoky sprite at
// its screen position, hidden wherever the scene is nearer than the light.
static const char* kRaySourceFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uDepth;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform float uAspect;
uniform int uVolLights;
uniform vec3 uLightUV[ 4 ];   // xy = screen uv, z = 1 when in front of the camera
uniform float uFacing[ 4 ];   // spot aimed at the camera = 1, away = the cone's spill
uniform float uSpriteSize[ 4 ];
void main()
{
	float z    = texture( uDepth, vUV ).r;
	float tEnd = 1e6;
	if( z < 1.0 )
	{
		vec4 s = uInvViewProj * vec4( vUV * 2.0 - 1.0, z * 2.0 - 1.0, 1.0 );
		tEnd   = length( s.xyz / s.w - uCamPos );
	}
	vec4 f  = uInvViewProj * vec4( vUV * 2.0 - 1.0, 1.0, 1.0 );
	vec3 rd = normalize( f.xyz / f.w - uCamPos );
	vec3 c  = vec3( 0.0 );
	for( int i = 0; i < uVolLights; ++i )
	{
		if( uLightUV[ i ].z < 0.5 )
			continue;
		float dl = length( uXLPos[ i ] - uCamPos );
		if( tEnd < dl )
			continue; // the mask is in front of the light here
		vec2 d     = ( vUV - uLightUV[ i ].xy ) * vec2( uAspect, 1.0 );
		float r    = length( d ) / max( uSpriteSize[ i ], 1e-4 );
		float core = exp( -r * r * 3.0 );
		float halo = uXLFront[ i ] < 0.0 ? exp( -r * 1.5 ) * uGlowCloud * 0.3 : 0.0; // Front Only: no cloud behind
		float smoke;
		vec2 m     = MediumAt( uCamPos + rd * dl, smoke );
		float wisp = 0.35 + 1.3 * smoke * min( uSmoke, 2.0 ) + uHaze;
		c += uXLColor[ i ] * uFacing[ i ] * ( core + halo * wisp ) * 0.2;
	}
	fragColor = vec4( c, 0.0 );
}
)GLSL";

// Screen Rays pass 2: radial blur toward each light.
static const char* kRayBlurFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSource;
uniform int uVolLights;
uniform vec3 uLightUV[ 4 ];
uniform int uSamples;
uniform float uDensity;
uniform float uDecay;
uniform float uWeight;
uniform float uFrame;
float IGN( vec2 p ) { return fract( 52.9829189 * fract( dot( p, vec2( 0.06711056, 0.00583715 ) ) ) ); }
void main()
{
	vec3 acc = vec3( 0.0 );
	float j  = IGN( gl_FragCoord.xy + uFrame * vec2( 5.588238, 3.1459 ) );
	for( int i = 0; i < uVolLights; ++i )
	{
		if( uLightUV[ i ].z < 0.5 )
			continue;
		vec2 delta  = ( vUV - uLightUV[ i ].xy ) * ( uDensity / float( uSamples ) );
		vec2 uv     = vUV - delta * j;
		float decay = 1.0;
		vec3 sum    = vec3( 0.0 );
		for( int s = 0; s < uSamples; ++s )
		{
			sum += texture( uSource, uv ).rgb * decay;
			decay *= uDecay;
			uv -= delta;
		}
		acc += sum * uWeight / float( uSamples ) * 5.0;
	}
	acc += texture( uSource, vUV ).rgb;
	fragColor = vec4( acc, 0.0 );
}
)GLSL";

// Motes: dust points inside the volume, lit only where a light's cone and shadow reach them.
static const char* kMotesVS = R"GLSL(
uniform mat4 uViewProj;
uniform vec3 uCamPos;
uniform vec3 uVolCenter;
uniform float uVolRadius;
uniform vec3 uMoteDrift; // the smoke drift as a fraction of its loop (0..1)
uniform float uTime;     // seconds, wrapped at kLoop
uniform float uPointScale;
uniform int uVolLights;
uniform float uG;
out vec3 vColor;
// every per-mote rate is rounded to a whole number of cycles per loop, so the clock's wrap is seamless
const float kLoop = 3600.0; // = kMoteLoop in LoadEhrPlugin.cpp
float Cycles( float t, float perSec ) { return fract( t * ( floor( perSec * kLoop + 0.5 ) / kLoop ) ); }
vec3 H3( float n ) { return fract( sin( vec3( n, n + 1.31, n + 2.73 ) * vec3( 12.9898, 78.233, 37.719 ) ) * 43758.5453 ); }
float HG( float c, float g )
{
	float g2 = g * g;
	return ( 1.0 - g2 ) / pow( max( 1.0 + g2 - 2.0 * g * c, 1e-4 ), 1.5 );
}
void main()
{
	float id = float( gl_VertexID );
	vec3 h   = H3( id * 0.618 );
	vec3 h2  = H3( id * 0.618 + 17.3 );
	// spawn inside a light's cone (point light: a ball around it), drifting slowly away from the light
	int li   = uVolLights > 0 ? gl_VertexID % uVolLights : 0;
	vec3 lp  = uXLPos[ li ];
	vec3 cw  = ( 0.3 + h2 ) / 6.2831853; // cycles per second
	vec3 wob = 0.04 * sin( 6.2831853 * vec3( Cycles( uTime, cw.x ), Cycles( uTime, cw.y ), Cycles( uTime, cw.z ) ) + h * 6.28 );
	vec3 p;
	float along = fract( h.x + Cycles( uTime, 0.01 + 0.02 * h2.x ) ) * uXLRange;
	if( uXLCone[ li ].x <= -1.0 )
		p = lp + normalize( h2 * 2.0 - 1.0 + 1e-3 ) * along * 0.6;
	else
	{
		vec3 d      = uXLDir[ li ];
		vec3 t      = normalize( abs( d.y ) > 0.9 ? cross( d, vec3( 1, 0, 0 ) ) : cross( d, vec3( 0, 1, 0 ) ) );
		vec3 b      = cross( d, t );
		float c     = max( uXLCone[ li ].x, 0.02 );
		float tanH  = sqrt( max( 1.0 - c * c, 0.0 ) ) / c;
		float r     = tanH * along * sqrt( h.y );
		float a     = h.z * 6.2831853;
		p           = lp + d * along + ( t * cos( a ) + b * sin( a ) ) * r;
	}
	// drift with the smoke: each mote recycles across its own 0.04-unit loop (four per drift loop, so the drift's
	// wrap stays seamless), fading out at the seam
	vec3 dl  = fract( uMoteDrift * 4.0 + h2 );
	vec3 de  = min( dl, 1.0 - dl );
	float df = smoothstep( 0.0, 0.01, min( de.x, min( de.y, de.z ) ) );
	p += wob + ( dl - 0.5 ) * 0.04;
	vec3 rd  = normalize( p - uCamPos );
	vec3 c   = vec3( 0.0 );
	for( int i = 0; i < uVolLights; ++i )
	{
		vec3 lv = p - uXLPos[ i ];
		float d = length( lv );
		vec3 l  = lv / max( d, 1e-5 );
		float k = XLCone( i, l ) * XLWindow( d ) * XLFront( i, lv );
		if( k > 0.0 )
			c += uXLColor[ i ] * ( k * XShadow( i, p, d ) * HG( dot( l, -rd ), uG ) );
	}
	float tw     = 0.35 + 0.65 * pow( 0.5 + 0.5 * sin( 6.2831853 * Cycles( uTime, ( 2.0 + 5.0 * h.x ) / 6.2831853 ) + h.y * 40.0 ), 4.0 );
	vColor       = c * tw * df * 1.5;
	vec4 clip    = uViewProj * vec4( p, 1.0 );
	float lit    = max( max( c.r, c.g ), c.b );
	gl_Position  = lit > 0.002 ? clip : vec4( 2.0, 2.0, 2.0, 1.0 );
	gl_PointSize = uPointScale * ( 1.0 + 2.0 * h.z ) / max( clip.w, 0.3 );
}
)GLSL";

static const char* kMotesFS = R"GLSL(
in vec3 vColor;
out vec4 fragColor;
uniform sampler2D uDepth;
uniform vec2 uScreen;
void main()
{
	vec2 q      = gl_PointCoord * 2.0 - 1.0;
	float fall  = max( 1.0 - dot( q, q ), 0.0 );
	float scene = texture( uDepth, gl_FragCoord.xy / uScreen ).r;
	fragColor   = vec4( ToDisplay( vColor * fall * fall ), 0.0 );
	if( gl_FragCoord.z > scene || fall <= 0.0 )
		discard;
}
)GLSL";
}
