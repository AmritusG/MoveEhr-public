#pragma once
// MoveEhr W2 Part B: true voxels (docs/PLAN.md section 7). Every pass is a pure function of its inputs (no additive blending; the
// owner of a cell = its lowest spot, by the depth test), so the same pose / dials give the same bits.
// Grid: the side's fitted space, box [uGridMin, uGridMin + uRes x uCell]^3. Atlas: slice z of the res^3 grid is the tile
// (z % uTiles, z / uTiles) of a uSize^2 texture; a texel past the grid is empty.

namespace mehr
{
// shared by the voxel passes: atlas texel <-> cell
static const char* kVoxLib = R"GLSL(#version 410 core
uniform int uRes, uTiles, uSize;
const uint kEmpty = 0xFFFFFFFFu, kMean = 0xFFFFFFFEu;
ivec2 CellPx( ivec3 c ) { return ivec2( c.x + ( c.z % uTiles ) * uRes, c.y + ( c.z / uTiles ) * uRes ); }
bool PxCell( ivec2 p, out ivec3 c )
{
	ivec2 t = p / uRes;
	c       = ivec3( p - t * uRes, t.y * uTiles + t.x );
	return t.x < uTiles && c.z < uRes;
}
// the HistoPyramid: level j (1..log2 uSize, size uSize >> j) sits at x = uSize - ( uSize >> ( j - 1 ) ) of atlas A (odd j) or B (even j),
// so a level is never read from the texture it is written to
uniform sampler2D uHpA; // unit 53
uniform sampler2D uHpB; // unit 56
int HpOff( int j ) { return uSize - ( uSize >> ( j - 1 ) ); }
vec4 HpAt( int j, ivec2 p )
{
	ivec2 q = ivec2( HpOff( j ) + p.x, p.y );
	return ( j & 1 ) == 1 ? texelFetch( uHpA, q, 0 ) : texelFetch( uHpB, q, 0 );
}
)GLSL";

// 1. splat: a captured spot -> its cell's texel; colour = the spot index, depth = its distance to the cell centre (LESS: the nearest owns it)
static const char* kVoxSplatVS = R"GLSL(
uniform samplerBuffer uCapPos; // unit 45: world position (capture)
uniform samplerBuffer uCapCol; // unit 46: base colour, a = visible
uniform mat4 uGridFromWorld;
uniform float uGridMin, uCell, uSpots;
flat out uint vId;
void main()
{
	vec4 cp   = texelFetch( uCapPos, gl_VertexID );
	float vis = texelFetch( uCapCol, gl_VertexID ).a;
	vec3 g    = ( uGridFromWorld * vec4( cp.xyz, 1.0 ) ).xyz;
	ivec3 c   = ivec3( floor( ( g - uGridMin ) / uCell ) );
	bool ok   = vis > 0.5 && all( greaterThanEqual( c, ivec3( 0 ) ) ) && all( lessThan( c, ivec3( uRes ) ) );
	vec2 px   = vec2( CellPx( c ) ) + 0.5;
	// the owner = the spot nearest the cell's centre (a thin inset part, a visor, keeps its cells); equal depths keep the first spot
	// (GL resolves a depth test in primitive order), so the owner is the same every time
	float dn  = clamp( length( g - ( uGridMin + ( vec3( c ) + 0.5 ) * uCell ) ) / ( uCell * 0.8661 ), 0.0, 1.0 );
	gl_Position  = ok ? vec4( px / float( uSize ) * 2.0 - 1.0, dn * 1.998 - 0.999, 1.0 ) : vec4( 2.0, 2.0, 2.0, 1.0 );
	gl_PointSize = 1.0;
	vId          = uint( gl_VertexID );
}
)GLSL";
static const char* kVoxSplatFS = R"GLSL(
flat in uint vId;
out uint oOwner;
void main() { oOwner = vId; }
)GLSL";

// a full-target triangle (passes 2, 3, 4)
static const char* kVoxQuadVS = R"GLSL(
void main()
{
	vec2 p      = vec2( ( gl_VertexID & 1 ) * 4 - 1, ( gl_VertexID & 2 ) * 2 - 1 );
	gl_Position = vec4( p, 0.0, 1.0 );
}
)GLSL";

// 2. column extents over a res x res target: texel (u, v) = the first / last surface cell along X of the column (y u, z v), along Y of
// (x u, z v), along Z of (x u, y v); none = (res, -1)
static const char* kVoxExtentFS = R"GLSL(
uniform usampler2D uOwner; // unit 47
layout( location = 0 ) out vec4 oXY;
layout( location = 1 ) out vec4 oZ;
bool Surf( ivec3 c ) { return texelFetch( uOwner, CellPx( c ), 0 ).r != kEmpty; }
void main()
{
	ivec2 q = ivec2( gl_FragCoord.xy );
	vec2 ex = vec2( float( uRes ), -1.0 ), ey = ex, ez = ex;
	for( int t = 0; t < uRes; ++t )
	{
		if( Surf( ivec3( t, q.x, q.y ) ) )
			ex = vec2( min( ex.x, float( t ) ), float( t ) );
		if( Surf( ivec3( q.x, t, q.y ) ) )
			ey = vec2( min( ey.x, float( t ) ), float( t ) );
		if( Surf( ivec3( q.x, q.y, t ) ) )
			ez = vec2( min( ez.x, float( t ) ), float( t ) );
	}
	oXY = vec4( ex, ey );
	oZ  = vec4( ez, 0.0, 0.0 );
}
)GLSL";

// 3. base: solid = surface, or between surface cells along all three axes (a visual hull); the owner (colour) of an interior cell = the
// nearest surface owner within 8 cells along the 6 axes (fixed order), else kMean. out 0 = 1 per solid cell (the proxies' count;
// SMOOTH: the field before the blur)
static const char* kVoxBaseFS = R"GLSL(
uniform usampler2D uOwner;  // unit 47
uniform sampler2D uExtXY;   // unit 48
uniform sampler2D uExtZ;    // unit 49
layout( location = 0 ) out float oCount;
layout( location = 1 ) out uint oOwn;
uint Own( ivec3 c ) { return texelFetch( uOwner, CellPx( c ), 0 ).r; }
#ifdef CARVE
// W4 A2: the concave fill. A hull cell (not surface) marches the 6 axis directions to the first surface cell and reads that cell's owner's
// captured world normal: facing along the march = the cell sits behind that surface (inside), facing back = outside (a concave pocket,
// e.g. between horns and the head). Inside wins ties (the hull's answer); a march that leaves the grid does not vote
uniform samplerBuffer uCapNrm; // unit 51: the spots' world normals (capture)
uniform samplerBuffer uCapPos; // unit 45: the spots' world positions
uniform mat3 uGridToWorldR;   // grid axis -> world direction
uniform mat4 uWorldFromGridC; // grid -> world
uniform float uGridMin, uCell;
uniform float uTrim; // world: how far outside its owner's plane a surface cell's centre must lie to be dropped
// a surface cell's centre against its owner spot's tangent plane: > 0 = outside the surface
float CentreSide( ivec3 c, uint o )
{
	vec3 cw = ( uWorldFromGridC * vec4( uGridMin + ( vec3( c ) + 0.5 ) * uCell, 1.0 ) ).xyz;
	return dot( cw - texelFetch( uCapPos, int( o ) ).xyz, texelFetch( uCapNrm, int( o ) ).xyz );
}
bool HullAt( ivec3 c )
{
	vec2 ex = texelFetch( uExtXY, c.yz, 0 ).xy, ey = texelFetch( uExtXY, c.xz, 0 ).zw, ez = texelFetch( uExtZ, c.xy, 0 ).xy;
	return float( c.x ) > ex.x && float( c.x ) < ex.y && float( c.y ) > ey.x && float( c.y ) < ey.y && float( c.z ) > ez.x && float( c.z ) < ez.y;
}
bool Inside( ivec3 c )
{
	const ivec3 dirs[ 6 ] = ivec3[ 6 ]( ivec3( 1, 0, 0 ), ivec3( -1, 0, 0 ), ivec3( 0, 1, 0 ), ivec3( 0, -1, 0 ), ivec3( 0, 0, 1 ), ivec3( 0, 0, -1 ) );
	int votes = 0;
	for( int k = 0; k < 6; ++k )
	{
		vec3 dw = uGridToWorldR * vec3( dirs[ k ] );
		for( int d = 1; d < uRes; ++d )
		{
			ivec3 n = c + dirs[ k ] * d;
			if( any( lessThan( n, ivec3( 0 ) ) ) || any( greaterThanEqual( n, ivec3( uRes ) ) ) )
				break;
			uint q = Own( n );
			if( q != kEmpty )
			{
				float f = dot( texelFetch( uCapNrm, int( q ) ).xyz, dw );
				votes += f > 0.0 ? 1 : f < 0.0 ? -1 : 0;
				break;
			}
		}
	}
	return votes >= 0;
}
#endif
void main()
{
	ivec3 c;
	oCount = 0.0, oOwn = kEmpty;
	if( !PxCell( ivec2( gl_FragCoord.xy ), c ) )
		return;
	uint o     = Own( c );
	bool solid = o != kEmpty;
#ifdef CARVE
	// W4 A2 trim: a surface cell whose centre lies outside the surface is dropped when the next cell inward (against the normal's main
	// axis) still holds the surface (a surface cell with its centre inside, or a hull cell that votes inside): the shape is no longer the
	// surface grown by up to a cell, and a part thinner than a cell keeps its cells
	if( solid && CentreSide( c, o ) > uTrim )
	{
		vec3 ng  = transpose( uGridToWorldR ) * texelFetch( uCapNrm, int( o ) ).xyz;
		vec3 an  = abs( ng );
		ivec3 st = an.x >= an.y && an.x >= an.z ? ivec3( ng.x > 0.0 ? -1 : 1, 0, 0 ) : an.y >= an.z ? ivec3( 0, ng.y > 0.0 ? -1 : 1, 0 ) : ivec3( 0, 0, ng.z > 0.0 ? -1 : 1 );
		ivec3 n  = c + st;
		if( all( greaterThanEqual( n, ivec3( 0 ) ) ) && all( lessThan( n, ivec3( uRes ) ) ) )
		{
			uint q   = Own( n );
			// the SAME surface inward (its owner faces the same way): a different one behind it (a body under armour) would show through
			bool inn = q != kEmpty ? CentreSide( n, q ) <= 0.0 && dot( texelFetch( uCapNrm, int( q ) ).xyz, texelFetch( uCapNrm, int( o ) ).xyz ) > 0.5
			                       : HullAt( n ) && Inside( n );
			if( inn )
				return;
		}
	}
#endif
	if( !solid )
	{
		vec2 ex = texelFetch( uExtXY, c.yz, 0 ).xy, ey = texelFetch( uExtXY, c.xz, 0 ).zw, ez = texelFetch( uExtZ, c.xy, 0 ).xy;
		solid   = float( c.x ) > ex.x && float( c.x ) < ex.y && float( c.y ) > ey.x && float( c.y ) < ey.y && float( c.z ) > ez.x && float( c.z ) < ez.y;
#ifdef CARVE
		solid   = solid && Inside( c );
#endif
		if( solid )
		{
			o = kMean;
			const ivec3 dirs[ 6 ] = ivec3[ 6 ]( ivec3( 1, 0, 0 ), ivec3( -1, 0, 0 ), ivec3( 0, 1, 0 ), ivec3( 0, -1, 0 ), ivec3( 0, 0, 1 ), ivec3( 0, 0, -1 ) );
			for( int d = 1; d <= 8 && o == kMean; ++d )
				for( int k = 0; k < 6; ++k )
				{
					ivec3 n = c + dirs[ k ] * d;
					if( any( lessThan( n, ivec3( 0 ) ) ) || any( greaterThanEqual( n, ivec3( uRes ) ) ) )
						continue;
					uint q = Own( n );
					if( q != kEmpty )
					{
						o = q;
						break;
					}
				}
		}
	}
	oCount = solid ? 1.0 : 0.0;
	oOwn   = solid ? o : kEmpty;
}
)GLSL";

// W3 PART SIZES 3d: one block of uL^3 fine cells per texel (Lib with the BLOCK grid: uRes / uTiles are the block grid's): x = its solid
// cells, y = those whose body part's size is uL or more (the votes), z = the colour owner: the voting cell with a real owner nearest the
// block centre (else any cell's; the first in scan order on ties), else kMean
static const char* kVoxBlockFS = R"GLSL(
uniform sampler2D uCntF;    // unit 58: 1 per solid fine cell
uniform usampler2D uOwnF;   // unit 59: the fine owners
uniform samplerBuffer uRef; // unit 57: the spots (w = the body part: 0 Head 1 Torso 2 Arms 3 Hands 4 Legs)
uniform int uFRes, uFTiles, uL, uLvl;
uniform int uPartLvl[ 5 ];
layout( location = 0 ) out uvec4 oBlk;
void main()
{
	ivec3 b;
	oBlk = uvec4( 0u, 0u, kMean, 0u );
	if( !PxCell( ivec2( gl_FragCoord.xy ), b ) )
		return;
	uint solid = 0u, votes = 0u, best = kMean, any_ = kMean;
	float bd = 1e9, ad = 1e9;
	vec3 mid = vec3( b * uL ) + 0.5 * float( uL - 1 );
	for( int z = 0; z < uL; ++z )
		for( int y = 0; y < uL; ++y )
			for( int x = 0; x < uL; ++x )
			{
				ivec3 c = b * uL + ivec3( x, y, z );
				if( any( greaterThanEqual( c, ivec3( uFRes ) ) ) )
					continue;
				ivec2 p = ivec2( c.x + ( c.z % uFTiles ) * uFRes, c.y + ( c.z / uFTiles ) * uFRes );
				if( texelFetch( uCntF, p, 0 ).r < 0.5 )
					continue;
				++solid;
				uint o    = texelFetch( uOwnF, p, 0 ).r;
				int part  = o < kMean ? clamp( int( texelFetch( uRef, int( o ) ).w + 0.5 ), 0, 4 ) : 1;
				bool vote = uPartLvl[ part ] >= uLvl;
				votes += vote ? 1u : 0u;
				if( o < kMean )
				{
					float d = dot( vec3( c ) - mid, vec3( c ) - mid );
					if( vote && d < bd )
						bd = d, best = o;
					if( d < ad )
						ad = d, any_ = o;
				}
			}
	oBlk = uvec4( solid, votes, best != kMean ? best : any_, 0u );
}
)GLSL";
// 3e: the merged atlases. A cell belongs to the largest block holding it whose solid cells mostly vote for that size or more (one per
// cell: a partition), else to itself. Proxies: the block's origin cell carries it (count 1, owner | level << 28; a mean-coloured block
// 0xFFFFFFF0 + level); SMOOTH: every cell of the block is solid with the block's owner (a blocky field)
static const char* kVoxMergeFS = R"GLSL(
uniform sampler2D uCntF;   // unit 58
uniform usampler2D uOwnF;  // unit 59
uniform usampler2D uBlk2;  // unit 60
uniform usampler2D uBlk4;  // unit 61
uniform usampler2D uBlk8;  // unit 62
uniform ivec3 uBN, uBT;    // the block grids (x2, x4, x8): cells across, tiles a row
uniform int uMaxLvl;       // the largest level in use (1..3)
uniform int uSmoothM;
layout( location = 0 ) out float oCount;
layout( location = 1 ) out uint oOwn;
ivec2 BPx( ivec3 b, int n, int t ) { return ivec2( b.x + ( b.z % t ) * n, b.y + ( b.z / t ) * n ); }
uvec4 Blk( int lvl, ivec3 c )
{
	ivec3 b = c >> lvl;
	if( lvl == 3 )
		return texelFetch( uBlk8, BPx( b, uBN.z, uBT.z ), 0 );
	if( lvl == 2 )
		return texelFetch( uBlk4, BPx( b, uBN.y, uBT.y ), 0 );
	return texelFetch( uBlk2, BPx( b, uBN.x, uBT.x ), 0 );
}
void main()
{
	ivec3 c;
	oCount = 0.0, oOwn = kEmpty;
	if( !PxCell( ivec2( gl_FragCoord.xy ), c ) )
		return;
	for( int lvl = uMaxLvl; lvl >= 1; --lvl )
	{
		uvec4 k = Blk( lvl, c );
		if( k.x > 0u && k.y * 2u > k.x )
		{
			if( uSmoothM == 1 )
				oCount = 1.0, oOwn = k.z;
			else if( all( equal( c, ( c >> lvl ) << lvl ) ) )
				oCount = 1.0, oOwn = k.z < kMean ? ( k.z | ( uint( lvl ) << 28 ) ) : 0xFFFFFFF0u + uint( lvl );
			return;
		}
	}
	ivec2 p = ivec2( gl_FragCoord.xy );
	oCount  = texelFetch( uCntF, p, 0 ).r;
	oOwn    = texelFetch( uOwnF, p, 0 ).r;
}
)GLSL";

// SMOOTH 3b: the field blurred along one axis (3 passes, x then y then z), sigma from Voxel Bevel; 3c: the marching-cubes triangle
// count of the cell whose corners are the cell centres c .. c + 1
static const char* kVoxBlurFS = R"GLSL(
uniform sampler2D uField; // unit 50
uniform ivec3 uAxis;
uniform float uSigma;     // cells (0 = copy)
out float oField;
void main()
{
	ivec3 c;
	oField = 0.0;
	if( !PxCell( ivec2( gl_FragCoord.xy ), c ) )
		return;
	if( uSigma <= 0.0 )
	{
		oField = texelFetch( uField, CellPx( c ), 0 ).r;
		return;
	}
	int r   = int( ceil( 2.5 * uSigma ) );
	float s = 0.0, w = 0.0;
	for( int i = -r; i <= r; ++i )
	{
		ivec3 n  = c + uAxis * i;
		float k  = exp( -0.5 * float( i * i ) / ( uSigma * uSigma ) );
		bool in_ = all( greaterThanEqual( n, ivec3( 0 ) ) ) && all( lessThan( n, ivec3( uRes ) ) );
		s += k * ( in_ ? texelFetch( uField, CellPx( n ), 0 ).r : 0.0 );
		w += k;
	}
	oField = s / w;
}
)GLSL";
static const char* kVoxMcCountFS = R"GLSL(
uniform sampler2D uField; // unit 50
uniform float uIso;
out float oCount;
// marching tetrahedra: the cell (corners c .. c + 1, bit 1 = x, 2 = y, 4 = z) as 6 tetrahedra round its 0-7 diagonal (the same split in
// every cell, so neighbours agree); a tetrahedron with 1 or 3 corners inside gives 1 triangle, with 2 inside 2
const ivec4 kTet[ 6 ] = ivec4[ 6 ]( ivec4( 0, 1, 3, 7 ), ivec4( 0, 3, 2, 7 ), ivec4( 0, 2, 6, 7 ), ivec4( 0, 6, 4, 7 ), ivec4( 0, 4, 5, 7 ), ivec4( 0, 5, 1, 7 ) );
float F( ivec3 c ) { return all( lessThan( c, ivec3( uRes ) ) ) ? texelFetch( uField, CellPx( c ), 0 ).r : 0.0; }
void main()
{
	ivec3 c;
	oCount = 0.0;
	if( !PxCell( ivec2( gl_FragCoord.xy ), c ) )
		return;
	int in_[ 8 ];
	for( int i = 0; i < 8; ++i )
		in_[ i ] = F( c + ivec3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 ) ) > uIso ? 1 : 0;
	int n = 0;
	for( int t = 0; t < 6; ++t )
	{
		int m = in_[ kTet[ t ].x ] + in_[ kTet[ t ].y ] + in_[ kTet[ t ].z ] + in_[ kTet[ t ].w ];
		n += m == 2 ? 2 : ( m == 1 || m == 3 ) ? 1 : 0;
	}
	oCount = float( n );
}
)GLSL";

// 4. HistoPyramid: level j texel = the four child totals (x: (0,0), y: (1,0), z: (0,1), w: (1,1)); level 1 reads the base counts
static const char* kVoxReduceFS = R"GLSL(
uniform sampler2D uBase; // unit 52: base counts (level 1 only)
uniform int uLevelJ;     // the level written (its region is the viewport)
out vec4 oSums;
float T( ivec2 p ) { return uLevelJ == 1 ? texelFetch( uBase, p, 0 ).r : dot( HpAt( uLevelJ - 1, p ), vec4( 1.0 ) ); }
void main()
{
	ivec2 p = ( ivec2( gl_FragCoord.xy ) - ivec2( HpOff( uLevelJ ), 0 ) ) * 2;
	oSums   = vec4( T( p ), T( p + ivec2( 1, 0 ) ), T( p + ivec2( 0, 1 ) ), T( p + ivec2( 1, 1 ) ) );
}
)GLSL";

// 5. the two indirect commands from the pyramid's total (1 vertex, transform feedback): traverse {n, 1, 0, 0}, draw {verts, n, 0, 0}
static const char* kVoxCmdVS = R"GLSL(
uniform int uTop; // log2 uSize: the 1 x 1 level
uniform uint uCap, uVerts;
flat out uvec4 oTrav;
flat out uvec4 oDraw;
void main()
{
	uint n = min( uint( dot( HpAt( uTop, ivec2( 0 ) ), vec4( 1.0 ) ) + 0.5 ), uCap );
	oTrav  = uvec4( n, 1u, 0u, 0u );
	oDraw  = uvec4( uVerts, n, 0u, 0u );
}
)GLSL";

// 6. traverse: output i -> its base texel (descend the pyramid) -> (cell | k << 24, owner)
static const char* kVoxTraverseVS = R"GLSL(
uniform usampler2D uOwnRes; // unit 54: resolved owners
uniform int uTop;
flat out uvec2 oCell;
void main()
{
	float i = float( gl_VertexID );
	ivec2 p = ivec2( 0 );
	for( int l = uTop; l >= 1; --l )
	{
		vec4 c  = HpAt( l, p );
		ivec2 o = ivec2( 0, 0 );
		if( i < c.x )
			o = ivec2( 0, 0 );
		else if( ( i -= c.x ) < c.y )
			o = ivec2( 1, 0 );
		else if( ( i -= c.y ) < c.z )
			o = ivec2( 0, 1 );
		else
		{
			i -= c.z;
			o = ivec2( 1, 1 );
		}
		p = p * 2 + o;
	}
	ivec3 cell;
	PxCell( p, cell );
	uint own = texelFetch( uOwnRes, p, 0 ).r;
	oCell    = uvec2( uint( cell.x ) | ( uint( cell.y ) << 8 ) | ( uint( cell.z ) << 16 ) | ( uint( i + 0.5 ) << 24 ), own );
}
)GLSL";

// motions, reveal and the hop (shared by the cube and the smooth draws). Grid space; c = the cell, g = its centre, h = its hashes.
static const char* kVoxMotion = R"GLSL(
uniform float uGridMin, uCell;
uniform int uMotion;      // 0 None 1 Build Up 2 Build Down 3 Explode Out 4 Implode 5 Ripple Wave 6 Noise Jitter 7 Spin Each 8 Scale Pulse
                          // 9 Gravity Drop 10 Vortex 11 Scatter Cloud 12 Row Slide 13 Magnet 14 Breathe 15 Beat Pop 16 Rain In 17 Melt Down
                          // 18 Tetris Step 19 Shuffle Swap
uniform float uAmt;       // Voxel Amount: strength (periodic motions)
uniform float uDis;       // how far apart (transient motions): Amount, or Amount x the cycle when Voxel Speed > 0
uniform float uPhase;     // turns, wrapped at 1000 (whole cycles per 1000 everywhere)
uniform float uBeat;      // the host beat's phase 0..1
uniform float uBeatN;     // beats, mod 1024
uniform float uReveal;    // 1 = all
uniform int uRevealStyle; // 0 Height 1 Centre Out 2 Noise 3 Random
uniform int uHopSide;     // -1 no hop; 0 the outgoing side keeps hash >= uHopE; 1 the incoming side keeps hash < uHopE
uniform float uHopE;
const float TAU = 6.2831853;
vec4 VHash( ivec3 c )
{
	uvec3 q = uvec3( c + 4096 ) * uvec3( 1597334677u, 3812015801u, 2798796415u );
	uint n  = ( q.x ^ q.y ^ q.z ) * 1597334677u;
	uvec4 r = uvec4( n, n * 747796405u + 2891336453u, n * 2891336453u + 747796405u, ( n ^ ( n >> 15 ) ) * 2246822519u );
	r       = ( r ^ ( r >> 16 ) ) * 2654435769u;
	return vec4( r >> 8 ) / 16777216.0;
}
mat3 RotAxisV( vec3 a, float t )
{
	float c = cos( t ), s = sin( t ), k = 1.0 - c;
	return mat3( c + a.x * a.x * k, a.y * a.x * k + a.z * s, a.z * a.x * k - a.y * s, a.x * a.y * k - a.z * s, c + a.y * a.y * k,
	             a.z * a.y * k + a.x * s, a.x * a.z * k + a.y * s, a.y * a.z * k - a.x * s, c + a.z * a.z * k );
}
float Sat( float x ) { return clamp( x, 0.0, 1.0 ); }
// W3 (Amrit: "allow for longer / further out motions"): the distance a motion travels, x1 up to Amount 0.5 (the W2 motions exactly),
// rising to x3 at Amount 1
float Reach( float x )
{
	float r = Sat( x * 2.0 - 1.0 );
	return 1.0 + 2.0 * r * r;
}
// a distance stretched by Reach; at x <= 0.5 the value untouched (W2's arithmetic bit for bit)
float Rch( float v, float x ) { return x > 0.5 ? v * Reach( x ) : v; }
vec2 Rch( vec2 v, float x ) { return x > 0.5 ? v * Reach( x ) : v; }
vec3 Rch( vec3 v, float x ) { return x > 0.5 ? v * Reach( x ) : v; }
float Ease( float x )
{
	x = Sat( x );
	return x * x * ( 3.0 - 2.0 * x );
}
// the voxel's offset (grid units), rotation and scale
void Motion( ivec3 c, vec3 g, vec4 h, out vec3 off, out mat3 R, out float s )
{
	off       = vec3( 0.0 ), R = mat3( 1.0 ), s = 1.0;
	float top = uGridMin + float( uRes ) * uCell;
	float k   = Sat( ( g.y - uGridMin ) / ( top - uGridMin ) ); // height 0..1 in the grid
	float fl  = -1.0;                                            // the floor (fitted)
	vec3 ax   = normalize( h.xyz - 0.5 + 1e-4 );
	float d = uDis, a = uAmt;
	if( uMotion == 1 ) // BUILD UP: built from the bottom; coming apart, the top leaves first, upwards
	{
		float u = Ease( ( d * 1.3 - ( 1.0 - k ) ) / 0.3 );
		off.y   = Rch( u * 1.2, d ), s = 1.0 - u;
	}
	else if( uMotion == 2 ) // BUILD DOWN: built from the top; the bottom leaves first, downwards
	{
		float u = Ease( ( d * 1.3 - k ) / 0.3 );
		off.y   = Rch( -u * 1.2, d ), s = 1.0 - u;
	}
	else if( uMotion == 3 ) // EXPLODE OUT
	{
		float u = Ease( d * 1.2 - 0.2 * h.w );
		off     = Rch( normalize( g + ( h.xyz - 0.5 ) * 0.6 + 1e-4 ) * u * 2.6 * ( 0.6 + 0.8 * h.x ), d );
		R       = RotAxisV( ax, u * TAU * ( 0.5 + h.y ) );
	}
	else if( uMotion == 4 ) // IMPLODE: into the centre, the rim first
	{
		float u = Ease( d * 1.4 - 0.4 * ( 1.0 - Sat( length( g ) ) ) );
		off     = -g * u, s = 1.0 - u, R = RotAxisV( vec3( 0.0, 1.0, 0.0 ), u * 3.1416 );
	}
	else if( uMotion == 5 ) // RIPPLE WAVE: rings out over the floor plan
		off.y = Rch( a * 0.18 * sin( TAU * ( uPhase - length( g.xz ) * 1.5 ) ), a );
	else if( uMotion == 6 ) // NOISE JITTER: a new nudge 8 times a cycle
		off = Rch( a * 1.2 * uCell * ( VHash( c + ivec3( int( floor( uPhase * 8.0 ) ), 7, 3 ) ).xyz - 0.5 ), a );
	else if( uMotion == 7 ) // SPIN EACH: whole turns per cycle (1..3)
		R = RotAxisV( ax, a * TAU * fract( uPhase * ( 1.0 + floor( h.x * 3.0 ) ) + h.y ) );
	else if( uMotion == 8 ) // SCALE PULSE: a wave up the model
		s = 1.0 - a * 0.7 * ( 0.5 + 0.5 * sin( TAU * ( uPhase - k ) ) );
	else if( uMotion == 9 ) // GRAVITY DROP: each falls to the floor (the top last), and rebuilds as Amount comes back
	{
		float u    = Sat( d * 1.4 - 0.4 * k - 0.15 * h.x );
		float land = fl + uCell * ( 0.5 + floor( h.z * 3.0 ) ); // a little pile
		float y    = g.y - 4.0 * u * u * ( g.y - land + 0.3 );
		off.y      = max( y, land ) - g.y;
		off.xz     = Rch( ( h.xy - 0.5 ) * u * 0.4, d );
		R          = RotAxisV( ax, Ease( u * 2.0 - 1.0 ) * 1.5708 );
	}
	else if( uMotion == 10 ) // VORTEX: the floor plan turns, faster in the middle, twisting with height
	{
		float r  = length( g.xz );
		mat3 Rv  = RotAxisV( vec3( 0.0, 1.0, 0.0 ), a * ( TAU * fract( uPhase * 2.0 ) + 3.0 * k ) / ( 0.6 + r ) );
		off      = Rv * g - g;
	}
	else if( uMotion == 11 ) // SCATTER CLOUD: out to a cloud round the model and back
	{
		float u   = Ease( d * 1.25 - 0.25 * h.w );
		vec3 dir  = normalize( h.xyz - 0.5 + 1e-4 );
		vec3 home = Rch( dir * ( 1.1 + 0.7 * h.w ), d );
		off       = ( home - g ) * u, s = 1.0 - 0.5 * u, R = RotAxisV( ax, u * TAU * h.x );
	}
	else if( uMotion == 12 ) // ROW SLIDE: rows slide side to side, alternate rows the other way
		off.x = Rch( a * 0.35 * sin( TAU * ( uPhase + float( c.y ) * 0.07 ) ) * ( ( c.y & 1 ) == 0 ? 1.0 : -1.0 ), a );
	else if( uMotion == 13 ) // MAGNET: pulled to a point above the right shoulder, the nearest first
	{
		vec3 m  = vec3( 1.2, 0.9, 0.6 );
		float u = Ease( d * 1.5 - 0.5 * Sat( length( g - m ) / 3.0 ) );
		off     = ( m + ( h.xyz - 0.5 ) * 0.25 - g ) * u, s = 1.0 - 0.6 * u;
	}
	else if( uMotion == 14 ) // BREATHE
	{
		float b = Rch( a * 0.12 * sin( TAU * uPhase ), a );
		off     = g * b, s = 1.0 + b;
	}
	else if( uMotion == 15 ) // BEAT POP: on every beat a new third of the voxels pops out and back
	{
		vec4 q  = VHash( c + ivec3( int( uBeatN ), 11, 5 ) );
		float e = exp( -uBeat * 7.0 ) * ( q.x < 0.33 ? 1.0 : 0.0 );
		s       = 1.0 + a * 0.9 * e, off = normalize( g + 1e-4 ) * a * 0.15 * e;
	}
	else if( uMotion == 16 ) // RAIN IN: falls in from high above in a random order
	{
		float u = Ease( d * 1.5 - 0.5 * h.x );
		off.y   = Rch( u * 3.0, d ), s = 1.0 - 0.3 * u;
	}
	else if( uMotion == 17 ) // MELT DOWN: sinks and spreads over the floor, the top last
	{
		float u = Sat( d * 1.3 - 0.3 * k );
		float y = mix( g.y, fl + uCell * ( 0.5 + floor( h.x * 2.0 ) ), Ease( u ) );
		off.y   = y - g.y;
		off.xz  = Rch( g.xz * Ease( u ) * ( 0.4 + 0.6 * ( 1.0 - k ) ), d );
		s       = 1.0 - 0.25 * u;
	}
	else if( uMotion == 18 ) // TETRIS STEP: drops in from above in whole-cell steps, row by row
	{
		float u = Sat( d * 1.4 - 0.4 * k );
		off.y   = Rch( ceil( u * 12.0 ) * uCell * 2.0, d );
	}
	else if( uMotion == 19 ) // SHUFFLE SWAP: a share (Amount) hops to a neighbouring cell and back, 4 hops a cycle
	{
		float st = floor( uPhase * 4.0 ), u = fract( uPhase * 4.0 );
		vec4 q   = VHash( c + ivec3( int( st ), 3, 9 ) );
		int ax3  = int( q.y * 3.0 );
		vec3 dir = vec3( ax3 == 0 ? 1.0 : 0.0, ax3 == 1 ? 1.0 : 0.0, ax3 == 2 ? 1.0 : 0.0 ) * ( q.z < 0.5 ? -1.0 : 1.0 );
		off      = q.x < a ? dir * uCell * 2.0 * sin( 3.1415927 * u ) : vec3( 0.0 );
	}
	// reveal: the voxels past the front shrink away
	float r = uRevealStyle == 0 ? k : uRevealStyle == 1 ? Sat( length( g ) / 1.6 ) : uRevealStyle == 2 ? Sat( 0.5 + 0.7 * ( h.z + h.w - 1.0 ) ) : h.x;
	s *= Ease( ( uReveal * 1.08 - r ) / 0.08 );
	if( ( uHopSide == 0 && h.w < uHopE ) || ( uHopSide == 1 && h.w >= uHopE ) )
		s = 0.0;
}
)GLSL";

// 7. the proxies: one box per voxel (36 vertices), the look intersected in the fragment stage inside the voxel's own frame
static const char* kVoxDrawVS = R"GLSL(
uniform usamplerBuffer uList;  // unit 55: (cell | k << 24, owner)
uniform samplerBuffer uCapCol; // unit 46
uniform mat4 uWorldFromGrid, uVP;
uniform float uWorldScale;     // |uWorldFromGrid| (uniform scale)
uniform float uGap;
uniform vec3 uMeanCol;
uniform sampler2D uSolid; // unit 52: 1 per solid cell (the count atlas of a proxy list)
uniform int uCullInside;  // 1 = still voxels with all 6 neighbours solid cannot be seen: skip them
flat out vec3 vCenter, vColor;
flat out mat3 vRot;
flat out float vHalf;
out vec3 vWorld;
#ifdef PLAIN
flat out vec3 vNrm;       // the face's world normal (front faces, rasterized depth: early depth test)
const vec3 kFaceN[ 6 ] = vec3[ 6 ]( vec3( 0, 0, -1 ), vec3( 0, 0, 1 ), vec3( 0, -1, 0 ), vec3( 0, 1, 0 ), vec3( -1, 0, 0 ), vec3( 1, 0, 0 ) );
#endif
bool SolidAt( ivec3 c ) { return all( greaterThanEqual( c, ivec3( 0 ) ) ) && all( lessThan( c, ivec3( uRes ) ) ) && texelFetch( uSolid, CellPx( c ), 0 ).r > 0.5; }
const vec3 kBox[ 8 ] = vec3[ 8 ]( vec3( -1, -1, -1 ), vec3( 1, -1, -1 ), vec3( -1, 1, -1 ), vec3( 1, 1, -1 ), vec3( -1, -1, 1 ), vec3( 1, -1, 1 ),
                                   vec3( -1, 1, 1 ), vec3( 1, 1, 1 ) );
const int kIdx[ 36 ] = int[ 36 ]( 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 );
void main()
{
	uvec2 e = texelFetch( uList, gl_InstanceID ).xy;
	ivec3 c = ivec3( e.x & 255u, ( e.x >> 8 ) & 255u, ( e.x >> 16 ) & 255u );
#ifdef BLOCKS
	// W3 part sizes (a merged list): a block's origin cell carries its level; the proxy spans 2^level cells. A separate program, so the
	// x1 list draws through W2's exact shader
	uint ow = e.y;
	int lvl = 0;
	if( ow >= 0xFFFFFFF0u )
		lvl = ow == kMean ? 0 : int( ow - 0xFFFFFFF0u ), ow = kMean;
	else
		lvl = int( ow >> 28 ), ow &= 0x0FFFFFFFu;
	float L = float( 1 << lvl );
	vec3 g  = uGridMin + ( vec3( c ) + 0.5 * L ) * uCell;
#else
	vec3 g  = uGridMin + ( vec3( c ) + 0.5 ) * uCell;
#endif
	vec3 off;
	mat3 R;
	float s;
	Motion( c, g, VHash( c ), off, R, s );
	if( uCullInside == 1 && SolidAt( c + ivec3( 1, 0, 0 ) ) && SolidAt( c - ivec3( 1, 0, 0 ) ) && SolidAt( c + ivec3( 0, 1, 0 ) ) && SolidAt( c - ivec3( 0, 1, 0 ) ) &&
	    SolidAt( c + ivec3( 0, 0, 1 ) ) && SolidAt( c - ivec3( 0, 0, 1 ) ) )
		s = 0.0;
#ifdef BLOCKS
	float hs = 0.5 * uCell * L * ( 1.0 - uGap ) * s;
	vColor   = ow < kMean ? texelFetch( uCapCol, int( ow ) ).rgb : uMeanCol;
#else
	float hs = 0.5 * uCell * ( 1.0 - uGap ) * s;
	vColor   = e.y < kMean ? texelFetch( uCapCol, int( e.y ) ).rgb : uMeanCol;
#endif
	vec3 cw  = ( uWorldFromGrid * vec4( g + off, 1.0 ) ).xyz;
	mat3 Rw  = mat3( uWorldFromGrid ) * R / uWorldScale;
	vec3 w   = cw + Rw * ( kBox[ kIdx[ gl_VertexID ] ] * hs * uWorldScale );
	vCenter = cw, vRot = Rw, vHalf = max( hs * uWorldScale, 1e-9 ), vWorld = w;
#ifdef PLAIN
	vNrm = Rw * kFaceN[ gl_VertexID / 6 ];
#endif
	gl_Position = hs <= 1e-6 ? vec4( 2.0, 2.0, 2.0, 1.0 ) : uVP * vec4( w, 1.0 );
}
)GLSL";

// the shading of a voxel surface (shared by the proxies and the smooth surface): the key + fill lights, ambient, a soft sky, a spec;
// exposure + the tonemap + the encode of the model's own shader
static const char* kVoxShade = R"GLSL(
uniform vec3 uCamPos, uKeyDir, uKeyColor, uFillDir, uFillColor;
uniform float uAmbient, uExposure;
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
#ifdef SHADOW
// W4 A1: the SHADOW twins write what a shadow caster writes (the key shadow: the depth along the light; a spot light's: the distance)
uniform vec3 uShadowLightPos, uShadowDir;
uniform int uShadowOrtho;
vec4 Caster( vec3 p ) { return vec4( uShadowOrtho == 1 ? dot( p - uShadowLightPos, uShadowDir ) : length( p - uShadowLightPos ) ); }
#endif
vec4 Shade( vec3 p, vec3 N, vec3 albedo )
{
	vec3 V   = normalize( uCamPos - p );
	vec3 col = albedo * ( uAmbient + 0.18 * ( 0.6 + 0.4 * N.y ) );
	for( int li = 0; li < 2; ++li )
	{
		vec3 L  = li == 0 ? uKeyDir : uFillDir;
		vec3 lc = li == 0 ? uKeyColor : uFillColor;
		float d = max( dot( N, L ), 0.0 );
		col += lc * d * ( albedo / 3.14159265 + 0.24 * pow( max( dot( N, normalize( L + V ) ), 0.0 ), 48.0 ) );
	}
	col *= exp2( uExposure );
	return vec4( pow( max( Tonemap( col ), 0.0 ), vec3( 1.0 / 2.2 ) ), 1.0 );
}
)GLSL";

static const char* kVoxDrawFS = R"GLSL(
uniform mat4 uVP, uInvVP;
uniform vec2 uViewport;
uniform int uLook; // 1 Cubes 3 Rounded 4 Spheres 5 Frames 6 Bricks
uniform float uBevel;
flat in vec3 vCenter, vColor;
flat in mat3 vRot;
flat in float vHalf;
in vec3 vWorld;
out vec4 fragColor;
float SdRoundBox( vec3 p, vec3 b, float r )
{
	vec3 q = abs( p ) - b;
	return length( max( q, 0.0 ) ) + min( max( q.x, max( q.y, q.z ) ), 0.0 ) - r;
}
float SdBoxFrame( vec3 p, vec3 b, float e )
{
	p      = abs( p ) - b;
	vec3 q = abs( p + e ) - e;
	return min( min( length( max( vec3( p.x, q.y, q.z ), 0.0 ) ) + min( max( p.x, max( q.y, q.z ) ), 0.0 ),
	                 length( max( vec3( q.x, p.y, q.z ), 0.0 ) ) + min( max( q.x, max( p.y, q.z ) ), 0.0 ) ),
	            length( max( vec3( q.x, q.y, p.z ), 0.0 ) ) + min( max( q.x, max( q.y, p.z ) ), 0.0 ) );
}
float SdCyl( vec3 p, float r, float h )
{
	vec2 d = abs( vec2( length( p.xz ), p.y ) ) - vec2( r, h );
	return min( max( d.x, d.y ), 0.0 ) + length( max( d, 0.0 ) );
}
float Sd( vec3 p )
{
	if( uLook == 1 )
	{
		float r = 0.6 * uBevel;
		return SdRoundBox( p, vec3( 1.0 - r ), r );
	}
	if( uLook == 3 )
	{
		float r = 0.3 + 0.65 * uBevel;
		return SdRoundBox( p, vec3( 1.0 - r ), r );
	}
	if( uLook == 5 )
		return SdBoxFrame( p, vec3( 0.999 ), 0.1 + 0.3 * uBevel );
	// BRICKS: a block (rounded by Bevel) + a stud on top
	float r = 0.06 + 0.2 * uBevel;
	float b = SdRoundBox( p - vec3( 0.0, -0.2, 0.0 ), vec3( 1.0 - r, 0.8 - r, 1.0 - r ), r );
	float s = SdCyl( p - vec3( 0.0, 0.8, 0.0 ), 0.42 + 0.08 * uBevel, 0.16 ) - 0.03;
	return min( b, s );
}
void main()
{
	// the eye ray through this pixel, into the voxel's frame (the unit box)
	vec2 ndc = gl_FragCoord.xy / uViewport * 2.0 - 1.0;
	vec4 a   = uInvVP * vec4( ndc, -1.0, 1.0 ), b = uInvVP * vec4( ndc, 1.0, 1.0 );
	vec3 ro  = a.xyz / a.w, rd = normalize( b.xyz / b.w - ro );
	mat3 Ri  = transpose( vRot );
	vec3 o   = Ri * ( ro - vCenter ) / vHalf, d = Ri * rd;
	vec3 inv = 1.0 / ( d + vec3( 1e-12 ) );
	vec3 t0  = ( -1.0 - o ) * inv, t1 = ( 1.0 - o ) * inv;
	vec3 tmn = min( t0, t1 ), tmx = max( t0, t1 );
	float tn = max( max( tmn.x, tmn.y ), tmn.z ), tf = min( min( tmx.x, tmx.y ), tmx.z );
	if( tn > tf || tf < 0.0 )
		discard;
	tn      = max( tn, 0.0 );
	vec3 n  = vec3( 0.0, 1.0, 0.0 );
	float t = tn;
	if( uLook == 4 ) // SPHERE (radius 1)
	{
		float bb = dot( o, d ), cc = dot( o, o ) - 1.0, hh = bb * bb - cc;
		if( hh < 0.0 )
			discard;
		t = -bb - sqrt( hh );
		n = o + d * t;
	}
	else if( uLook == 1 && uBevel <= 0.0 ) // the plain cube: the box itself
		n = step( tmn.yzx, tmn.xyz ) * step( tmn.zxy, tmn.xyz ) * -sign( d );
	else
	{
		bool hit = false;
		for( int i = 0; i < 40; ++i )
		{
			float s = Sd( o + d * t );
			if( s < 1e-3 )
			{
				hit = true;
				break;
			}
			t += s;
			if( t > tf )
				break;
		}
		if( !hit )
			discard;
		vec3 p       = o + d * t;
		const vec2 k = vec2( 1.0, -1.0 );
		n = k.xyy * Sd( p + k.xyy * 1e-3 ) + k.yyx * Sd( p + k.yyx * 1e-3 ) + k.yxy * Sd( p + k.yxy * 1e-3 ) + k.xxx * Sd( p + k.xxx * 1e-3 );
	}
	vec3 pw      = vCenter + vRot * ( ( o + d * t ) * vHalf );
	vec3 N       = normalize( vRot * normalize( n ) );
	vec4 clip    = uVP * vec4( pw, 1.0 );
	gl_FragDepth = clip.z / clip.w * 0.5 + 0.5;
#ifdef SHADOW
	fragColor    = Caster( pw );
#else
	fragColor    = Shade( pw, N, vColor );
#endif
}
)GLSL";

// the plain cube (Cubes, Bevel 0): the proxy IS the voxel: no ray, no gl_FragDepth (the early depth test rejects hidden voxels)
static const char* kVoxPlainFS = R"GLSL(
flat in vec3 vColor, vNrm;
in vec3 vWorld;
out vec4 fragColor;
#ifdef SHADOW
void main() { fragColor = Caster( vWorld ); }
#else
void main() { fragColor = Shade( vWorld, normalize( vNrm ), vColor ); }
#endif
)GLSL";

// SMOOTH: marching cubes, 3 vertices per instance = one triangle of a cell's case, on the cell-centre lattice; normals from the field's
// gradient; each triangle moves with its cell (the cube draw's motion)
static const char* kVoxSmoothVS = R"GLSL(
uniform usamplerBuffer uList;  // unit 55
uniform samplerBuffer uCapCol; // unit 46
uniform sampler2D uField;      // unit 50
uniform usampler2D uOwnRes;    // unit 54
uniform mat4 uWorldFromGrid, uVP;
uniform float uIso;
uniform vec3 uMeanCol;
out vec3 vWorld, vNrm, vColor;
const ivec4 kTet[ 6 ] = ivec4[ 6 ]( ivec4( 0, 1, 3, 7 ), ivec4( 0, 3, 2, 7 ), ivec4( 0, 2, 6, 7 ), ivec4( 0, 6, 4, 7 ), ivec4( 0, 4, 5, 7 ), ivec4( 0, 5, 1, 7 ) );
float F( ivec3 c ) { return all( greaterThanEqual( c, ivec3( 0 ) ) ) && all( lessThan( c, ivec3( uRes ) ) ) ? texelFetch( uField, CellPx( c ), 0 ).r : 0.0; }
vec3 Grad( ivec3 c )
{
	return vec3( F( c + ivec3( 1, 0, 0 ) ) - F( c - ivec3( 1, 0, 0 ) ), F( c + ivec3( 0, 1, 0 ) ) - F( c - ivec3( 0, 1, 0 ) ),
	             F( c + ivec3( 0, 0, 1 ) ) - F( c - ivec3( 0, 0, 1 ) ) );
}
ivec3 Corner( int i ) { return ivec3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 ); }
uint OwnAt( ivec3 c ) { return texelFetch( uOwnRes, CellPx( clamp( c, ivec3( 0 ), ivec3( uRes - 1 ) ) ), 0 ).r; }
void main()
{
	uvec2 e = texelFetch( uList, gl_InstanceID ).xy;
	ivec3 c = ivec3( e.x & 255u, ( e.x >> 8 ) & 255u, ( e.x >> 16 ) & 255u );
	int k   = int( e.x >> 24 ); // the k-th triangle of the cell: walk the tetrahedra in order
	int ea  = 0, eb = 0;          // this vertex's edge (two corner indices)
	for( int t = 0; t < 6; ++t )
	{
		int v[ 4 ] = int[ 4 ]( kTet[ t ].x, kTet[ t ].y, kTet[ t ].z, kTet[ t ].w );
		int ins[ 4 ];
		int m = 0;
		for( int j = 0; j < 4; ++j )
		{
			ins[ j ] = F( c + Corner( v[ j ] ) ) > uIso ? 1 : 0;
			m += ins[ j ];
		}
		int n = m == 2 ? 2 : ( m == 1 || m == 3 ) ? 1 : 0;
		if( k >= n )
		{
			k -= n;
			continue;
		}
		if( n == 1 )
		{
			// the odd corner out (alone inside or alone outside) and its three edges
			int odd = 0;
			for( int j = 0; j < 4; ++j )
				if( ins[ j ] == ( m == 1 ? 1 : 0 ) )
					odd = j;
			int o3[ 3 ];
			int q = 0;
			for( int j = 0; j < 4; ++j )
				if( j != odd )
					o3[ q++ ] = j;
			ea = v[ odd ], eb = v[ o3[ gl_VertexID ] ];
		}
		else
		{
			// two in (a, b), two out (c, d): the quad (a c) (a d) (b d) (b c) as two triangles
			int a = -1, b = -1, cc = -1, d = -1;
			for( int j = 0; j < 4; ++j )
				if( ins[ j ] == 1 )
				{
					if( a < 0 ) a = j; else b = j;
				}
				else
				{
					if( cc < 0 ) cc = j; else d = j;
				}
			ivec2 quad[ 4 ] = ivec2[ 4 ]( ivec2( a, cc ), ivec2( a, d ), ivec2( b, d ), ivec2( b, cc ) );
			int qi          = k == 0 ? ( gl_VertexID == 0 ? 0 : gl_VertexID == 1 ? 1 : 2 ) : ( gl_VertexID == 0 ? 0 : gl_VertexID == 1 ? 2 : 3 );
			ea = v[ quad[ qi ].x ], eb = v[ quad[ qi ].y ];
		}
		break;
	}
	ivec3 ca = c + Corner( ea ), cb = c + Corner( eb );
	float fa = F( ca ), fb = F( cb );
	float u  = clamp( ( uIso - fa ) / ( fb - fa + 1e-9 ), 0.0, 1.0 );
	vec3 gv  = uGridMin + ( mix( vec3( ca ), vec3( cb ), u ) + 0.5 ) * uCell;
	vec3 n   = -mix( Grad( ca ), Grad( cb ), u );
	vec3 cc0 = uGridMin + ( vec3( c ) + 1.0 ) * uCell; // the dual cell's centre
	vec3 off;
	mat3 R;
	float s;
	Motion( c, cc0, VHash( c ), off, R, s );
	gv      = cc0 + off + R * ( gv - cc0 ) * s;
	n       = R * n;
	uint ow = OwnAt( fa > fb ? ca : cb );
	if( ow >= kMean )
		ow = OwnAt( fa > fb ? cb : ca );
	vColor  = ow < kMean ? texelFetch( uCapCol, int( ow ) ).rgb : uMeanCol;
	vWorld  = ( uWorldFromGrid * vec4( gv, 1.0 ) ).xyz;
	vNrm    = normalize( mat3( uWorldFromGrid ) * n + vec3( 0.0, 1e-9, 0.0 ) );
	gl_Position = s <= 1e-6 ? vec4( 2.0, 2.0, 2.0, 1.0 ) : uVP * vec4( vWorld, 1.0 );
}
)GLSL";
static const char* kVoxSmoothFS = R"GLSL(
in vec3 vWorld, vNrm, vColor;
out vec4 fragColor;
void main()
{
	// the field's gradient points out (the triangles' winding varies between tetrahedron cases: never flip by the facing)
#ifdef SHADOW
	fragColor = Caster( vWorld );
#else
	fragColor = Shade( vWorld, normalize( vNrm ), vColor );
#endif
}
)GLSL";
}
