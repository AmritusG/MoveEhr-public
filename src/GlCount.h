#pragma once
// MoveEhr W6: counts the GL calls that stall, read back, re-upload or link (the W6 1M-points question: "any glFinish / readback /
// buffer re-upload / program link per frame"). Included in place of <OpenGL/gl3.h>: the header first, then each counted entry point
// becomes "count, then the real call" (a plain increment, render thread only). Read only by the PERF line (Debug Log on): the
// GLCALLS part = per-frame averages since the last line. All instances share the counters (read it with one instance playing).
#include <OpenGL/gl3.h>
#include <cstddef>
#include <cstdint>

namespace mehr
{
struct GlCount
{
	enum Kind
	{
		Finish,         // glFinish
		ReadPixels,     // glReadPixels (to memory or a PBO)
		GetBufferData,  // glGetBufferSubData (a buffer readback: waits for the GPU)
		ClientWait,     // glClientWaitSync
		BufferData,     // glBufferData (a buffer's storage re-specified / uploaded)
		TexImage,       // glTexImage2D / 3D (a texture's storage re-specified / uploaded)
		TexSubImage,    // glTexSubImage2D
		Mipmap,         // glGenerateMipmap
		Link,           // glLinkProgram
		Count
	};
	uint64_t n[ Count ]  = {};
	uint64_t bufferBytes = 0;
	void Add( Kind k ) { ++n[ k ]; }
	void AddBytes( Kind k, size_t b ) { ++n[ k ], bufferBytes += b; }
	static const char* Name( int k )
	{
		static const char* s[ Count ] = { "finish", "readpix", "getbuf", "waitsync", "bufdata", "teximg", "texsub", "mipmap", "link" };
		return k >= 0 && k < Count ? s[ k ] : "?";
	}
};
inline GlCount glCount;
}

#define glFinish() ( mehr::glCount.Add( mehr::GlCount::Finish ), ::glFinish() )
#define glReadPixels( ... ) ( mehr::glCount.Add( mehr::GlCount::ReadPixels ), ::glReadPixels( __VA_ARGS__ ) )
#define glGetBufferSubData( ... ) ( mehr::glCount.Add( mehr::GlCount::GetBufferData ), ::glGetBufferSubData( __VA_ARGS__ ) )
#define glClientWaitSync( ... ) ( mehr::glCount.Add( mehr::GlCount::ClientWait ), ::glClientWaitSync( __VA_ARGS__ ) )
#define glBufferData( t, s, d, u ) ( mehr::glCount.AddBytes( mehr::GlCount::BufferData, size_t( s ) ), ::glBufferData( t, s, d, u ) )
#define glTexImage2D( ... ) ( mehr::glCount.Add( mehr::GlCount::TexImage ), ::glTexImage2D( __VA_ARGS__ ) )
#define glTexImage3D( ... ) ( mehr::glCount.Add( mehr::GlCount::TexImage ), ::glTexImage3D( __VA_ARGS__ ) )
#define glTexSubImage2D( ... ) ( mehr::glCount.Add( mehr::GlCount::TexSubImage ), ::glTexSubImage2D( __VA_ARGS__ ) )
#define glGenerateMipmap( ... ) ( mehr::glCount.Add( mehr::GlCount::Mipmap ), ::glGenerateMipmap( __VA_ARGS__ ) )
#define glLinkProgram( ... ) ( mehr::glCount.Add( mehr::GlCount::Link ), ::glLinkProgram( __VA_ARGS__ ) )
