// W5: the wireframe. Lines come from the triangle-soup stage's barycentrics (the W1 wire); everything past the W1 look draws through
// the WIRE twins of the main-pass soup programs (Renderer::Init links them; the plain programs are untouched):
//   Wire Mode Over Surface / Wire Only / Wire + Dark Fill x Wire Back Hidden / X-Ray = a fixed little set of draws (DrawWired);
//   Wire Edges Feature = per-triangle edge flags built off the render thread (shared-edge crease > 20 deg, or an open / non-manifold
//   edge), an R8UI texture buffer on unit 25 read by the soup stage;
//   Wire Glow / Wire Bloom = the lines drawn again into an HDR wire buffer (own depth: the solid's depth first, so hidden lines stay
//   hidden), blurred at half size for the halo, both added over the composite as light at alpha 0 (W23).
#include "Renderer.h"
#include "Log.h"
#include "Shaders.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace mehr
{
GLuint Link( const std::string& vs, const std::string& fs, std::string& error );

namespace
{
inline GLint U( GLuint p, const char* n ) { return glGetUniformLocation( p, n ); }

// glow + halo over the composite: light (rgb added, alpha 0)
const char* kWireAddFS = R"GLSL(
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uGlowTex;
uniform sampler2D uHaloTex;
uniform float uGlow, uHalo;
void main()
{
	fragColor = vec4( texture( uGlowTex, vUV ).rgb * uGlow + texture( uHaloTex, vUV ).rgb * uHalo, 0.0 );
}
)GLSL";
}

// Vertices welded by position (loaders split them by normal / uv): the welded id per vertex. W6: the points share it.
std::vector< uint32_t > WeldByPosition( const CpuGeometry& g )
{
	const size_t nv = g.pos.size() / 3;
	float lo[ 3 ] = { 1e30f, 1e30f, 1e30f }, hi[ 3 ] = { -1e30f, -1e30f, -1e30f };
	for( size_t v = 0; v < nv; ++v )
		for( int k = 0; k < 3; ++k )
			lo[ k ] = std::min( lo[ k ], g.pos[ v * 3 + k ] ), hi[ k ] = std::max( hi[ k ], g.pos[ v * 3 + k ] );
	const float ext = std::max( { hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ], hi[ 2 ] - lo[ 2 ], 1e-12f } );
	std::unordered_map< uint64_t, uint32_t > weldOf;
	weldOf.reserve( nv );
	std::vector< uint32_t > weld( nv );
	for( size_t v = 0; v < nv; ++v )
	{
		uint64_t key = 0;
		for( int k = 0; k < 3; ++k )
			key = ( key << 21 ) | uint64_t( std::lround( ( g.pos[ v * 3 + k ] - lo[ k ] ) / ext * 2097150.0 ) & 0x1FFFFF );
		weld[ v ] = weldOf.emplace( key, uint32_t( weldOf.size() ) ).first->second;
	}
	return weld;
}

// Feature edges of a mesh in GPU index order (the whole mesh, then the fractured copy per piece: each piece keeps its own edges).
// An edge draws when it is open (one face), shared by more than two faces, or its two faces bend more than 20 degrees. Bit k = the
// edge opposite corner k. W6: the Feature Vertices points use it too.
void BuildEdgeFlags( const CpuGeometry& g, std::vector< uint8_t >& out )
{
	const std::vector< uint32_t > weld = WeldByPosition( g );
	const size_t nw = g.idx.size() / 3, nt = nw + g.fracIdx.size() / 3;
	out.assign( nt, 0 );
	auto corner = [ & ]( size_t t, int c ) { return t < nw ? g.idx[ t * 3 + c ] : g.fracIdx[ ( t - nw ) * 3 + c ]; };
	std::vector< float > nrm( nt * 3, 0.0f );
	for( size_t t = 0; t < nt; ++t )
	{
		const float* a = &g.pos[ corner( t, 0 ) * 3 ];
		const float* b = &g.pos[ corner( t, 1 ) * 3 ];
		const float* c = &g.pos[ corner( t, 2 ) * 3 ];
		const float u[ 3 ] = { b[ 0 ] - a[ 0 ], b[ 1 ] - a[ 1 ], b[ 2 ] - a[ 2 ] }, w[ 3 ] = { c[ 0 ] - a[ 0 ], c[ 1 ] - a[ 1 ], c[ 2 ] - a[ 2 ] };
		float n[ 3 ] = { u[ 1 ] * w[ 2 ] - u[ 2 ] * w[ 1 ], u[ 2 ] * w[ 0 ] - u[ 0 ] * w[ 2 ], u[ 0 ] * w[ 1 ] - u[ 1 ] * w[ 0 ] };
		const float l = std::sqrt( n[ 0 ] * n[ 0 ] + n[ 1 ] * n[ 1 ] + n[ 2 ] * n[ 2 ] );
		for( int k = 0; k < 3; ++k )
			nrm[ t * 3 + k ] = l > 0.0f ? n[ k ] / l : 0.0f;
	}
	struct Edge
	{
		uint32_t t0 = 0, t1 = 0;
		uint8_t k0 = 0, k1 = 0, n = 0;
	};
	std::unordered_map< uint64_t, Edge > edges;
	edges.reserve( nt * 2 );
	for( size_t t = 0; t < nt; ++t )
	{
		const uint64_t region = t < nw ? 0 : 1 + ( t - nw < g.fracPiece.size() ? g.fracPiece[ t - nw ] : 0 );
		for( int k = 0; k < 3; ++k )
		{
			uint32_t a = weld[ corner( t, ( k + 1 ) % 3 ) ], b = weld[ corner( t, ( k + 2 ) % 3 ) ];
			if( a > b )
				std::swap( a, b );
			// 2 x 24 bits of welded vertex + 16 of region; a mesh past 16M welded vertices folds keys (an extra line at worst)
			const uint64_t key = ( uint64_t( a & 0xFFFFFF ) << 40 ) | ( uint64_t( b & 0xFFFFFF ) << 16 ) | ( region & 0xFFFF );
			Edge& e = edges[ key ];
			if( e.n == 0 )
				e.t0 = uint32_t( t ), e.k0 = uint8_t( k );
			else if( e.n == 1 )
				e.t1 = uint32_t( t ), e.k1 = uint8_t( k );
			e.n = uint8_t( std::min( 3, e.n + 1 ) );
		}
	}
	const float crease = std::cos( 20.0f * 3.14159265f / 180.0f );
	for( const auto& kv : edges )
	{
		const Edge& e = kv.second;
		bool draw     = e.n != 2;
		if( !draw )
		{
			const float* a = &nrm[ e.t0 * 3 ];
			const float* b = &nrm[ e.t1 * 3 ];
			draw           = a[ 0 ] * b[ 0 ] + a[ 1 ] * b[ 1 ] + a[ 2 ] * b[ 2 ] < crease;
		}
		if( !draw )
			continue;
		out[ e.t0 ] |= uint8_t( 1u << e.k0 );
		if( e.n >= 2 )
			out[ e.t1 ] |= uint8_t( 1u << e.k1 );
	}
}

GLuint Renderer::WireTwin( GLuint p ) const
{
	for( const auto& t : wireTwins )
		if( t.first == p )
			return t.second;
	return p;
}

bool Renderer::InitWire( std::string& error )
{
	progWireAdd = Link( kQuadVS, kWireAddFS, error );
	if( !progWireAdd )
		return false;
	glUseProgram( progWireAdd );
	glUniform1i( U( progWireAdd, "uGlowTex" ), 0 );
	glUniform1i( U( progWireAdd, "uHaloTex" ), 1 );
	glUseProgram( 0 );
	const uint8_t all = 7; // every edge
	glGenBuffers( 1, &wireDummyEdgeBuf );
	glBindBuffer( GL_TEXTURE_BUFFER, wireDummyEdgeBuf );
	glBufferData( GL_TEXTURE_BUFFER, 1, &all, GL_STATIC_DRAW );
	glGenTextures( 1, &wireDummyEdge );
	glBindTexture( GL_TEXTURE_BUFFER, wireDummyEdge );
	glTexBuffer( GL_TEXTURE_BUFFER, GL_R8UI, wireDummyEdgeBuf );
	glBindTexture( GL_TEXTURE_BUFFER, 0 );
	glBindBuffer( GL_TEXTURE_BUFFER, 0 );
	return true;
}

void Renderer::ReleaseWire()
{
	auto delTex = []( GLuint& t ) { if( t ) glDeleteTextures( 1, &t ), t = 0; };
	auto delBuf = []( GLuint& b ) { if( b ) glDeleteBuffers( 1, &b ), b = 0; };
	auto delFbo = []( GLuint& f ) { if( f ) glDeleteFramebuffers( 1, &f ), f = 0; };
	for( WireEdges& e : wireEdges )
	{
		delTex( e.tex ), delBuf( e.buf );
		e = WireEdges();
	}
	delTex( wireDummyEdge ), delBuf( wireDummyEdgeBuf ), delTex( wireTex ), delFbo( wireFBO );
	for( int i = 0; i < 2; ++i )
		delTex( wireBlur[ i ] ), delFbo( wireBlurFBO[ i ] );
	if( wireDepth )
		glDeleteRenderbuffers( 1, &wireDepth ), wireDepth = 0;
	if( progWireAdd )
		glDeleteProgram( progWireAdd ), progWireAdd = 0;
	wireW = wireH = 0;
}

GLuint Renderer::WireEdgeTex( GpuModel* model, bool want )
{
	if( !want || !model || !model->geometry )
		return wireDummyEdge;
	WireEdges* e = nullptr;
	for( WireEdges& x : wireEdges )
		if( x.serial == model->serial )
			e = &x;
	if( !e )
	{
		// a free entry, else the one built longest ago (entries are filled in order)
		static unsigned next = 0;
		e = &wireEdges[ next++ % 4 ];
		if( e->tex )
			glDeleteTextures( 1, &e->tex ), e->tex = 0;
		if( e->buf )
			glDeleteBuffers( 1, &e->buf ), e->buf = 0;
		*e         = WireEdges();
		e->serial  = model->serial;
		e->job     = std::make_shared< std::vector< uint8_t > >();
		e->done    = std::make_shared< std::atomic< bool > >( false );
		auto geo   = model->geometry;
		auto out   = e->job;
		auto done  = e->done;
		std::thread( [ geo, out, done ]() {
			const auto t0 = std::chrono::steady_clock::now();
			BuildEdgeFlags( *geo, *out );
			size_t lines = 0;
			for( uint8_t m : *out )
				lines += ( m & 1 ) + ( ( m >> 1 ) & 1 ) + ( ( m >> 2 ) & 1 );
			Log( "WIRE edges: %zu triangles, %zu of %zu triangle edges drawn as feature edges, %.0f ms (worker)", out->size(), lines, out->size() * 3,
			     std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - t0 ).count() );
			done->store( true );
		} ).detach();
	}
	if( !e->ready && e->done && e->done->load() )
	{
		glGenBuffers( 1, &e->buf );
		glBindBuffer( GL_TEXTURE_BUFFER, e->buf );
		glBufferData( GL_TEXTURE_BUFFER, std::max< size_t >( 1, e->job->size() ), e->job->empty() ? nullptr : e->job->data(), GL_STATIC_DRAW );
		glGenTextures( 1, &e->tex );
		glBindTexture( GL_TEXTURE_BUFFER, e->tex );
		glTexBuffer( GL_TEXTURE_BUFFER, GL_R8UI, e->buf ); // after glBufferData (Apple GL keeps reading old storage otherwise)
		glBindTexture( GL_TEXTURE_BUFFER, 0 );
		glBindBuffer( GL_TEXTURE_BUFFER, 0 );
		e->ready = true;
		e->job.reset();
	}
	return e->ready ? e->tex : wireDummyEdge; // until the flags land: every edge
}

void Renderer::SetWireUniforms( GLuint p, const FrameState& fs, GpuModel* model )
{
	const FrameState::WireState& w = fs.wire;
	glUniform1i( U( p, "uWirePass" ), wirePassNow );
	glUniform1f( U( p, "uWireMix" ), wireMixNow >= 0.0f ? wireMixNow : w.mix );
	glUniform3fv( U( p, "uWireColor" ), 1, w.color );
	glUniform1f( U( p, "uWireColorMix" ), w.colorMix );
	// Wire Fade runs across the model: from its near side to its far side as seen from the camera
	const float d = std::sqrt( ( fs.camPos[ 0 ] - fs.model[ 12 ] ) * ( fs.camPos[ 0 ] - fs.model[ 12 ] ) + ( fs.camPos[ 1 ] - fs.model[ 13 ] ) * ( fs.camPos[ 1 ] - fs.model[ 13 ] ) +
	                          ( fs.camPos[ 2 ] - fs.model[ 14 ] ) * ( fs.camPos[ 2 ] - fs.model[ 14 ] ) );
	const float r = 1.2f * fs.modelScale / std::max( model->fitScale, 1e-12f );
	glUniform1f( U( p, "uWireFade" ), w.fade );
	glUniform1f( U( p, "uWireFadeNear" ), d - r );
	glUniform1f( U( p, "uWireFadeFar" ), d + r );
	glUniform1i( U( p, "uWireEdges" ), w.edges == 1 ? 1 : 0 );
	glActiveTexture( GL_TEXTURE0 + 25 );
	glBindTexture( GL_TEXTURE_BUFFER, WireEdgeTex( model, w.edges == 1 ) );
	glActiveTexture( GL_TEXTURE0 );
}

// The main pass with a wire mode on (fs.wire.on): blended, premultiplied ONE / ONE_MINUS_SRC_ALPHA.
//   Over Surface   Hidden: one draw (the W1 formula with the line colour)  X-Ray: the surface, then the lines with no depth test
//   Wire Only      Hidden: depth of the solid, then the lines (LEQUAL)     X-Ray: the lines, no depth test
//   + Dark Fill    Hidden: one opaque black draw with the lines            X-Ray: the black fill, then the lines with no depth test
//   W6 Points Only Hidden: depth of the solid, then the points (LEQUAL)    X-Ray: the points, no depth test
//   Points + Wire  Hidden: depth of the solid, the lines, the points       X-Ray: the lines, then the points, no depth test
void Renderer::DrawWired( GLuint prog, const FrameState& fs, GpuModel* model, const std::vector< Instance >* inst )
{
	const FrameState::WireState& w = fs.wire;
	const GLboolean blendWas       = glIsEnabled( GL_BLEND );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	auto draw = [ & ]( int pass, float mix ) {
		wirePassNow = pass, wireMixNow = mix;
		DrawModel( prog, fs, model, false, true, 0, nullptr, nullptr, inst );
	};
	// MoveEhr W4: Points Only with a Point Motion: the points leave the surface (Attract / Breathe pull them inside it), so the still solid's
	// depth would hide them: no solid depth, the points hide each other instead (they write depth)
	const bool moving = w.mode == 4 && w.ptfx && w.motion != 0;
	auto lines = [ & ]( bool xray, bool points ) {
		glDepthMask( points && moving && !xray ? GL_TRUE : GL_FALSE );
		if( xray )
			glDisable( GL_DEPTH_TEST );
		else
			glDepthFunc( points && moving ? GL_LESS : GL_LEQUAL );
		pointsNow = points;
		draw( 2, -1.0f );
		pointsNow = false;
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
		glDepthMask( GL_TRUE );
	};
	const bool xray = w.back == 1;
	if( w.mode >= 4 )
	{
		if( !xray && !moving )
			draw( 0, -1.0f );
		if( w.mode == 5 )
			lines( xray, false );
		lines( xray, true );
	}
	else if( w.mode == 2 )
	{
		if( !xray )
			draw( 0, -1.0f );
		lines( xray, false );
	}
	else if( w.mode == 3 )
	{
		draw( 1, xray ? 0.0f : -1.0f );
		if( xray )
			lines( true, false );
	}
	else
	{
		draw( 3, xray ? 0.0f : -1.0f );
		if( xray )
			lines( true, false );
	}
	wirePassNow = 3, wireMixNow = -1.0f;
	if( !blendWas )
		glDisable( GL_BLEND );
	if( w.glow > 0.0f || w.bloom > 0.0f )
		wireDraws.push_back( { prog, &fs, model, inst, activeSide, 1.0f } );
}

// The lines' light into the HDR wire buffer: the solids' depth first (Back Hidden), then the lines added (ONE, ONE). Then the
// halo: the buffer at half size, blurred across then down (Wire Bloom Size = the reach).
void Renderer::RenderWireGlow( const FrameState& fs, int w, int h )
{
	wireGlowOk = false;
	// W9: the line looks glow with Wire Glow / Bloom; neon tubes always (their light on the model is read from this buffer)
	const bool lines = !lineDraws.empty() && ( fs.wire.glow > 0.0f || fs.wire.bloom > 0.0f || fs.lines.look == 3 );
	if( wireDraws.empty() && !lines )
		return;
	if( wireW != w || wireH != h || !wireFBO )
	{
		auto target = []( GLuint& tex, GLuint& fbo, int tw, int th ) {
			if( !tex )
				glGenTextures( 1, &tex );
			glBindTexture( GL_TEXTURE_2D, tex );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, tw, th, 0, GL_RGBA, GL_HALF_FLOAT, nullptr );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
			glBindTexture( GL_TEXTURE_2D, 0 );
			if( !fbo )
				glGenFramebuffers( 1, &fbo );
			glBindFramebuffer( GL_FRAMEBUFFER, fbo );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
		};
		target( wireTex, wireFBO, w, h );
		if( !wireDepth )
			glGenRenderbuffers( 1, &wireDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, wireDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h ); // W9: the main depth's format (the line glow blits it in)
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, wireFBO );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, wireDepth );
		for( int i = 0; i < 2; ++i )
			target( wireBlur[ i ], wireBlurFBO[ i ], std::max( 1, w / 2 ), std::max( 1, h / 2 ) );
		wireW = w, wireH = h;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, wireFBO );
	glViewport( 0, 0, w, h );
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE );
	const int sideWas = activeSide;
	for( int step = 0; step < 2; ++step )
		for( const WireDraw& d : wireDraws )
		{
			const bool xray = d.fs->wire.back == 1;
			const bool moving = d.fs->wire.mode == 4 && d.fs->wire.ptfx && d.fs->wire.motion != 0; // W4: the moving points' light: no solid depth
			if( step == 0 && ( xray || moving ) )
				continue; // X-Ray: no depth from the solid
			activeSide  = d.side;
			EnsureVAO( d.model );
			wirePassNow = step == 0 ? 0 : 4, wireMixNow = -1.0f;
			glDepthMask( step == 0 ? GL_TRUE : GL_FALSE );
			if( step == 1 )
			{
				if( xray )
					glDisable( GL_DEPTH_TEST );
				else
					glDepthFunc( GL_LEQUAL );
			}
			// W6: Points Only glows its points, Points + Wire its lines and its points
			const int mode = d.fs->wire.mode;
			if( step == 0 || mode != 4 )
				DrawModel( d.prog, *d.fs, d.model, false, true, 0, nullptr, nullptr, d.inst );
			if( step == 1 && mode >= 4 )
			{
				pointsNow = true, pointShareNow = d.share;
				DrawModel( d.prog, *d.fs, d.model, false, true, 0, nullptr, nullptr, d.inst );
				pointsNow = false, pointShareNow = 1.0f;
			}
			glEnable( GL_DEPTH_TEST );
			glDepthFunc( GL_LESS );
		}
	activeSide = sideWas;
	if( !wireDraws.empty() )
		EnsureVAO( wireDraws.front().model );
	if( lines )
	{
		// the solid's depth: the wired draws wrote it; else the main pass's (one sample of it)
		if( wireDraws.empty() )
		{
			glBindFramebuffer( GL_READ_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
			glBindFramebuffer( GL_DRAW_FRAMEBUFFER, wireFBO );
			glBlitFramebuffer( 0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST );
			glBindFramebuffer( GL_FRAMEBUFFER, wireFBO );
		}
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LEQUAL );
		glDepthMask( GL_FALSE );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		for( const LineDraw& d : lineDraws )
			DrawLines( d, true );
		glDepthFunc( GL_LESS );
	}
	wirePassNow = 3;
	glDepthMask( GL_TRUE );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
	if( fs.wire.bloom > 0.0f )
	{
		const int bw = std::max( 1, w / 2 ), bh = std::max( 1, h / 2 );
		const int taps     = 4 + int( 20.0f * fs.wire.bloomSize );
		const float stride = ( 1.0f + 3.0f * fs.wire.bloomSize ) * float( h ) / 1080.0f;
		glViewport( 0, 0, bw, bh );
		glBindVertexArray( quadVAO );
		glUseProgram( progBlur1D );
		glUniform1i( U( progBlur1D, "uTaps" ), taps );
		glActiveTexture( GL_TEXTURE0 );
		glBindFramebuffer( GL_FRAMEBUFFER, wireBlurFBO[ 0 ] );
		glUniform2f( U( progBlur1D, "uDir" ), stride / float( w ), 0.0f );
		glBindTexture( GL_TEXTURE_2D, wireTex );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glBindFramebuffer( GL_FRAMEBUFFER, wireBlurFBO[ 1 ] );
		glUniform2f( U( progBlur1D, "uDir" ), 0.0f, stride / float( bh ) );
		glBindTexture( GL_TEXTURE_2D, wireBlur[ 0 ] );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	wireGlowOk = true;
}

void Renderer::AddWireLight( const FrameState& fs, int w, int h )
{
	if( !wireGlowOk )
		return;
	glViewport( 0, 0, w, h );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE ); // light: rgb added, alpha untouched (W23)
	glUseProgram( progWireAdd );
	// W7: with sparkles in the buffer and no Wire Glow, the buffer adds at Glow 1 (the sparkles' gain is set against it)
	glUniform1f( U( progWireAdd, "uGlow" ), ( sparkleNow && fs.wire.glow <= 0.0f ? 1.0f : fs.wire.glow ) * 0.35f );
	glUniform1f( U( progWireAdd, "uHalo" ), fs.wire.bloom > 0.0f ? fs.wire.bloom * 0.6f : 0.0f );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, fs.wire.bloom > 0.0f ? wireBlur[ 1 ] : wireTex );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, wireTex );
	glBindVertexArray( quadVAO );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ZERO );
}
}
