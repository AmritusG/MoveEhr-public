#pragma once
// W20: GLSL 4.10 for the 3D smoke simulation (Crane / Llamas / Tariq, GPU Gems 3 ch.30; Fedkiw / Stam / Jensen 2001).
// GL 4.1 has no compute and no image stores, so every field is a 2D ATLAS of z-slices (tile z at column z % uTilesX, row
// z / uTilesX) and every pass is ONE full-screen triangle over the atlas: a fragment = a cell. Continuous reads interpolate
// bilinearly inside a tile (clamped half a texel from its edge) and linearly between the two nearest tiles.
// Units: world units and seconds; uCell = world size of a cell; grid coords g: cell centres at c + 0.5.

namespace mehr
{
static const char* kSimLib = R"GLSL(
uniform int uRes;
uniform int uTilesX;
uniform vec2 uAtlas;
uniform vec3 uBoxMin; // world position of grid corner (0,0,0)
uniform float uCell;
bool CellOf( vec2 frag, out ivec3 c )
{
	ivec2 p = ivec2( frag );
	ivec2 t = p / uRes;
	c       = ivec3( p - t * uRes, t.y * uTilesX + t.x );
	return c.z < uRes;
}
vec2 AtlasUV( vec2 xy, int z )
{
	vec2 tile = vec2( z % uTilesX, z / uTilesX );
	vec2 q    = clamp( xy, vec2( 0.5 ), vec2( float( uRes ) - 0.5 ) );
	return ( tile * float( uRes ) + q ) / uAtlas;
}
vec4 SampleG( sampler2D t, vec3 g )
{
	float z = clamp( g.z - 0.5, 0.0, float( uRes ) - 1.0 );
	int z0  = int( floor( z ) );
	int z1  = min( z0 + 1, uRes - 1 );
	return mix( texture( t, AtlasUV( g.xy, z0 ) ), texture( t, AtlasUV( g.xy, z1 ) ), z - float( z0 ) );
}
vec4 FetchC( sampler2D t, ivec3 c )
{
	c          = clamp( c, ivec3( 0 ), ivec3( uRes - 1 ) );
	ivec2 tile = ivec2( c.z % uTilesX, c.z / uTilesX );
	return texelFetch( t, tile * uRes + c.xy, 0 );
}
bool InGrid( vec3 g ) { return all( greaterThanEqual( g, vec3( 0.0 ) ) ) && all( lessThanEqual( g, vec3( float( uRes ) ) ) ); }
bool OnGrid( ivec3 c ) { return all( greaterThanEqual( c, ivec3( 0 ) ) ) && all( lessThan( c, ivec3( uRes ) ) ); }
vec3 WorldOf( vec3 g ) { return uBoxMin + g * uCell; }
)GLSL";

// Pass 1: the obstacle. Each cell's world point goes back into the model's raw space (inverse of this frame's model
// matrix: spin, wobble, bounce, pose, scale, fit), samples the SDF; obstacle velocity = where that material point is now
// minus where it was a frame ago (rigid motion: a spinning mask stirs the smoke). rgb = velocity, a = signed distance.
static const char* kSimObstacleFS = R"GLSL(
out vec4 outObs;
uniform sampler3D uSdf;
uniform vec3 uSdfMin;
uniform float uSdfSize;
uniform float uSdfToWorld;
uniform mat4 uInvModel;
uniform mat4 uPrevModel;
uniform float uInvDt;
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outObs = vec4( 0.0, 0.0, 0.0, 1e3 );
		return;
	}
	vec3 w  = WorldOf( vec3( c ) + 0.5 );
	vec3 q  = ( uInvModel * vec4( w, 1.0 ) ).xyz;
	vec3 u  = ( q - uSdfMin ) / uSdfSize;
	float d = any( lessThan( u, vec3( 0.0 ) ) ) || any( greaterThan( u, vec3( 1.0 ) ) ) ? 1e3 : texture( uSdf, u ).r * uSdfToWorld;
	vec3 prev = ( uPrevModel * vec4( q, 1.0 ) ).xyz;
	outObs    = vec4( ( w - prev ) * uInvDt, d );
}
)GLSL";

// Pass 2: MacCormack, step 1 = semi-Lagrangian forward. The grid follows the mask: the previous frame's grid sits at
// g + uShift (cells), so the back-trace starts there. Density coming in from outside the grid is clean air.
static const char* kSimAdvectFS = R"GLSL(
layout( location = 0 ) out vec4 outV;
layout( location = 1 ) out vec4 outS;
uniform sampler2D uVel;
uniform sampler2D uSca;
uniform float uDtCells;
uniform vec3 uShift;
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outV = outS = vec4( 0.0 );
		return;
	}
	vec3 g    = vec3( c ) + 0.5 + uShift;
	vec3 back = g - SampleG( uVel, g ).xyz * uDtCells;
	outV      = SampleG( uVel, back );
	outS      = InGrid( back ) ? SampleG( uSca, back ) : vec4( 0.0 );
}
)GLSL";

// Pass 3: MacCormack, step 2 = the error correction, clamped to the 8 cells around the back-traced point (keeps it
// stable), then fades, and solids: velocity = the obstacle's, no smoke inside the mask. W24 (TRIP variant): b = the age
// mass (density x age) is advected and faded with the density, so age = b / density.
static const char* kSimCorrectFS = R"GLSL(
layout( location = 0 ) out vec4 outV;
layout( location = 1 ) out vec4 outS;
uniform sampler2D uVel;
uniform sampler2D uSca;
uniform sampler2D uTmpV;
uniform sampler2D uTmpS;
uniform sampler2D uObs;
uniform float uDtCells;
uniform vec3 uShift;
uniform float uSolidPad;
uniform vec3 uFade; // per-step multipliers: velocity, density, heat
#ifdef FIRE
uniform float uFuelFade;
#endif
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outV = outS = vec4( 0.0 );
		return;
	}
	vec4 ob = FetchC( uObs, c );
	if( ob.w < uSolidPad )
	{
		outV = vec4( ob.xyz, 0.0 );
		outS = vec4( 0.0 );
		return;
	}
	vec3 gc   = vec3( c ) + 0.5;
	vec3 g    = gc + uShift;
	vec3 u    = SampleG( uVel, g ).xyz;
	vec3 back = g - u * uDtCells;
	vec4 v1 = FetchC( uTmpV, c ), s1 = FetchC( uTmpS, c );
	vec3 fwd = gc + u * uDtCells;
	vec4 V   = v1 + 0.5 * ( SampleG( uVel, g ) - SampleG( uTmpV, fwd ) );
	vec4 S   = s1 + 0.5 * ( SampleG( uSca, g ) - SampleG( uTmpS, fwd ) );
	ivec3 i0  = ivec3( floor( back - 0.5 ) );
	vec4 mnV = vec4( 1e9 ), mxV = vec4( -1e9 ), mnS = vec4( 1e9 ), mxS = vec4( -1e9 );
	for( int k = 0; k < 8; ++k )
	{
		ivec3 o = i0 + ivec3( k & 1, ( k >> 1 ) & 1, k >> 2 );
		vec4 a = FetchC( uVel, o ), b = FetchC( uSca, o );
		mnV = min( mnV, a ), mxV = max( mxV, a ), mnS = min( mnS, b ), mxS = max( mxS, b );
	}
	V = clamp( V, mnV, mxV );
	S = InGrid( back ) ? clamp( S, mnS, mxS ) : vec4( 0.0 );
	outV = vec4( V.xyz * uFade.x, 0.0 );
#if defined( FIRE )
	outS = vec4( max( S.r, 0.0 ) * uFade.y, max( S.g, 0.0 ) * uFade.z, max( S.b, 0.0 ) * uFuelFade, 0.0 ); // W26: b = fuel
#elif defined( TRIP )
	outS = vec4( max( S.r, 0.0 ) * uFade.y, max( S.g, 0.0 ) * uFade.z, max( S.b, 0.0 ) * uFade.y, 0.0 );
#else
	outS = vec4( max( S.r, 0.0 ) * uFade.y, max( S.g, 0.0 ) * uFade.z, 0.0, 0.0 );
#endif
}
)GLSL";

// Pass 4a: vorticity (curl of the velocity), central differences.
static const char* kSimCurlFS = R"GLSL(
out vec4 outW;
uniform sampler2D uVel;
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outW = vec4( 0.0 );
		return;
	}
	vec3 L = FetchC( uVel, c - ivec3( 1, 0, 0 ) ).xyz, R = FetchC( uVel, c + ivec3( 1, 0, 0 ) ).xyz;
	vec3 D = FetchC( uVel, c - ivec3( 0, 1, 0 ) ).xyz, U = FetchC( uVel, c + ivec3( 0, 1, 0 ) ).xyz;
	vec3 B = FetchC( uVel, c - ivec3( 0, 0, 1 ) ).xyz, F = FetchC( uVel, c + ivec3( 0, 0, 1 ) ).xyz;
	outW = vec4( ( U.z - D.z ) - ( F.y - B.y ), ( F.x - B.x ) - ( R.z - L.z ), ( R.y - L.y ) - ( U.x - D.x ), 0.0 ) * ( 0.5 / uCell );
}
)GLSL";

// Pass 4b: forces and sources. Buoyancy (heat up, density down), vorticity confinement, wind, turbulence from the
// smoke noise, the emitters (eyes, behind, rim, inside), the confinement wall. W24 Trip (the TRIP variant, a separate
// program: even unused code here changed the eye jets' rounding, W1 rule): whirlpool / hyperspace forces, vortex rings,
// ghost faces (density in the SDF shell of a moved, scaled copy of the mask). Outputs for the Trip looks: velocity
// a = |curl| (Swirl), smoke b = age mass (every bit of smoke ages; sources add age 0), a = |grad density| per cell.
static const char* kSimForceFS = R"GLSL(
layout( location = 0 ) out vec4 outV;
layout( location = 1 ) out vec4 outS;
uniform sampler2D uVel;
uniform sampler2D uSca;
uniform sampler2D uCurl;
uniform sampler2D uObs;
uniform sampler3D uSdf;
uniform sampler3D uNoise;
uniform vec3 uSdfMin;
uniform float uSdfSize;
uniform mat4 uInvModel;
uniform float uDt;
uniform float uSolidPad;
uniform float uBuoy;
uniform float uWeight;
uniform float uVort;
uniform vec3 uWind;
uniform float uTurb;
uniform float uNoiseFreq;
uniform vec3 uDrift;
uniform float uEmitMul;
uniform float uEmitEyes, uEyeHeat, uEyeRad, uEyeSpeed;
uniform vec3 uEyePos[ 2 ];
uniform vec3 uEyeDir;
uniform float uEmitBehind, uBehindHeat, uBehindRad;
uniform vec3 uBehindPos;
uniform float uEmitRim, uRimHeat;
uniform float uEmitOff; // the rim band sits this far (world) out of the surface
uniform float uEmitInside, uInsideHeat;
uniform vec3 uCamPos;
uniform float uConfine, uConfineR;
uniform vec3 uConfineCenter;
#ifdef TRIP
uniform vec3 uMaskCenter, uMaskUp;
uniform float uWhirl, uHyper;
uniform vec3 uHyperAxis;
// W25: up to 2 rings alive at once, each travelling from where it fired (world), driven for its whole life
uniform float uRingSpeed, uRingSpin, uRingSmoke, uRingHold;
uniform int uRingN;
uniform float uRingK[ 2 ], uRingS[ 2 ], uRingR[ 2 ], uRingH[ 2 ];
uniform vec3 uRingPos[ 2 ];
uniform vec3 uRingDir[ 2 ];
uniform int uGhostN;
uniform mat4 uGhostInv[ 3 ];
uniform float uGhostK[ 3 ], uGhostToWorld[ 3 ];
uniform float uGhostSmoke;
#endif
#ifdef FIRE
// W26 fire (Nguyen, Fedkiw, Jensen 2002, simplified to the grid we have): b = gaseous fuel. The emitters inject fuel (and their
// heat, which ignites it); above the ignition heat it burns at uBurn per second, releasing heat (the products) and soot
// (smoke); the hot gas cools radiatively (~ T^4) and rises (uLift on top of Buoyancy)
uniform float uFireFuel, uFireSmoke, uBurn, uHeatRelease, uSoot, uFireCool, uLift;
#endif
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outV = outS = vec4( 0.0 );
		return;
	}
	vec4 ob = FetchC( uObs, c );
	if( ob.w < uSolidPad )
	{
		outV = vec4( ob.xyz, 0.0 );
		outS = vec4( 0.0 );
		return;
	}
	vec3 v = FetchC( uVel, c ).xyz;
	vec4 s = FetchC( uSca, c );
	vec3 p = WorldOf( vec3( c ) + 0.5 );
	vec3 f = vec3( 0.0, uBuoy * s.g - uWeight * s.r, 0.0 );
	// vorticity confinement: f = eps h (N x w), N = normalised gradient of |w|
	vec3 w   = FetchC( uCurl, c ).xyz;
	vec3 eta = vec3( length( FetchC( uCurl, c + ivec3( 1, 0, 0 ) ).xyz ) - length( FetchC( uCurl, c - ivec3( 1, 0, 0 ) ).xyz ),
	                 length( FetchC( uCurl, c + ivec3( 0, 1, 0 ) ).xyz ) - length( FetchC( uCurl, c - ivec3( 0, 1, 0 ) ).xyz ),
	                 length( FetchC( uCurl, c + ivec3( 0, 0, 1 ) ).xyz ) - length( FetchC( uCurl, c - ivec3( 0, 0, 1 ) ).xyz ) );
#ifdef TRIP
	// W25: a ring's core is calm: no confinement / turbulence there (they shredded the core into a band), and it keeps its smoke
	float calm = 0.0, hold = 0.0;
	for( int i = 0; i < 2; ++i )
	{
		if( i >= uRingN )
			break;
		vec3 d    = p - uRingPos[ i ];
		float a   = dot( d, uRingDir[ i ] );
		float rho = length( vec2( length( d - a * uRingDir[ i ] ) - uRingR[ i ], a ) );
		float sg  = max( 0.25 * uRingR[ i ], uCell );
		calm      = max( calm, exp( -rho * rho / ( 4.0 * sg * sg ) ) * min( 1.0, 4.0 * uRingK[ i ] ) );
		hold += exp( -rho * rho / ( 9.0 * sg * sg ) ) * uRingH[ i ];
	}
#ifdef FIRE
	f += ( 1.0 - calm ) * 0.5 * uVort * uCell * cross( eta / ( length( eta ) + 1e-5 ), w ); // W26: hot gas feeds the swirls: half the confinement (Vorticity 4+ blew fire apart)
#else
	f += ( 1.0 - calm ) * uVort * uCell * cross( eta / ( length( eta ) + 1e-5 ), w );
#endif
	f += ( uWind - v ) * 0.4;
	f += ( 1.0 - calm ) * ( texture( uNoise, p * uNoiseFreq + uDrift ).gba - 0.5 ) * uTurb * 4.0;
#else
	f += uVort * uCell * cross( eta / ( length( eta ) + 1e-5 ), w );
	f += ( uWind - v ) * 0.4;
	f += ( texture( uNoise, p * uNoiseFreq + uDrift ).gba - 0.5 ) * uTurb * 4.0;
#endif
#ifdef TRIP
	if( uWhirl != 0.0 || uHyper != 0.0 )
	{
		// whirlpool: round the mask's up axis, drawn a little inward; hyperspace: out from the camera axis and toward the camera
		vec3 d  = p - uMaskCenter;
		vec3 rw = d - uMaskUp * dot( d, uMaskUp );
		float r = length( rw );
		vec3 er = rw / max( r, 1e-4 );
		f += uWhirl * ( cross( uMaskUp, er ) - 0.25 * sign( uWhirl ) * er ) * smoothstep( 0.0, 3.0 * uCell, r );
		vec3 rh  = d - uHyperAxis * dot( d, uHyperAxis );
		float rr = length( rh );
		f += uHyper * ( rh / max( rr, 1e-4 ) * smoothstep( 0.0, 3.0 * uCell, rr ) - 0.5 * uHyperAxis );
	}
#endif
	v += f * uDt;
	// sources: density / s and heat / s (x uEmitMul: beat puff and audio)
	float e = 0.0, h = 0.0;
	for( int i = 0; i < 2; ++i )
	{
		vec3 d  = p - uEyePos[ i ];
		float k = exp( -dot( d, d ) / max( uEyeRad * uEyeRad, 1e-6 ) );
		e += uEmitEyes * k, h += uEyeHeat * uEmitEyes * k;
		v = mix( v, uEyeDir * uEyeSpeed, min( 1.0, k * uEmitEyes * uDt * 8.0 ) );
	}
	{
		vec3 d  = p - uBehindPos;
		float k = exp( -dot( d, d ) / max( uBehindRad * uBehindRad, 1e-6 ) );
		e += uEmitBehind * k, h += uBehindHeat * uEmitBehind * k;
	}
	if( uEmitRim > 0.0 && ob.w < uEmitOff + 3.0 * uCell )
	{
		vec3 n  = vec3( FetchC( uObs, c + ivec3( 1, 0, 0 ) ).w - FetchC( uObs, c - ivec3( 1, 0, 0 ) ).w,
		                FetchC( uObs, c + ivec3( 0, 1, 0 ) ).w - FetchC( uObs, c - ivec3( 0, 1, 0 ) ).w,
		                FetchC( uObs, c + ivec3( 0, 0, 1 ) ).w - FetchC( uObs, c - ivec3( 0, 0, 1 ) ).w );
		n       = n / ( length( n ) + 1e-6 );
		float k = ( 1.0 - smoothstep( 0.0, 0.4, abs( dot( n, normalize( uCamPos - p ) ) ) ) ) * ( 1.0 - smoothstep( 1.0, 3.0, abs( ob.w - uEmitOff ) / uCell ) );
		e += uEmitRim * k, h += uRimHeat * uEmitRim * k;
	}
	if( uEmitInside > 0.0 )
	{
		vec3 u  = ( ( uInvModel * vec4( p, 1.0 ) ).xyz - uSdfMin ) / uSdfSize;
		float k = any( lessThan( u, vec3( 0.0 ) ) ) || any( greaterThan( u, vec3( 1.0 ) ) ) ? 0.0 : texture( uSdf, u ).g;
		e += uEmitInside * k, h += uInsideHeat * uEmitInside * k;
	}
#if defined( FIRE )
	s.r += e * uEmitMul * uDt * uFireSmoke; // fire mode: the emitters give fuel (Fire 1 = all fuel, no raw smoke)
	s.b += e * uEmitMul * uDt * uFireFuel;
	s.g += h * uEmitMul * uDt;
	{
		float burn = s.b * ( 1.0 - exp( -uBurn * smoothstep( 0.2, 0.8, s.g ) * uDt ) );
		s.b -= burn;
		s.g += burn * uHeatRelease;
		s.r += burn * uSoot;
		s.g = max( s.g - uFireCool * uDt * s.g * ( 1.0 + 0.05 * s.g * s.g ), 0.0 );
		v.y += uLift * min( s.g, 3.0 ) * uDt; // capped: hot pockets must not blow the flow apart
		v.y += 0.8 * uWeight * min( s.r, 50.0 ) * uDt; // soot rides the hot gas: most of its weight is cancelled
	}
#else
#ifdef TRIP
	s.b += s.r * uDt; // age mass: the smoke already here ages (the sources below add age 0)
#endif
	s.r += e * uEmitMul * uDt;
	s.g += h * uEmitMul * uDt;
#endif
#ifdef TRIP
	// vortex rings: a torus of radius uRingR round each centre, axis uRingDir. Inside the ring the flow goes forward, round the
	// core and back outside (v_pol), plus the travel and a corkscrew spin. W25: smoke goes onto a tight core in the first
	// 0.1 s only (uRingS); the drive (uRingK) follows the travelling centre for the ring's life, so the core keeps rolling
	for( int i = 0; i < 2; ++i )
	{
		if( i >= uRingN )
			break;
		vec3 d    = p - uRingPos[ i ];
		float a   = dot( d, uRingDir[ i ] );
		vec3 rv   = d - a * uRingDir[ i ];
		float r   = length( rv );
		vec3 er   = rv / max( r, 1e-4 );
		float rho = length( vec2( r - uRingR[ i ], a ) );
		float sg  = max( 0.25 * uRingR[ i ], 1.0 * uCell );
		float k   = exp( -rho * rho / ( sg * sg ) );
		vec3 pol  = ( -( r - uRingR[ i ] ) * uRingDir[ i ] + a * er ) / max( rho, 1e-4 );
		vec3 vr   = uRingSpeed * ( 0.6 * uRingDir[ i ] + 1.5 * pol * min( rho / sg, 1.0 ) ) + uRingSpin * cross( uRingDir[ i ], er ) * min( r / uRingR[ i ], 1.0 );
		v         = mix( v, vr, min( 1.0, 2.0 * k * uRingK[ i ] ) );
		float ss  = max( 0.5 * sg, 0.8 * uCell );
		s.r += 3.0 * uRingSmoke * exp( -rho * rho / ( ss * ss ) ) * uRingS[ i ] * uDt; // x3: the old 0.15 s on a 2x wider core
	}
	s.r *= exp( uRingHold * uDt * clamp( hold, -2.0, 1.0 ) ); // a ring keeps its smoke while driven, then dissolves (3x Smoke Fade)
	// ghost faces: smoke in the SDF shell (~1 cell thick) of a copy of the mask that peels off
	for( int i = 0; i < 3; ++i )
	{
		if( i >= uGhostN )
			break;
		vec3 u = ( ( uGhostInv[ i ] * vec4( p, 1.0 ) ).xyz - uSdfMin ) / uSdfSize;
		if( any( lessThan( u, vec3( 0.0 ) ) ) || any( greaterThan( u, vec3( 1.0 ) ) ) )
			continue;
		float d = texture( uSdf, u ).r * uGhostToWorld[ i ] / uCell;
		s.r += uGhostSmoke * uGhostK[ i ] * exp( -d * d * 0.7 ) * uDt;
	}
#endif
	// the grid border is a soft sink (smoke leaving the box fades instead of piling up at the wall)
	vec3 bd = min( vec3( c ) + 0.5, vec3( float( uRes ) ) - vec3( c ) - 0.5 );
#ifdef TRIP
	float sink = exp( -3.0 * uDt * ( 1.0 - smoothstep( 0.0, 4.0, min( bd.x, min( bd.y, bd.z ) ) ) ) );
	s.r *= sink;
	s.b *= sink;
	if( uConfine > 0.0 )
	{
		float wall = smoothstep( uConfineR, uConfineR * 1.4, length( p - uConfineCenter ) );
		float cf   = exp( -uConfine * 6.0 * uDt * wall );
		s.r *= cf;
		s.b *= cf;
	}
#else
	s.r *= exp( -3.0 * uDt * ( 1.0 - smoothstep( 0.0, 4.0, min( bd.x, min( bd.y, bd.z ) ) ) ) );
	if( uConfine > 0.0 )
	{
		float wall = smoothstep( uConfineR, uConfineR * 1.4, length( p - uConfineCenter ) );
		s.r *= exp( -uConfine * 6.0 * uDt * wall );
	}
#endif
	// speed limit: at most 6 cells per substep (CFL-style; keeps even the extreme dial settings bounded)
	float vmax = 6.0 * uCell / uDt, sp = length( v );
	if( sp > vmax )
		v *= vmax / sp;
#ifdef TRIP
	// Trip fields: |grad density| per cell (from the smoke before this pass), the age mass capped at 30 s of age
	float grad = 0.5 * length( vec3( FetchC( uSca, c + ivec3( 1, 0, 0 ) ).r - FetchC( uSca, c - ivec3( 1, 0, 0 ) ).r,
	                                 FetchC( uSca, c + ivec3( 0, 1, 0 ) ).r - FetchC( uSca, c - ivec3( 0, 1, 0 ) ).r,
	                                 FetchC( uSca, c + ivec3( 0, 0, 1 ) ).r - FetchC( uSca, c - ivec3( 0, 0, 1 ) ).r ) );
	outV = vec4( v, length( w ) );
#ifdef FIRE
	outS = vec4( min( s.rg, vec2( 50.0 ) ), clamp( s.b, 0.0, 8.0 ), grad );
#else
	outS = vec4( min( s.rg, vec2( 50.0 ) ), clamp( s.b, 0.0, min( s.r, 50.0 ) * 30.0 ), grad );
#endif
#else
	outV = vec4( v, 0.0 );
	outS = vec4( min( s.rg, vec2( 50.0 ) ), 0.0, 0.0 );
#endif
}
)GLSL";

// Pass 5: divergence. Solid neighbours contribute the obstacle's velocity; the grid border is zero-gradient.
static const char* kSimDivFS = R"GLSL(
out vec4 outD;
uniform sampler2D uVel;
uniform sampler2D uObs;
uniform float uSolidPad;
vec3 VelAt( ivec3 n, vec3 self )
{
	if( !OnGrid( n ) )
		return self;
	vec4 ob = FetchC( uObs, n );
	return ob.w < uSolidPad ? ob.xyz : FetchC( uVel, n ).xyz;
}
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outD = vec4( 0.0 );
		return;
	}
	vec3 v = FetchC( uVel, c ).xyz;
	float d = VelAt( c + ivec3( 1, 0, 0 ), v ).x - VelAt( c - ivec3( 1, 0, 0 ), v ).x + VelAt( c + ivec3( 0, 1, 0 ), v ).y - VelAt( c - ivec3( 0, 1, 0 ), v ).y +
	          VelAt( c + ivec3( 0, 0, 1 ), v ).z - VelAt( c - ivec3( 0, 0, 1 ), v ).z;
	outD = vec4( FetchC( uObs, c ).w < uSolidPad ? 0.0 : d * 0.5 / uCell );
}
)GLSL";

// Pass 6: one Jacobi iteration of lap(p) = div (compact 7-point Laplacian, GPU Gems 3 ch.30). Solid neighbour: its
// pressure = this cell's (no flow through the wall); outside the grid: 0 (open: smoke can leave). NOTE (W20, measured):
// with central-difference divergence / gradient on this collocated grid the projection cannot reach zero divergence (the
// compact and the central stencils differ); the consistent 2h-spacing Laplacian was tried and is unstable (the odd / even
// checkerboard modes it cannot see grow under vorticity confinement). The compact one is the stable, standard choice.
static const char* kSimJacobiFS = R"GLSL(
out vec4 outP;
uniform sampler2D uP;
uniform sampler2D uDiv;
uniform sampler2D uObs;
uniform float uSolidPad;
float PAt( ivec3 n, float self )
{
	if( !OnGrid( n ) )
		return 0.0;
	return FetchC( uObs, n ).w < uSolidPad ? self : FetchC( uP, n ).r;
}
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outP = vec4( 0.0 );
		return;
	}
	float p = FetchC( uP, c ).r;
	float s = PAt( c + ivec3( 1, 0, 0 ), p ) + PAt( c - ivec3( 1, 0, 0 ), p ) + PAt( c + ivec3( 0, 1, 0 ), p ) + PAt( c - ivec3( 0, 1, 0 ), p ) +
	          PAt( c + ivec3( 0, 0, 1 ), p ) + PAt( c - ivec3( 0, 0, 1 ), p );
	outP = vec4( ( s - uCell * uCell * FetchC( uDiv, c ).r ) / 6.0 );
}
)GLSL";

// Pass 7: subtract the pressure gradient; free slip at solids (the normal component takes the obstacle's velocity).
static const char* kSimProjectFS = R"GLSL(
out vec4 outV;
uniform sampler2D uVel;
uniform sampler2D uP;
uniform sampler2D uObs;
uniform float uSolidPad;
void main()
{
	ivec3 c;
	if( !CellOf( gl_FragCoord.xy, c ) )
	{
		outV = vec4( 0.0 );
		return;
	}
	vec4 ob = FetchC( uObs, c );
	if( ob.w < uSolidPad )
	{
		outV = vec4( ob.xyz, 0.0 );
		return;
	}
	float p = FetchC( uP, c ).r;
	vec3 v  = FetchC( uVel, c ).xyz;
	vec3 pl, ph;
	bvec3 sl, sh;
	for( int a = 0; a < 3; ++a )
	{
		ivec3 e = ivec3( a == 0, a == 1, a == 2 );
		ivec3 nl = c - e, nh = c + e;
		sl[ a ] = OnGrid( nl ) && FetchC( uObs, nl ).w < uSolidPad;
		sh[ a ] = OnGrid( nh ) && FetchC( uObs, nh ).w < uSolidPad;
		pl[ a ] = !OnGrid( nl ) ? 0.0 : sl[ a ] ? p : FetchC( uP, nl ).r;
		ph[ a ] = !OnGrid( nh ) ? 0.0 : sh[ a ] ? p : FetchC( uP, nh ).r;
	}
	v -= ( ph - pl ) * ( 0.5 / uCell );
	for( int a = 0; a < 3; ++a )
		if( sl[ a ] || sh[ a ] )
			v[ a ] = ob[ a ];
#ifdef TRIP
	outV = vec4( v, FetchC( uVel, c ).a ); // a = |curl| from the force pass (Trip: Colour By Swirl)
#else
	outV = vec4( v, 0.0 );
#endif
}
)GLSL";

// The medium with the simulated smoke (Smoke Engine Sim / Sim + Detail). Same interface as kMediumLib; the plain lib
// stays untouched so the Noise engine's programs are the W3 programs, byte for byte. gTint colours the light the sim
// smoke scatters (Smoke Color x the projected input).
static const char* kMediumSimLib = R"GLSL(
uniform sampler3D uNoise;
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
uniform sampler2D uSimDen;
uniform int uSimDetail;
uniform vec3 uSmokeTint;
uniform float uProject;
uniform sampler2D uProjTex;
uniform mat4 uProjVP;
vec3 gTint = vec3( 1.0 );
float gSmokeDen = 0.0;  // the smoke's share of the density at the last MediumAt (W23 smoke ambient / key)
vec3 gSmokeTint = vec3( 1.0 );

float DetailAt( vec3 p )
{
	vec3 q    = p * uSmokeFreq * 2.5 + uDrift;
	vec3 warp = texture( uNoise, q * 0.43 + vec3( 0.37, 0.11, 0.73 ) ).gba - 0.5;
	q += warp * uTurb * 1.2;
	return texture( uNoise, q ).r * 0.7 + texture( uNoise, q * 2.63 + vec3( 0.13 ) ).r * 0.3;
}
#ifdef FIRE
uniform float uFlameTemp, uFlameGlow, uFlameTintMix, uFlameSoot;
uniform vec3 uFlameTint;
// Kelvin -> linear rgb, max component 1 (Helland's fit; the same as the surface Heat Glow)
vec3 FireBlackbody( float k )
{
	float t = clamp( k, 800.0, 15000.0 ) / 100.0;
	vec3 c;
	c.r = t <= 66.0 ? 1.0 : clamp( 1.292936 * pow( t - 60.0, -0.1332047592 ), 0.0, 1.0 );
	c.g = t <= 66.0 ? clamp( 0.3900815788 * log( t ) - 0.6318414438, 0.0, 1.0 ) : clamp( 1.129890861 * pow( t - 60.0, -0.0755148492 ), 0.0, 1.0 );
	c.b = t >= 66.0 ? 1.0 : ( t <= 19.0 ? 0.0 : clamp( 0.5432067891 * log( t - 10.0 ) - 1.19625408914, 0.0, 1.0 ) );
	return pow( c, vec3( 2.2 ) );
}
#endif
#ifdef TRIP
// W24 Trip: the sim medium with the Trip looks (kTripLib). The lookup is folded (kaleido) and carved (fractal); the Colour By
// fields come from the sim: density, heat (g), speed and swirl (velocity atlas, a = |curl|), age (b / density), height.
uniform sampler2D uSimVel;
vec2 MediumAt( vec3 p, out float smoke )
{
	float r    = length( p - uVolCenter );
	float edge = 1.0 - smoothstep( uVolRadius * 0.65, uVolRadius, r );
	vec3 pf    = TripFold( p );
	vec3 g     = ( pf - uBoxMin ) / uCell;
	vec3 bd    = min( g, vec3( float( uRes ) ) - g );
	bool inG   = InGrid( g );
	vec4 raw   = inG ? max( SampleG( uSimDen, g ), vec4( 0.0 ) ) : vec4( 0.0 );
	float bf   = smoothstep( 0.0, 5.0, min( bd.x, min( bd.y, bd.z ) ) );
	float sim  = raw.r * 0.3 * bf;
	if( uSimDetail == 1 )
		sim *= smoothstep( 0.25, 0.75, DetailAt( pf ) ) * 1.6;
	sim *= TripFractal( pf );
	vec3 tint = uSmokeTint;
	if( uProject > 0.0 && sim > 0.0 )
	{
		vec4 cp = uProjVP * vec4( p, 1.0 );
		vec3 pc = pow( texture( uProjTex, cp.xy / max( cp.w, 1e-5 ) * 0.5 + 0.5 ).rgb, vec3( 2.2 ) );
		sim *= mix( 1.0, 0.2 + 1.8 * dot( pc, vec3( 0.2126, 0.7152, 0.0722 ) ), uProject );
		tint *= mix( vec3( 1.0 ), pc * 1.6, uProject );
	}
	float field = 0.0;
	if( uColourBy == 1 )
		field = 1.0 - exp( -sim * 2.0 );
	else if( uColourBy == 2 )
		field = 1.0 - exp( -raw.g * 0.4 );
	else if( uColourBy == 3 || uColourBy == 4 )
	{
		vec4 vel = inG ? SampleG( uSimVel, g ) : vec4( 0.0 );
		field    = uColourBy == 3 ? 1.0 - exp( -length( vel.xyz ) / ( 1.5 * uTripUnit ) ) : 1.0 - exp( -vel.w * 0.12 );
	}
	else if( uColourBy == 5 )
		field = raw.b / max( raw.r, 1e-3 ) * 0.125; // one palette turn per 8 s of age
	else if( uColourBy == 6 )
		field = TripHeight( pf );
	// veins: the gradient relative to the density there (edges of curls light up, dense cores and empty air do not)
	TripShade( pf, sim, field, 1.0 - exp( -raw.g * 0.4 ), raw.a * bf / ( raw.r + 0.5 ), tint );
	smoke      = min( sim, 1.0 );
	float ds   = uSmoke * sim;
	float dm   = uHaze * edge + ds;
	gTint      = mix( vec3( 1.0 ), tint, ds / max( dm, 1e-6 ) );
	gSmokeDen  = ds;
	gSmokeTint = tint;
	gEmit *= uSmoke;
#ifdef FIRE
	{
		// the flames: blackbody light by temperature (heat 0..: T up to Flame Temp), brightness ~ (T / Flame Temp)^4; the hot
		// gas has a little soot of its own, so the march integrates its light even where no smoke is left
		float tn = 1.0 - exp( -max( raw.g * bf - 0.3, 0.0 ) * 0.3 ); // the flame shows above an ignition heat
		float K  = 700.0 + ( uFlameTemp - 700.0 ) * tn;
		float q  = K / uFlameTemp;
		vec3 c   = mix( FireBlackbody( K ), uFlameTint * dot( FireBlackbody( K ), vec3( 0.3333 ) ) * 1.6, uFlameTintMix );
		gEmit += c * ( uFlameGlow * q * q * q * q * tn * 8.0 );
		dm += uFlameSoot * tn;
	}
#endif
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
	vec3 g     = ( p - uBoxMin ) / uCell;
	// the grid's last cells fade out: its box never shows as an edge
	vec3 bd    = min( g, vec3( float( uRes ) ) - g );
	float sim  = InGrid( g ) ? max( SampleG( uSimDen, g ).r, 0.0 ) * 0.3 * smoothstep( 0.0, 5.0, min( bd.x, min( bd.y, bd.z ) ) ) : 0.0;
	if( uSimDetail == 1 )
		sim *= smoothstep( 0.25, 0.75, DetailAt( p ) ) * 1.6;
	vec3 tint = uSmokeTint;
	if( uProject > 0.0 && sim > 0.0 )
	{
		vec4 cp = uProjVP * vec4( p, 1.0 );
		vec3 pc = pow( texture( uProjTex, cp.xy / max( cp.w, 1e-5 ) * 0.5 + 0.5 ).rgb, vec3( 2.2 ) );
		sim *= mix( 1.0, 0.2 + 1.8 * dot( pc, vec3( 0.2126, 0.7152, 0.0722 ) ), uProject );
		tint *= mix( vec3( 1.0 ), pc * 1.6, uProject );
	}
	smoke      = min( sim, 1.0 );
	float ds   = uSmoke * sim;
	float dm   = uHaze * edge + ds;
	gTint      = mix( vec3( 1.0 ), tint, ds / max( dm, 1e-6 ) );
	gSmokeDen  = ds;
	gSmokeTint = tint;
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
}
