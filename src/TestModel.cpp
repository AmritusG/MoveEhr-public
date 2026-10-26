// MoveEhr W5 (S1): Test Model (Debug group, last; default Off so a fresh instance still draws nothing, guard G4). A generated model
// for checking every feature without loading a file: no library, no file. The model is a binary glTF built HERE, in memory, and read
// by the normal glTF path (cgltf_parse from the bytes), so it is an ordinary ModelData: parts, skin, clips, everything downstream.
//   Cube: 24 vertices, three materials (the X / Y / Z face pairs: Parts can tell them apart), static.
//   Tube: W1's test tube (tools/gen_anim_models.py gen_tube, ported): a capped tube on a 3-joint chain, clips Bend (linear), Twist
//         (cubic spline), Wave (step). Its Anim Clip list fills as a loaded file's does; Anim Play Off = the bind pose (W1 rule).
#include "ModelLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mehr
{
namespace
{
constexpr int kFloat = 5126, kUbyte = 5121, kUint = 5125;

struct Glb
{
	std::vector< uint8_t > bin;
	std::string views, accessors;
	int nViews = 0, nAcc = 0;

	int View( const void* data, size_t bytes, int target )
	{
		while( bin.size() % 4 )
			bin.push_back( 0 );
		const size_t off = bin.size();
		bin.insert( bin.end(), (const uint8_t*)data, (const uint8_t*)data + bytes );
		views += std::string( nViews ? "," : "" ) + "{\"buffer\":0,\"byteOffset\":" + std::to_string( off ) + ",\"byteLength\":" + std::to_string( bytes ) +
		         ( target ? ",\"target\":" + std::to_string( target ) : "" ) + "}";
		return nViews++;
	}
	// n components per element; minmax for POSITION and animation inputs (the spec wants them)
	int Acc( const std::vector< float >& v, int n, const char* type, int target, bool minmax = false )
	{
		std::string mm;
		if( minmax )
		{
			std::string lo = "[", hi = "[";
			for( int k = 0; k < n; ++k )
			{
				float a = v[ k ], b = v[ k ];
				for( size_t i = k; i < v.size(); i += n )
					a = std::min( a, v[ i ] ), b = std::max( b, v[ i ] );
				lo += ( k ? "," : "" ) + Num( a ), hi += ( k ? "," : "" ) + Num( b );
			}
			mm = ",\"min\":" + lo + "],\"max\":" + hi + "]";
		}
		const int view = View( v.data(), v.size() * 4, target );
		accessors += std::string( nAcc ? "," : "" ) + "{\"bufferView\":" + std::to_string( view ) + ",\"componentType\":" + std::to_string( kFloat ) +
		             ",\"count\":" + std::to_string( v.size() / n ) + ",\"type\":\"" + type + "\"" + mm + "}";
		return nAcc++;
	}
	int AccU8( const std::vector< uint8_t >& v, int n, const char* type )
	{
		const int view = View( v.data(), v.size(), 34962 );
		accessors += std::string( nAcc ? "," : "" ) + "{\"bufferView\":" + std::to_string( view ) + ",\"componentType\":" + std::to_string( kUbyte ) +
		             ",\"count\":" + std::to_string( v.size() / n ) + ",\"type\":\"" + type + "\"}";
		return nAcc++;
	}
	int AccIdx( const std::vector< uint32_t >& v )
	{
		const int view = View( v.data(), v.size() * 4, 34963 );
		accessors += std::string( nAcc ? "," : "" ) + "{\"bufferView\":" + std::to_string( view ) + ",\"componentType\":" + std::to_string( kUint ) +
		             ",\"count\":" + std::to_string( v.size() ) + ",\"type\":\"SCALAR\"}";
		return nAcc++;
	}
	static std::string Num( float f )
	{
		char b[ 32 ];
		snprintf( b, sizeof( b ), "%.9g", f );
		return b;
	}
	std::vector< uint8_t > Write( const std::string& body ) // body: the JSON's other top-level members (no buffers / views / accessors)
	{
		while( bin.size() % 4 )
			bin.push_back( 0 );
		std::string js = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"MoveEhr Test Model\"}," + body + ",\"buffers\":[{\"byteLength\":" +
		                 std::to_string( bin.size() ) + "}],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
		while( js.size() % 4 )
			js += ' ';
		std::vector< uint8_t > out;
		auto u32 = [ & ]( uint32_t x ) {
			for( int k = 0; k < 4; ++k )
				out.push_back( uint8_t( x >> ( 8 * k ) ) );
		};
		u32( 0x46546C67 ), u32( 2 ), u32( uint32_t( 12 + 8 + js.size() + 8 + bin.size() ) );
		u32( uint32_t( js.size() ) ), u32( 0x4E4F534A );
		out.insert( out.end(), js.begin(), js.end() );
		u32( uint32_t( bin.size() ) ), u32( 0x004E4942 );
		out.insert( out.end(), bin.begin(), bin.end() );
		return out;
	}
};

std::vector< float > QuatAxis( float ax, float ay, float az, float deg )
{
	const float a = deg * 3.14159265f / 360.0f, s = std::sin( a ), l = std::sqrt( ax * ax + ay * ay + az * az );
	return { ax / l * s, ay / l * s, az / l * s, std::cos( a ) };
}

std::vector< uint8_t > Cube()
{
	Glb g;
	// per axis pair: 2 faces x 4 corners; normal along the axis, uv over the face
	std::vector< float > pos[ 3 ], nrm[ 3 ], uv[ 3 ];
	std::vector< uint32_t > idx[ 3 ];
	for( int ax = 0; ax < 3; ++ax )
		for( int sgn = -1; sgn <= 1; sgn += 2 )
		{
			const int u = ( ax + 1 ) % 3, v = ( ax + 2 ) % 3;
			const uint32_t base = uint32_t( pos[ ax ].size() / 3 );
			for( int c = 0; c < 4; ++c )
			{
				float p[ 3 ], n[ 3 ] = { 0, 0, 0 };
				const float cu = ( c == 1 || c == 2 ) ? 1.0f : -1.0f, cv = c >= 2 ? 1.0f : -1.0f;
				p[ ax ] = float( sgn ), p[ u ] = cu * float( sgn ), p[ v ] = cv;
				n[ ax ] = float( sgn );
				for( int k = 0; k < 3; ++k )
					pos[ ax ].push_back( p[ k ] * 0.5f ), nrm[ ax ].push_back( n[ k ] );
				uv[ ax ].push_back( ( cu + 1.0f ) * 0.5f ), uv[ ax ].push_back( 1.0f - ( cv + 1.0f ) * 0.5f );
			}
			for( uint32_t k : { 0u, 1u, 2u, 0u, 2u, 3u } )
				idx[ ax ].push_back( base + k );
		}
	std::string prims;
	for( int ax = 0; ax < 3; ++ax )
	{
		const int p = g.Acc( pos[ ax ], 3, "VEC3", 34962, true ), n = g.Acc( nrm[ ax ], 3, "VEC3", 34962 ), t = g.Acc( uv[ ax ], 2, "VEC2", 34962 );
		const int i = g.AccIdx( idx[ ax ] );
		prims += std::string( ax ? "," : "" ) + "{\"attributes\":{\"POSITION\":" + std::to_string( p ) + ",\"NORMAL\":" + std::to_string( n ) +
		         ",\"TEXCOORD_0\":" + std::to_string( t ) + "},\"indices\":" + std::to_string( i ) + ",\"material\":" + std::to_string( ax ) + "}";
	}
	const std::string body = "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"Test Cube\",\"mesh\":0}],"
	                         "\"meshes\":[{\"name\":\"Test Cube\",\"primitives\":[" + prims + "]}],"
	                         "\"materials\":[{\"name\":\"Cube X\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.85,0.3,0.25,1],\"metallicFactor\":0,\"roughnessFactor\":0.45}},"
	                         "{\"name\":\"Cube Y\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.35,0.8,0.35,1],\"metallicFactor\":0,\"roughnessFactor\":0.45}},"
	                         "{\"name\":\"Cube Z\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.3,0.45,0.9,1],\"metallicFactor\":0,\"roughnessFactor\":0.45}}]";
	return g.Write( body );
}

std::vector< uint8_t > Tube()
{
	Glb g;
	const int rings = 24, seg = 24;
	const float height = 2.0f, radius = 0.25f, kPi = 3.14159265f;
	std::vector< float > pos, nrm, uv, hgt;
	std::vector< uint32_t > idx;
	for( int r = 0; r <= rings; ++r )
	{
		const float y = height * r / rings;
		for( int s = 0; s <= seg; ++s )
		{
			const float a = 2 * kPi * s / seg;
			pos.insert( pos.end(), { radius * std::cos( a ), y - 1.0f, radius * std::sin( a ) } );
			nrm.insert( nrm.end(), { std::cos( a ), 0.0f, std::sin( a ) } );
			uv.insert( uv.end(), { float( s ) / seg, 1.0f - float( r ) / rings } );
			hgt.push_back( y );
		}
	}
	for( int r = 0; r < rings; ++r )
		for( int s = 0; s < seg; ++s )
		{
			const uint32_t a = r * ( seg + 1 ) + s, b = a + seg + 1;
			idx.insert( idx.end(), { a, b, a + 1, a + 1, b, b + 1 } );
		}
	for( int cap = 0; cap < 2; ++cap )
	{
		const float y = cap ? height : 0.0f, ny = cap ? 1.0f : -1.0f;
		const uint32_t c = uint32_t( pos.size() / 3 );
		pos.insert( pos.end(), { 0.0f, y - 1.0f, 0.0f } ), nrm.insert( nrm.end(), { 0.0f, ny, 0.0f } ), uv.insert( uv.end(), { 0.5f, 0.5f } ), hgt.push_back( y );
		for( int s = 0; s <= seg; ++s )
		{
			const float a = 2 * kPi * s / seg;
			pos.insert( pos.end(), { radius * std::cos( a ), y - 1.0f, radius * std::sin( a ) } ), nrm.insert( nrm.end(), { 0.0f, ny, 0.0f } );
			uv.insert( uv.end(), { 0.5f + 0.5f * std::cos( a ), 0.5f + 0.5f * std::sin( a ) } ), hgt.push_back( y );
		}
		for( int s = 0; s < seg; ++s )
			if( ny > 0 )
				idx.insert( idx.end(), { c, c + 2 + s, c + 1 + s } );
			else
				idx.insert( idx.end(), { c, c + 1 + s, c + 2 + s } );
	}
	// joints at bind y 0 / 0.7 / 1.4 (world - 1): a smooth blend between the two nearest (W1's chain_weights)
	const float jy[ 3 ] = { 0.0f, 0.7f, 1.4f };
	std::vector< uint8_t > J;
	std::vector< float > W;
	for( float y : hgt )
	{
		int k = 0;
		while( k + 1 < 3 && y > jy[ k + 1 ] )
			++k;
		if( k + 1 >= 3 )
		{
			J.insert( J.end(), { uint8_t( k ), 0, 0, 0 } ), W.insert( W.end(), { 1.0f, 0.0f, 0.0f, 0.0f } );
			continue;
		}
		float t = std::max( 0.0f, std::min( 1.0f, ( y - jy[ k ] ) / ( jy[ k + 1 ] - jy[ k ] ) ) );
		t       = t * t * ( 3 - 2 * t );
		J.insert( J.end(), { uint8_t( k ), uint8_t( k + 1 ), 0, 0 } ), W.insert( W.end(), { 1.0f - t, t, 0.0f, 0.0f } );
	}
	std::vector< float > ibm;
	for( float y : jy )
		ibm.insert( ibm.end(), { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.0f, 1.0f - y, 0.0f, 1 } );
	const int aP = g.Acc( pos, 3, "VEC3", 34962, true ), aN = g.Acc( nrm, 3, "VEC3", 34962 ), aT = g.Acc( uv, 2, "VEC2", 34962 );
	const int aJ = g.AccU8( J, 4, "VEC4" ), aW = g.Acc( W, 4, "VEC4", 34962 ), aI = g.AccIdx( idx ), aB = g.Acc( ibm, 16, "MAT4", 0 );
	// clips (nodes: 0 root, 1 j0, 2 j1, 3 j2, 4 mesh)
	std::string samplers[ 3 ], channels[ 3 ];
	int nSamp[ 3 ] = {};
	auto chan = [ & ]( int clip, int node, const char* path, const char* interp, const std::vector< float >& times, const std::vector< float >& vals, int n, const char* type ) {
		const int in = g.Acc( times, 1, "SCALAR", 0, true ), out = g.Acc( vals, n, type, 0 );
		const int si = nSamp[ clip ]++;
		samplers[ clip ] += std::string( si ? "," : "" ) + "{\"input\":" + std::to_string( in ) + ",\"output\":" + std::to_string( out ) + ",\"interpolation\":\"" + interp + "\"}";
		channels[ clip ] += std::string( si ? "," : "" ) + "{\"sampler\":" + std::to_string( si ) + ",\"target\":{\"node\":" + std::to_string( node ) + ",\"path\":\"" + path + "\"}}";
	};
	const std::vector< float > times = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };
	std::vector< float > bend, tw;
	for( float a : { 0.0f, 35.0f, 0.0f, -35.0f, 0.0f } )
	{
		auto q = QuatAxis( 0, 0, 1, a );
		bend.insert( bend.end(), q.begin(), q.end() );
	}
	for( float a : { 0.0f, 60.0f, 0.0f, -60.0f, 0.0f } )
	{
		const float r = a * kPi / 180.0f, k = 120.0f * kPi / 180.0f * 0.5f;
		const std::vector< float > t = { 0.0f, 0.5f * std::cos( r / 2 ) * k, 0.0f, -0.5f * std::sin( r / 2 ) * k };
		auto q = QuatAxis( 0, 1, 0, a );
		tw.insert( tw.end(), t.begin(), t.end() ), tw.insert( tw.end(), q.begin(), q.end() ), tw.insert( tw.end(), t.begin(), t.end() );
	}
	chan( 0, 2, "rotation", "LINEAR", times, bend, 4, "VEC4" );
	chan( 0, 3, "rotation", "LINEAR", times, bend, 4, "VEC4" );
	chan( 1, 2, "rotation", "CUBICSPLINE", times, tw, 4, "VEC4" );
	chan( 1, 3, "rotation", "CUBICSPLINE", times, tw, 4, "VEC4" );
	std::vector< float > st;
	for( float d : { 0.0f, 0.15f, 0.0f, -0.1f } )
		st.insert( st.end(), { 0.0f, 0.7f + d, 0.0f } );
	chan( 2, 3, "translation", "STEP", { 0.0f, 0.25f, 0.5f, 0.75f }, st, 3, "VEC3" );
	std::vector< float > wr = QuatAxis( 1, 0, 0, 20 ), w2 = QuatAxis( 1, 0, 0, -20 );
	wr.insert( wr.end(), w2.begin(), w2.end() );
	chan( 2, 2, "rotation", "STEP", { 0.0f, 0.5f }, wr, 4, "VEC4" );
	const char* names[ 3 ] = { "Bend", "Twist", "Wave" };
	std::string anims;
	for( int c = 0; c < 3; ++c )
		anims += std::string( c ? "," : "" ) + "{\"name\":\"" + names[ c ] + "\",\"samplers\":[" + samplers[ c ] + "],\"channels\":[" + channels[ c ] + "]}";
	const std::string body =
	    "\"scene\":0,\"scenes\":[{\"nodes\":[0,4]}],"
	    "\"nodes\":[{\"name\":\"Tube_root\",\"translation\":[0,-1,0],\"children\":[1]},{\"name\":\"Tube_j0\",\"translation\":[0,0,0],\"children\":[2]},"
	    "{\"name\":\"Tube_j1\",\"translation\":[0,0.7,0],\"children\":[3]},{\"name\":\"Tube_j2\",\"translation\":[0,0.7,0]},{\"name\":\"Tube_mesh\",\"mesh\":0,\"skin\":0}],"
	    "\"meshes\":[{\"name\":\"Tube\",\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string( aP ) + ",\"NORMAL\":" + std::to_string( aN ) +
	    ",\"TEXCOORD_0\":" + std::to_string( aT ) + ",\"JOINTS_0\":" + std::to_string( aJ ) + ",\"WEIGHTS_0\":" + std::to_string( aW ) + "},\"indices\":" +
	    std::to_string( aI ) + ",\"material\":0}]}],"
	    "\"skins\":[{\"joints\":[1,2,3],\"inverseBindMatrices\":" + std::to_string( aB ) + ",\"skeleton\":0}],"
	    "\"materials\":[{\"name\":\"Tube\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.85,0.35,0.2,1],\"metallicFactor\":0,\"roughnessFactor\":0.4}}],"
	    "\"animations\":[" + anims + "]";
	return g.Write( body );
}
}

bool IsTestModel( const std::string& path ) { return path.compare( 0, 5, "test:" ) == 0; }

std::vector< uint8_t > TestModelGlb( const std::string& path )
{
	if( path == "test:cube" )
		return Cube();
	if( path == "test:tube" )
		return Tube();
	return {};
}
}
