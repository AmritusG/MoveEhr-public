// W26: the model-swap prewarm of every W26 program (the driver compiles a variant on its first draw: 30-160 ms), each drawn
// once through a 1-pixel scissor so switching an effect on mid-show never hitches.
#include "Renderer.h"
#include <cstring>

namespace mehr
{
void Renderer::WarmWow( const FrameState& fs, GpuModel* model, int w, int h )
{
	if( !EnsureSceneDepth( w, h ) )
		return;
	FrameState t = fs;
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	glViewport( 0, 0, w, h );
	glEnable( GL_SCISSOR_TEST );
	glScissor( 0, 0, 1, 1 );
	// Stage 2 projector: the cookie pass, the PROJ_PASS surface programs (blended, LEQUAL), both beam marches
	t.projector.on      = true;
	t.projector.surface = t.projector.beam = 1.0f;
	t.projector.source  = 1;
	RenderProjector( t, nullptr );
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	glViewport( 0, 0, w, h );
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glEnable( GL_POLYGON_OFFSET_FILL );
	for( GLuint p : { progProjMain, progProjSoup } )
	{
		glUseProgram( p );
		BindCookieLights( p, t );
		DrawModel( p, t, model, false, p == progProjSoup, 3 );
	}
	glDisable( GL_POLYGON_OFFSET_FILL );
	glDisable( GL_DEPTH_TEST );
	glDepthFunc( GL_LESS );
	glDisable( GL_BLEND );
	DrawProjMarch( t, false );
	DrawProjMarch( t, true );
	projOk = false;
	// Stage 4 liquid (needs 3D textures of its type on units 1 / 2: the noise volume stands in)
	EnsureNoise();
	for( LiquidSdf& l : liquidSdf )
		if( !l.tex )
			l.min[ 0 ] = l.min[ 1 ] = l.min[ 2 ] = 0.0f, l.size = 1.0f;
	LiquidSdf& la = liquidSdf[ sideSlot[ 0 ] ];
	LiquidSdf& lb = liquidSdf[ sideSlot[ 1 ] ];
	const GLuint keep[ 2 ] = { la.tex, lb.tex };
	la.tex = keep[ 0 ] ? keep[ 0 ] : noiseTex;
	lb.tex = keep[ 1 ] ? keep[ 1 ] : noiseTex;
	glBindFramebuffer( GL_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
	glViewport( 0, 0, w, h );
	RenderLiquid( t, 0.5f, w, h );
	lb.tex = keep[ 1 ], la.tex = keep[ 0 ];
	// Stage 5 lightning: the segment quads (MAX into the bolt buffer) and the add pass
	glDisable( GL_DEPTH_TEST );
	t.bolts.segs = { 0, 0, 0, 1, 0.1f, 0, 0, 1 };
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	RenderGlow( t, w, h );
	glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
	WarmSwarm( t, w, h ); // Stage 6
	glEnable( GL_SCISSOR_TEST );
	// Stage 7 lava: the crack glow programs (plain / soup / split soup), the split render + shadow soups, the haze composite
	if( model )
	{
		t.lava.on = true, t.lava.split = 0.5f;
		glBindFramebuffer( GL_FRAMEBUFFER, tSamples > 1 ? msFBO : resolveFBO );
		glViewport( 0, 0, w, h );
		glEnable( GL_DEPTH_TEST );
		for( GLuint p : { progCrackMain, progCrackSoup, progCrackSoupC, progSoupC, progShadowSoupC, progSoupB, progShadowSoupB } ) // + Stage 9 Bounce
			DrawModel( p, t, model, false, p != progCrackMain, 3 );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
		glDepthFunc( GL_LEQUAL );
		for( GLuint p : { progCrackMain, progCrackSoup, progCrackSoupC } )
			DrawModel( p, t, model, false, p != progCrackMain, 3 );
		glDepthFunc( GL_LESS );
		glDisable( GL_BLEND );
		glDisable( GL_DEPTH_TEST );
		glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
		glUseProgram( progCompositeHaze );
		glBindVertexArray( quadVAO );
		glDrawArrays( GL_TRIANGLES, 0, 3 );
		glBindVertexArray( 0 );
	}
	// Stage 8 caustics: the photon passes (their own targets: unscissored there, ~1 ms once), then the floor composite
	if( model )
	{
		glDisable( GL_SCISSOR_TEST );
		t.caustic.on = true, t.caustic.floor = t.caustic.smoke = t.caustic.surface = 1.0f, t.caustic.rainbow = 0.0f;
		const float o[ 3 ] = { 0, 3, 3 }, d[ 3 ] = { 0, -0.7f, -0.7f };
		std::memcpy( t.caustic.pos, o, sizeof( o ) ), std::memcpy( t.caustic.dir, d, sizeof( d ) );
		RenderCaustics( t, model );
		glEnable( GL_SCISSOR_TEST );
		glScissor( 0, 0, 1, 1 );
		glBindFramebuffer( GL_FRAMEBUFFER, resolveFBO );
		glViewport( 0, 0, w, h );
		glEnable( GL_DEPTH_TEST );
		glEnable( GL_BLEND );
		glBlendFuncSeparate( GL_ONE, GL_ONE, GL_ZERO, GL_ONE );
		glDepthFunc( GL_LEQUAL );
		for( GLuint p : { progProjMain, progProjSoup } )
		{
			glUseProgram( p );
			BindCookieLights( p, t );
			DrawModel( p, t, model, false, p == progProjSoup, 3 );
		}
		glDepthFunc( GL_LESS );
		glDisable( GL_BLEND );
		glDisable( GL_DEPTH_TEST );
		causticOk = false;
	}
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_SCISSOR_TEST );
}
}
