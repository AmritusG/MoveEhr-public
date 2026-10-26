#pragma once
// W21: text and logos as generated models. A spec string ("gen:" + key=value fields, see MakeTextSpec / MakeLogoSpec)
// goes through the normal loader (LoadJob), so the result is an ordinary ModelData: every LoadEhr feature works on it.
//   Text: CoreText (installed families or a font file), real line breaks = rows, kerning / ligatures from CTLine.
//   Logo: SVG (nanosvg: paths + fill colours; no <text>, gradients take their first stop), PDF and PDF-compatible
//         .ai (CGPDFScanner path + fill operators, CTM, form XObjects), PNG / JPG (traced: threshold, marching squares,
//         simplify, smooth; the image maps onto the front). EPS: not readable on this macOS (answered in Status).
//   Extrusion: the source's fill rule resolved by Clipper2 (outers + holes), caps triangulated by earcut, bevel profiles, side walls, optional back cap,
//         Outline Only. Parts: Front, Back, Side, Bevel. Every glyph / logo shape is one group (a letter).
#include "ModelData.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mehr
{
struct GenParams
{
	std::map< std::string, std::string > kv;
	std::string Get( const std::string& k, const std::string& def = "" ) const;
	float F( const std::string& k, float def ) const;
};

// the spec string for the plugin (the cache key: the same spec never rebuilds)
std::string MakeSpec( const std::map< std::string, std::string >& fields );
bool IsGenSpec( const std::string& path );
std::string GenSpecKey( const std::string& spec ); // the spec + the mtime / size of the files it names (font, logo)
// builds the model; `status` is the one-line report for the Status param (also on failure)
std::unique_ptr< ModelData > GenerateModel( const std::string& spec, const std::atomic< bool >& cancel, std::string& error, std::string& status );

std::vector< std::string > FontFamilies();                    // installed families (sorted, no hidden '.' ones)
std::vector< std::string > FontStyles( const std::string& family ); // the faces of one family
// MoveEhr W4 Point Shape Glyph: up to 64 characters of `text` (each once, no spaces; none = A-Z 0-9) in the font, 8 x 8 cells of 64 px
// in a 512 x 512 R8 atlas (row 0 = the top of glyph 0); returns the glyph count
int GlyphAtlas( const std::string& text, const std::string& family, const std::string& style, const std::string& file, std::vector< uint8_t >& out );
std::string ShortSpecName( const std::string& spec );         // for logs: "text 'LOADEHR'" / "logo emblem.svg"
inline std::string LogName( const std::string& path ) { return IsGenSpec( path ) ? ShortSpecName( path ) : path; }
}
