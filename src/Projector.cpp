// W26 Stage 2: the projector in the haze. A cookie light: its 1024^2 RGBA32F texture holds, in the projector's own frustum,
// the shadow distance (a; the shadow programs write it to every channel) and the picture (rgb; kProjCookieFS with colour mask
// RGB, blurred by mip level for Focus). The beam = kProjMarchFS added onto the fog's march target; the light on the mask = the
// PROJ_PASS variants of the main programs, drawn additively over it (LEQUAL depth, a hair in front).
#include "Log.h"
#include "MathUtil.h"
#include "ProjectorShaders.h"
#include "Renderer.h"
#include "Shaders.h"
#include "SimShaders.h"
#include "VolumeShaders.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <CoreVideo/CoreVideo.h>

namespace mehr
{
namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }
constexpr int kProjRes      = 1024;
constexpr int kPLUnit[ 2 ]  = { 22, 23 }; // the cookie lights' textures in the PROJ_PASS programs
constexpr float kProjFar    = 60.0f;
}

bool Renderer::InitProjector( std::string& error )
{
	const std::string fs = std::string( "#define PROJ_PASS\n" ) + kLightLib + kMainFS;
	progProjMain     = Link( kMainVS, fs, error );
	progProjSoup     = Link( kSoupVS, fs, error );
	progProjSoupL    = Link( std::string( "#define LETTERS\n" ) + kSoupVS, fs, error );
	progProjCookie   = Link( kQuadVS, kProjCookieFS, error );
	progProjVideo    = Link( kQuadVS, kProjVideoFS, error ); // W2
	progProjMarch    = Link( kQuadVS, std::string( kMediumLib ) + kProjMarchFS, error );
	progProjMarchSim = Link( kQuadVS, std::string( kSimLib ) + kMediumSimLib + kProjMarchFS, error );
	if( !progProjMain || !progProjSoup || !progProjSoupL || !progProjCookie || !progProjMarch || !progProjMarchSim )
		return false;
	for( GLuint p : { progProjMain, progProjSoup, progProjSoupL } )
	{
		glUseProgram( p );
		char name[ 16 ];
		for( int i = 0; i < CH_COUNT; ++i )
		{
			snprintf( name, sizeof( name ), "uTex%d", i );
			glUniform1i( U( p, name ), i );
		}
		glUniform1i( U( p, "uInputTex" ), 8 );
		glUniform1i( U( p, "uIdxBuf" ), 9 );
		glUniform1i( U( p, "uVtxBuf" ), 10 );
		glUniform1i( U( p, "uPLTex0" ), kPLUnit[ 0 ] );
		glUniform1i( U( p, "uPLTex1" ), kPLUnit[ 1 ] );
		BindShadowSamplers( p );
	}
	glUseProgram( progProjCookie );
	glUniform1i( U( progProjCookie, "uImage" ), 0 );
	if( !progProjVideo )
		return false;
	glUseProgram( progProjVideo );
	glUniform1i( U( progProjVideo, "uRect" ), 0 );
	glUniform1i( U( progProjVideo, "uDxt" ), 1 );
	glUniform1i( U( progProjVideo, "uDxtA" ), 2 );
	glGenTextures( 1, &dummyRect ); // every declared sampler needs a texture of its type (Apple GL)
	glBindTexture( GL_TEXTURE_RECTANGLE, dummyRect );
	const uint8_t px[ 4 ] = { 255, 255, 255, 255 };
	glTexImage2D( GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px );
	glBindTexture( GL_TEXTURE_RECTANGLE, 0 );
	for( GLuint p : { progProjMarch, progProjMarchSim } )
	{
		glUseProgram( p );
		glUniform1i( U( p, "uDepth" ), 0 );
		glUniform1i( U( p, "uNoise" ), 1 );
		glUniform1i( U( p, "uSimDen" ), 2 );
		glUniform1i( U( p, "uProjTex" ), 3 );
		glUniform1i( U( p, "uPLTex" ), 5 );
	}
	glUseProgram( 0 );
	return true;
}

void Renderer::ReleaseProjector()
{
	for( GLuint* p : { &progProjMain, &progProjSoup, &progProjSoupL, &progProjCookie, &progProjMarch, &progProjMarchSim, &progProjVideo } )
		if( *p )
			glDeleteProgram( *p ), *p = 0;
	for( GLuint* t : { &projTex, &projImageTex, &projVideoRect, &projVideoDxt, &projVideoAlpha, &dummyRect } )
		if( *t )
			glDeleteTextures( 1, t ), *t = 0;
	if( projVideoFBO )
		glDeleteFramebuffers( 1, &projVideoFBO ), projVideoFBO = 0;
	projVideoHeld.reset();
	projImageW = projImageH = 0;
	projVideoDxtFmt = projVideoAlphaFmt = 0, projDxtW = projDxtH = 0;
	if( projFBO )
		glDeleteFramebuffers( 1, &projFBO ), projFBO = 0;
	if( projDepth )
		glDeleteRenderbuffers( 1, &projDepth ), projDepth = 0;
	projImageSerial = 0;
	projOk          = false;
}

// W2: a video frame -> projImageTex (<= 2048 px, all levels allocated; the mips are built on the GPU only when Focus needs them)
bool Renderer::UploadVideoFrame( const VideoFrame& f, bool mips, bool straight )
{
	const auto t0 = std::chrono::steady_clock::now();
	int w = f.w, h = f.h;
	while( std::max( w, h ) > 2048 )
		w = std::max( 1, w / 2 ), h = std::max( 1, h / 2 );
	if( !projImageTex || w != projImageW || h != projImageH )
	{
		if( !projImageTex )
			glGenTextures( 1, &projImageTex );
		glBindTexture( GL_TEXTURE_2D, projImageTex );
		const int n = 1 + (int)std::floor( std::log2( float( std::max( w, h ) ) ) );
		for( int l = 0; l < n; ++l )
			glTexImage2D( GL_TEXTURE_2D, l, GL_RGBA8, std::max( 1, w >> l ), std::max( 1, h >> l ), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1 );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		projImageW = w, projImageH = h, projImageLevels = n;
		if( !projVideoFBO )
			glGenFramebuffers( 1, &projVideoFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, projVideoFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, projImageTex, 0 );
	}
	int mode = 0;
	glActiveTexture( GL_TEXTURE0 );
	if( f.pixelBuffer )
	{
		// zero-copy: the decoder's IOSurface becomes the rectangle texture's storage
		IOSurfaceRef surf = CVPixelBufferGetIOSurface( (CVPixelBufferRef)f.pixelBuffer );
		if( !surf )
			return false;
		if( !projVideoRect )
			glGenTextures( 1, &projVideoRect );
		glBindTexture( GL_TEXTURE_RECTANGLE, projVideoRect );
		if( CGLTexImageIOSurface2D( CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA, (GLsizei)IOSurfaceGetWidth( surf ), (GLsizei)IOSurfaceGetHeight( surf ), GL_BGRA,
		                            GL_UNSIGNED_INT_8_8_8_8_REV, surf, 0 ) != kCGLNoError )
			return false;
		glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	else
	{
		// HAP / DXV (W3): the block planes straight into compressed textures (no CPU pixel decode), each defined once per size + format;
		// the textures are block-padded (HAP to 4 px, DXV to 16): uDxtFit crops the padding
		const int bw = f.tw, bh = f.th;
		const GLenum fmt = f.format == 1 ? 0x83F0 /* RGB S3TC DXT1 */ : f.format == 4 || f.format == 7 ? 0x8DBB /* RED RGTC1 */
		                 : f.format == 8 ? 0x8DBD /* RG RGTC2 */ : 0x83F3 /* RGBA S3TC DXT5 */;
		auto put = [ & ]( GLuint& tex, GLenum& texFmt, const std::vector< uint8_t >& data, GLenum format, int w4, int h4 ) {
			const GLsizei bytes = GLsizei( ( w4 / 4 ) * ( h4 / 4 ) * ( format == 0x83F0 || format == 0x8DBB ? 8 : 16 ) );
			if( !tex )
				glGenTextures( 1, &tex );
			glBindTexture( GL_TEXTURE_2D, tex );
			if( texFmt != format || projDxtW != bw || projDxtH != bh )
			{
				glCompressedTexImage2D( GL_TEXTURE_2D, 0, format, w4, h4, 0, bytes, data.data() );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
				texFmt = format;
			}
			else
				glCompressedTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, w4, h4, format, bytes, data.data() );
		};
		glActiveTexture( GL_TEXTURE1 );
		put( projVideoDxt, projVideoDxtFmt, f.blocks, fmt, bw, bh );
		if( !f.blocks2.empty() )
		{
			glActiveTexture( GL_TEXTURE2 );
			if( f.format >= 7 )
			{
				// DXV HQ: Co, Cg as BC5 at half size, sampled NEAREST (the DXV spec's chroma: one sample per 2 x 2 pixels). Linear chroma
				// smeared colour edges (testsrc2: max 107 of 255 vs DXVKit) and pulled the padding's chroma into the last row (97).
				put( projVideoAlpha, projVideoAlphaFmt, f.blocks2, 0x8DBD, bw / 2, bh / 2 );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST ), glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			}
			else
				put( projVideoAlpha, projVideoAlphaFmt, f.blocks2, 0x8DBB, bw, bh );
		}
		projDxtW = bw, projDxtH = bh;
		mode     = f.format == 6 ? ( f.blocks2.empty() ? 2 : 3 ) : f.format == 4 ? 4 : f.format == 7 ? 5 : f.format == 8 ? 6 : 1;
		glActiveTexture( GL_TEXTURE0 );
	}
	// bind every sampler with a texture of its type, draw the frame into the picture
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_RECTANGLE, mode == 0 ? projVideoRect : dummyRect );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, mode != 0 && projVideoDxt ? projVideoDxt : whiteTex );
	glActiveTexture( GL_TEXTURE2 );
	glBindTexture( GL_TEXTURE_2D, ( mode == 3 || mode >= 5 ) && projVideoAlpha ? projVideoAlpha : whiteTex );
	glBindFramebuffer( GL_FRAMEBUFFER, projVideoFBO );
	glViewport( 0, 0, w, h );
	glDisable( GL_BLEND );
	glDisable( GL_DEPTH_TEST );
	glUseProgram( progProjVideo );
	glUniform1i( U( progProjVideo, "uMode" ), mode );
	glUniform1i( U( progProjVideo, "uStraight" ), straight ? 1 : 0 );
	glUniform2f( U( progProjVideo, "uSize" ), float( f.w ), float( f.h ) );
	glUniform2f( U( progProjVideo, "uDxtFit" ), mode ? float( f.w ) / float( std::max( 1, projDxtW ) ) : 1.0f, mode ? float( f.h ) / float( std::max( 1, projDxtH ) ) : 1.0f );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindVertexArray( 0 );
	// W3 proof (debug only): LOADEHR_VIDEO_DUMP=<folder/> writes each new picture as raw RGBA8 rows bottom-up, <frame>_<w>x<h>.rgba
	static const char* dump = getenv( "MOVEEHR_VIDEO_DUMP" );
	if( dump && f.serial != projImageSerial )
	{
		std::vector< uint8_t > px( size_t( w ) * h * 4 );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data() );
		char name[ 1024 ];
		static const bool seq = getenv( "MOVEEHR_VIDEO_DUMP_SEQ" ) != nullptr; // W4: <sequence>_<frame>_<w>x<h>.rgba (the order shown)
		static int dumpSeq    = 0;
		if( seq )
			snprintf( name, sizeof( name ), "%s%06d_%d_%dx%d.rgba", dump, dumpSeq++, f.index, w, h );
		else
			snprintf( name, sizeof( name ), "%s%d_%dx%d.rgba", dump, f.index, w, h );
		if( FILE* o = fopen( name, "wb" ) )
			fwrite( px.data(), 1, px.size(), o ), fclose( o );
	}
	for( int u = 2; u >= 0; --u )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBindTexture( GL_TEXTURE_RECTANGLE, 0 );
	}
	projMipsValid = false;
	if( mips )
	{
		glBindTexture( GL_TEXTURE_2D, projImageTex );
		glGenerateMipmap( GL_TEXTURE_2D ); // only while Focus > 0 (the blur reads the upper levels)
		glBindTexture( GL_TEXTURE_2D, 0 );
		projMipsValid = true;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	videoUploadMs = std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count();
	return true;
}

// the projector's frustum (its aspect = the picture's), shadow distance, then the picture
void Renderer::RenderProjector( const FrameState& fs, GpuModel* model )
{
	const ProjState& pr = fs.projector;
	projOk              = false;
	if( !pr.on )
		return;
	// the picture: the FX input (the mip-mapped copy) or the image file (uploaded once per decode, mips from the loader)
	GLuint img  = 0;
	int iw = 0, ih = 0, levels = 1;
	float scale[ 2 ] = { 1, 1 };
	if( pr.source == 1 && pr.video )
	{
		// W2: a video file. A new frame is drawn into the picture; the mips only while Focus blurs it
		const bool mips = pr.focus > 0.0f;
		if( projImageSerial != pr.video->serial || ( mips && !projMipsValid ) || projVideoStraight != pr.videoStraight )
		{
			if( UploadVideoFrame( *pr.video, mips, pr.videoStraight ) )
				projVideoHeld = pr.video;
			projImageSerial = pr.video->serial, projVideoStraight = pr.videoStraight;
		}
		if( projVideoHeld )
			img = projImageTex, iw = projVideoHeld->w, ih = projVideoHeld->h, levels = mips ? projImageLevels : 1;
	}
	else if( pr.source == 1 && pr.videoKeep && projVideoHeld )
		img = projImageTex, iw = projVideoHeld->w, ih = projVideoHeld->h, levels = projMipsValid && pr.focus > 0.0f ? projImageLevels : 1;
	else if( pr.source == 1 && pr.image && !pr.image->mips.empty() )
	{
		if( projImageSerial != pr.image->serial )
		{
			if( !projImageTex )
				glGenTextures( 1, &projImageTex );
			glBindTexture( GL_TEXTURE_2D, projImageTex );
			const int n = (int)pr.image->mips.size();
			for( int l = 0; l < n; ++l )
				glTexImage2D( GL_TEXTURE_2D, l, GL_RGBA8, std::max( 1, pr.image->w >> l ), std::max( 1, pr.image->h >> l ), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
			for( int l = 0; l < n; ++l )
				glTexSubImage2D( GL_TEXTURE_2D, l, 0, 0, std::max( 1, pr.image->w >> l ), std::max( 1, pr.image->h >> l ), GL_RGBA, GL_UNSIGNED_BYTE, pr.image->mips[ l ].data() );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1 );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			glBindTexture( GL_TEXTURE_2D, 0 );
			projImageSerial = pr.image->serial;
			projImageLevels = n;
			projImageW = projImageH = 0; // the next video frame re-allocates its picture
			projVideoHeld.reset();
		}
		img = projImageTex, iw = pr.image->w, ih = pr.image->h, levels = projImageLevels;
	}
	else if( pr.source == 0 && fs.inputTex && inTex )
		img = inTex, iw = inW, ih = inH, levels = inLevels; // already scaled into the copy
	const float aspect = iw > 0 && ih > 0 ? float( iw ) / float( ih ) : 16.0f / 9.0f;
	const float tx     = std::tan( std::min( 170.0f, std::max( 1.0f, pr.fovX ) ) * 0.5f * 3.14159265f / 180.0f );
	const float fovY   = 2.0f * std::atan( tx / aspect );
	float at[ 3 ]      = { pr.pos[ 0 ] + pr.dir[ 0 ], pr.pos[ 1 ] + pr.dir[ 1 ], pr.pos[ 2 ] + pr.dir[ 2 ] };
	float up[ 3 ]      = { 0, 1, 0 };
	if( std::fabs( pr.dir[ 1 ] ) > 0.95f )
		up[ 1 ] = 0, up[ 2 ] = -1;
	const Mat4 vp = Mat4::Perspective( fovY, aspect, 0.05f, kProjFar ) * Mat4::LookAt( pr.pos, at, up );
	std::memcpy( projVP, vp.m, sizeof( projVP ) );

	if( !projTex )
	{
		glGenTextures( 1, &projTex );
		glBindTexture( GL_TEXTURE_2D, projTex );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kProjRes, kProjRes, 0, GL_RGBA, GL_FLOAT, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenRenderbuffers( 1, &projDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, projDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kProjRes, kProjRes );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		glGenFramebuffers( 1, &projFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, projFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, projTex, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, projDepth );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, projFBO );
	glViewport( 0, 0, kProjRes, kProjRes );
	glClearColor( 1e6f, 1e6f, 1e6f, 1e6f );
	glClearDepth( 1.0 );
	glDepthMask( GL_TRUE );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glClearColor( 0, 0, 0, 0 );
	if( model )
	{
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
		DrawCasters( fs, model, vp.m, pr.pos, true );
		glDisable( GL_DEPTH_TEST );
	}
	// the picture into rgb
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE );
	glUseProgram( progProjCookie );
	glUniform1i( U( progProjCookie, "uHaveImage" ), img ? 1 : 0 );
	glUniform2f( U( progProjCookie, "uImageScale" ), scale[ 0 ], scale[ 1 ] );
	glUniform1f( U( progProjCookie, "uLod" ), pr.focus * float( std::max( 0, levels - 1 ) ) * 0.6f );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, img ? img : whiteTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	if( getenv( "MOVEEHR_PROJ_DEBUG" ) )
	{
		std::vector< float > px( size_t( kProjRes ) * kProjRes * 4 );
		glReadPixels( 0, 0, kProjRes, kProjRes, GL_RGBA, GL_FLOAT, px.data() );
		float mn = 1e9f;
		size_t near = 0;
		for( size_t i = 0; i < px.size(); i += 4 )
			mn = std::min( mn, px[ i + 3 ] ), near += px[ i + 3 ] < 1e5f;
		Log( "PROJ shadow: min distance %.3f, %zu of %d texels see the mask; pos (%.2f %.2f %.2f) dir (%.2f %.2f %.2f)", mn, near, kProjRes * kProjRes, pr.pos[ 0 ], pr.pos[ 1 ], pr.pos[ 2 ],
		     pr.dir[ 0 ], pr.dir[ 1 ], pr.dir[ 2 ] );
	}
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	projOk = true;
}

// uniforms + textures of the PROJ_PASS programs' cookie lights (the projector is light 0)
void Renderer::BindCookieLights( GLuint p, const FrameState& fs )
{
	int n = 0;
	float vp[ 2 * 16 ] = {}, pos[ 6 ] = {}, col[ 6 ] = {};
	GLuint tex[ 2 ] = { whiteTex, whiteTex };
	if( projOk && fs.projector.surface > 0.0f )
	{
		std::memcpy( vp, projVP, sizeof( projVP ) );
		for( int k = 0; k < 3; ++k )
			pos[ k ] = fs.projector.pos[ k ], col[ k ] = fs.projector.color[ k ] * fs.projector.surface;
		tex[ n++ ] = projTex;
	}
	if( causticOk && fs.caustic.surface > 0.0f ) // W26 Stage 8: the caustic map, as a second cookie light
	{
		std::memcpy( vp + n * 16, causticVP, sizeof( causticVP ) );
		for( int k = 0; k < 3; ++k )
			pos[ n * 3 + k ] = fs.caustic.pos[ k ], col[ n * 3 + k ] = fs.caustic.color[ k ] * fs.caustic.surface * fs.caustic.gain;
		tex[ n++ ] = causticTex;
	}
	const GLint exact[ 2 ] = { tex[ 0 ] == causticTex && n > 0 ? 1 : 0, tex[ 1 ] == causticTex && n > 1 ? 1 : 0 };
	glUniform1iv( U( p, "uPLExact" ), 2, exact );
	glUniform1i( U( p, "uPLCount" ), n );
	glUniformMatrix4fv( U( p, "uPLVP" ), 2, GL_FALSE, vp );
	glUniform3fv( U( p, "uPLPos" ), 2, pos );
	glUniform3fv( U( p, "uPLColor" ), 2, col );
	glActiveTexture( GL_TEXTURE0 + kPLUnit[ 0 ] );
	glBindTexture( GL_TEXTURE_2D, tex[ 0 ] );
	glActiveTexture( GL_TEXTURE0 + kPLUnit[ 1 ] );
	glBindTexture( GL_TEXTURE_2D, tex[ 1 ] );
	glActiveTexture( GL_TEXTURE0 );
}

// the projected light on the mask: the same geometry and material through the PROJ_PASS programs, added (alpha untouched)
void Renderer::DrawProjSurface( const FrameState& fs, GpuModel* model )
{
	if( !( ( projOk && fs.projector.surface > 0.0f ) || ( causticOk && fs.caustic.surface > 0.0f ) ) || !model )
		return;
	const bool soup = fs.wireMix > 0.0f || fs.explode > 0.0f || fs.shape.on;
	const GLuint p  = soup ? progProjSoup : progProjMain;
	glUseProgram( p );
	BindCookieLights( p, fs );
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_FALSE );
	glEnable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( -1.0f, -4.0f );
	FrameState f  = fs;
	f.fogDensity = 0.0f; // light only: Depth Fog would mix the background in
	DrawModel( p, f, model, false, soup, 0, nullptr, nullptr, nullptr );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glPolygonOffset( 0.0f, 0.0f );
	glDepthMask( GL_TRUE );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	for( int u : kPLUnit )
	{
		glActiveTexture( GL_TEXTURE0 + u );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	glActiveTexture( GL_TEXTURE0 );
}

// into the bound march target (attachment 0 only, rgb added): the medium lit by the picture, shadowed by the mask; and the
// caustic map's light the same way (W26 Stage 8)
void Renderer::DrawProjMarch( const FrameState& fs, bool sim )
{
	if( projOk && fs.projector.beam > 0.0f )
	{
		float c[ 3 ];
		for( int k = 0; k < 3; ++k )
			c[ k ] = fs.projector.color[ k ] * fs.projector.beam;
		DrawCookieMarch( fs, sim, projTex, projVP, fs.projector.pos, c );
	}
	if( causticOk && fs.caustic.smoke > 0.0f )
	{
		float c[ 3 ];
		for( int k = 0; k < 3; ++k )
			c[ k ] = fs.caustic.color[ k ] * fs.caustic.smoke * fs.caustic.gain;
		DrawCookieMarch( fs, sim, causticTex, causticVP, fs.caustic.pos, c );
	}
}

void Renderer::DrawCookieMarch( const FrameState& fs, bool sim, GLuint tex, const float* lvp, const float* lpos, const float* color )
{
	Mat4 V, P;
	std::memcpy( V.m, fs.view, sizeof( V.m ) );
	std::memcpy( P.m, fs.proj, sizeof( P.m ) );
	const Mat4 VP = P * V, inv = VP.Inverse();
	const GLuint p = sim ? progProjMarchSim : progProjMarch;
	glUseProgram( p );
	SetMediumUniforms( p, fs );
	if( sim )
		SetSimMediumUniforms( p, fs, VP );
	else
	{
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, whiteTex );
		glActiveTexture( GL_TEXTURE3 );
		glBindTexture( GL_TEXTURE_2D, whiteTex );
	}
	glUniformMatrix4fv( U( p, "uInvViewProj" ), 1, GL_FALSE, inv.m );
	glUniform3fv( U( p, "uCamPos" ), 1, fs.camPos );
	glUniform1i( U( p, "uSteps" ), fs.vol.steps );
	glUniform1f( U( p, "uFrame" ), float( frameCounter % 64 ) );
	glUniformMatrix4fv( U( p, "uPLVP" ), 1, GL_FALSE, lvp );
	glUniform3fv( U( p, "uPLPos" ), 1, lpos );
	glUniform3fv( U( p, "uPLColor" ), 1, color );
	glUniform1f( U( p, "uPLFar" ), std::min( kProjFar, 2.0f * fs.vol.radius + 30.0f ) );
	glActiveTexture( GL_TEXTURE5 );
	glBindTexture( GL_TEXTURE_2D, tex );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_3D, noiseTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, sceneDepthTex );
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	glActiveTexture( GL_TEXTURE5 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
}
}
