#pragma once

// MatterEngine3/src/export/obj_writer.h
//
// The Wavefront half of asset export: one ExportModel in, one `.obj`, one
// `.mtl` and the texture files out. Everything format-specific lives here, so
// adding a glTF writer means adding a sibling of this file and nothing else
// (mesh_export.h's ExportModel is the seam, and is deliberately free of OBJ
// concepts).
//
// FILE LAYOUT for a part exported as `<base>`:
//   <base>.obj              geometry, one `o <name>`, one `usemtl` per material
//   <base>.mtl              one `newmtl` per material the mesh actually uses
//   <base>_albedo.png       }
//   <base>_normal.png       }  the six channels of texture_bake.h, when the
//   <base>_roughness.png    }  texture format is not None
//   <base>_metallic.png     }
//   <base>_emissive.png     }
//   <base>_ao.png           }
//
// MATERIAL KEY MAPPING (docs/export-obj.md has the table and the three.js
// notes). The PBR keys are the widely-implemented "PBR extension" spelling:
//   Kd/Ke/Ks/Ns/d/Ni    classic Wavefront
//   Pr  map_Pr          roughness
//   Pm  map_Pm          metalness
//   map_Kd              albedo          map_Ke  emissive
//   map_Ka              ambient occlusion
//   norm + map_Bump     tangent-space normal (both, see the docs: three.js's
//                       MTLLoader reads `norm` as normalMap, and the spec this
//                       exporter implements asks for map_Bump too)
//
// Kd IS WHITE WHENEVER map_Kd IS WRITTEN. Every consumer that supports map_Kd
// multiplies it by Kd, so a coloured Kd would darken a textured material twice.
// The flat colour is not lost: it is in a `# base_albedo` comment on the
// material and in the manifest, and a texture-less export (`--texture-format
// none`) writes the real Kd.
//
// Ke follows the same rule but ONLY when an emissive map is present, and that
// distinction matters: white Kd with no map_Kd is a washed-out surface, while
// white Ke with no map_Ke is a fully self-lit one. part_export therefore skips
// the emissive bake for a part where nothing emits, and this writer then emits
// the real (usually black) Ke.
//
// DETERMINISM. All text goes through export_text.h's locale-independent
// formatter, lines are LF, and files are written binary so no platform inserts
// CRLF. Two runs over the same ExportModel produce byte-identical text.

#include "export/export_image.h"
#include "export/mesh_export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace matter_export {

struct ObjWriteOptions {
    // Existing directory the files are written into. The writer creates
    // nothing; part_export.h owns the output tree.
    std::string directory;
    // File stem. Defaults to the model name when empty.
    std::string base_name;
    TextureFormat texture_format = TextureFormat::Png;
};

// One file the export produced, for the manifest and for size gates.
struct WrittenFile {
    std::string name;            // file name only, no directory
    uint64_t bytes = 0;
    uint64_t content_hash = 0;   // fnv1a64 of the bytes on disk
};

struct ObjWriteResult {
    std::string obj_name;
    std::string mtl_name;
    // In write order: obj, mtl, then the maps in MapKind order.
    std::vector<WrittenFile> files;
};

// Render the OBJ body. Pure text, no I/O — exposed because it is what the
// determinism and round-trip tests want to compare.
std::string build_obj_text(const ExportModel& model, const std::string& base_name);

// Render the MTL body. `map_extension` is the texture files' extension without
// the dot ("png"); empty means no maps are referenced at all.
std::string build_mtl_text(const ExportModel& model, const std::string& base_name,
                           const std::string& map_extension);

// Write everything. Fails (false, `error` set) on the first I/O or encode
// error; files already written are left in place, because a partially written
// export directory is more diagnosable than a silently cleaned one.
bool write_obj(const ExportModel& model, const ObjWriteOptions& options,
               ObjWriteResult& result, std::string& error);

} // namespace matter_export
