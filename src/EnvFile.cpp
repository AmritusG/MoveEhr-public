// MoveEhr W5 (S5): the HDRI file environment. Env Source "HDRI File" + Env File (a Radiance .hdr, read by the vendored stb_image:
// stbi_loadf = linear float radiance; only FORMAT=32-bit_rle_rgbe with the "-Y h +X w" layout, anything else is refused with a
// reason) + Env Backdrop. The equirect goes the Input env's way: one RGB16F texture with its full mip chain, roughness picks the
// level. The main-pass programs read it only through their ENVFILE twins (linked on demand, like the PTFX / ANIM twins), so every
// plain program compiles from the same tokens as before: Studio / Input / Off draw exactly what they drew. EXR: no permitted decoder
// is vendored, so .hdr only.
#include "Renderer.h"
#include "Log.h"
#include "Shaders.h"
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mehr
{
GLuint LinkPlainSrc( const std::string& vs, const std::string& fs, std::string& error, bool capture ); // Renderer.cpp
void CopySamplerUnitsExt( GLuint from, GLuint to );                                                   // Renderer.cpp

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kEnvFileUnit = 64; // GL 4.1 on Apple: 80 combined units; 64-79 were free
constexpr int kMaxEnvWidth = 4096;
}

std::shared_ptr< const EnvImage > LoadEnvImage( const std::string& path )
{
	auto img  = std::make_shared< EnvImage >();
	img->path = path;
	FILE* f   = std::fopen( path.c_str(), "rb" );
	if( !f )
	{
		img->error = "file not found";
		return img;
	}
	if( !stbi_is_hdr_from_file( f ) ) // rewinds
	{
		std::fclose( f );
		img->error = "not a Radiance .hdr file";
		return img;
	}
	// through callbacks that see the end of the file: stb zero-fills a read past it, so a truncated file would decode "fine" with a
	// black remainder; a read that finds nothing left while stb still wants bytes = truncated
	struct Reader
	{
		FILE* f;
		bool past = false;
	} rd{ f };
	stbi_io_callbacks cb;
	cb.read = []( void* u, char* data, int size ) {
		Reader* r   = static_cast< Reader* >( u );
		const int n = (int)std::fread( data, 1, size_t( size ), r->f );
		r->past     = r->past || ( n == 0 && size > 0 );
		return n;
	};
	cb.skip = []( void* u, int n ) { std::fseek( static_cast< Reader* >( u )->f, n, SEEK_CUR ); };
	cb.eof  = []( void* u ) { return std::feof( static_cast< Reader* >( u )->f ) ? 1 : 0; };
	int w = 0, h = 0, n = 0;
	float* d = stbi_loadf_from_callbacks( &cb, &rd, &w, &h, &n, 3 );
	std::fclose( f );
	if( d && rd.past )
	{
		stbi_image_free( d );
		img->error = "truncated .hdr (the file ends before its last pixel)";
		return img;
	}
	if( !d )
	{
		const char* why = stbi_failure_reason();
		img->error      = std::string( "unreadable .hdr (" ) + ( why ? why : "?" ) + ")";
		return img;
	}
	img->srcW = w, img->srcH = h;
	std::vector< float > px( d, d + size_t( w ) * h * 3 );
	stbi_image_free( d );
	// halve (2 x 2 box) until it fits: a 16k panorama would be 800 MB of floats on the GPU for nothing a reflection can show
	while( w > kMaxEnvWidth && h > 1 )
	{
		const int w2 = w / 2, h2 = h / 2;
		std::vector< float > q( size_t( w2 ) * h2 * 3 );
		for( int y = 0; y < h2; ++y )
			for( int x = 0; x < w2; ++x )
				for( int c = 0; c < 3; ++c )
				{
					auto at = [ & ]( int xx, int yy ) { return px[ ( size_t( yy ) * w + xx ) * 3 + c ]; };
					q[ ( size_t( y ) * w2 + x ) * 3 + c ] = 0.25f * ( at( 2 * x, 2 * y ) + at( 2 * x + 1, 2 * y ) + at( 2 * x, 2 * y + 1 ) + at( 2 * x + 1, 2 * y + 1 ) );
				}
		px.swap( q );
		w = w2, h = h2;
	}
	// non-finite texels (a corrupt RLE run can decode to huge exponents) would turn every mip above them to NaN
	for( float& v : px )
		if( !std::isfinite( v ) || v < 0.0f )
			v = 0.0f;
		else
			v = std::min( v, 65000.0f ); // half-float range
	img->w = w, img->h = h;
	img->rgb.swap( px );
	return img;
}

void Renderer::SetEnvImage( std::shared_ptr< const EnvImage > img )
{
	if( img == envImg )
		return;
	envImg = img;
	if( envFileTex )
		glDeleteTextures( 1, &envFileTex ), envFileTex = 0;
	envFileLevels = 0;
	if( !img || !img->error.empty() || img->rgb.empty() )
		return;
	const auto t0 = std::chrono::steady_clock::now();
	glGenTextures( 1, &envFileTex );
	glActiveTexture( GL_TEXTURE0 + kEnvFileUnit );
	glBindTexture( GL_TEXTURE_2D, envFileTex );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGB16F, img->w, img->h, 0, GL_RGB, GL_FLOAT, img->rgb.data() );
	glGenerateMipmap( GL_TEXTURE_2D );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT ); // the seam behind the camera
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	envFileLevels = 1 + (int)std::floor( std::log2( (double)std::max( img->w, img->h ) ) );
	Log( "ENV file %s: %dx%d (file %dx%d), %d levels, uploaded in %.0f ms", img->path.c_str(), img->w, img->h, img->srcW, img->srcH, envFileLevels,
	     std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
}

GLuint Renderer::EnvTwin( GLuint p )
{
	auto it = envTwins.find( p );
	if( it != envTwins.end() )
		return it->second;
	GLuint twin = p; // not a main-pass program (casters, capture): itself, it reads no env
	auto src    = progSrc.find( p );
	if( src != progSrc.end() && !src->second.capture )
	{
		const std::string& f = *src->second.fs;
		bool pass            = f.find( "vec3 Env( vec3 d, float r )" ) == std::string::npos;
		for( const char* d : { "#define SHADOW_PASS", "#define CRACK_PASS", "#define PROJ_PASS", "#define PHOTON_PASS" } )
			pass = pass || f.find( d ) != std::string::npos;
		if( !pass )
		{
			const auto t0        = std::chrono::steady_clock::now();
			const std::string vs = "#define ENVFILE\n" + *src->second.vs, fs = "#define ENVFILE\n" + f;
			std::string err;
			const GLuint q = LinkPlainSrc( vs, fs, err, false );
			if( q )
			{
				CopySamplerUnitsExt( p, q );
				glUseProgram( q );
				glUniform1i( U( q, "uEnvFile" ), kEnvFileUnit );
				glUseProgram( 0 );
				progSrc[ q ] = { std::make_shared< const std::string >( vs ), std::make_shared< const std::string >( fs ), false };
				twin         = q;
				Log( "ENV file twin of program %u linked in %.0f ms", p, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
			}
			else
				Log( "ENV file twin of program %u failed (Studio there): %s", p, err.c_str() );
		}
	}
	envTwins[ p ] = twin;
	return twin;
}

// once per frame before anything draws: the texture on its unit, with no host sampler object over it (Arena leaves samplers bound;
// a non-mipmap LINEAR one would make textureLod ignore the roughness level: MOVEEHR-005)
void Renderer::BindEnvFile( const FrameState& fs )
{
	if( fs.envSource != 3 || !envFileTex )
		return;
	glActiveTexture( GL_TEXTURE0 + kEnvFileUnit );
	glBindTexture( GL_TEXTURE_2D, envFileTex );
	glBindSampler( kEnvFileUnit, 0 );
	glActiveTexture( GL_TEXTURE0 );
}

void Renderer::DrawBackdrop( const FrameState& fs, int w, int h )
{
	if( fs.envSource != 3 || !envFileTex || fs.envBackdrop <= 0.0f )
		return;
	if( !progBackdrop )
	{
		std::string err;
		progBackdrop = LinkPlainSrc( kQuadVS, kBackdropFS, err, false );
		if( !progBackdrop )
		{
			Log( "ENV backdrop program failed: %s", err.c_str() );
			return;
		}
		glUseProgram( progBackdrop );
		glUniform1i( U( progBackdrop, "uEnvFile" ), kEnvFileUnit );
	}
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 inv = ( P * V ).Inverse();
	glUseProgram( progBackdrop );
	glUniformMatrix4fv( U( progBackdrop, "uInvViewProj" ), 1, GL_FALSE, inv.m );
	glUniform3fv( U( progBackdrop, "uCamPos" ), 1, fs.camPos );
	glUniform1f( U( progBackdrop, "uEnvRotate" ), fs.envRotate );
	glUniform1f( U( progBackdrop, "uExposure" ), fs.exposure );
	glUniform1f( U( progBackdrop, "uBackdrop" ), std::min( 1.0f, fs.envBackdrop ) );
	glUniform1i( U( progBackdrop, "uTonemap" ), fs.tonemap );
	BindEnvFile( fs );
	glViewport( 0, 0, w, h );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE_MINUS_DST_ALPHA, GL_ONE ); // under what is already there (premultiplied)
	glDisable( GL_DEPTH_TEST );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glDisable( GL_BLEND );
}

void Renderer::ReleaseEnvFile()
{
	for( const auto& t : envTwins )
		if( t.second != t.first )
			glDeleteProgram( t.second );
	envTwins.clear();
	if( progBackdrop )
		glDeleteProgram( progBackdrop ), progBackdrop = 0;
	if( envFileTex )
		glDeleteTextures( 1, &envFileTex ), envFileTex = 0;
	envImg.reset();
	envFileLevels = 0;
}
}
