// libs/MatterSurfaceLib/src/shader_preprocessor.cpp
//
// Build-time tool, not part of any library: this file has its own `main` and is
// linked into a standalone executable. GLSL has no `#include`, so shaders that
// want to share code (`bvh_tlas_common.glsl`, `materials.glsl`) use a quoted
// `#include` that this tool expands ahead of compilation.
//
//   shader_preprocessor <input.fs> <output.fs>
//
// It is invoked from `libs/MatterSurfaceLib/Makefile` (which builds it to
// `build/shader_preprocessor` and uses it for the `_processed.fs` shader) and
// from `MatterEditor/Makefile`'s `regen-processed-shader` target, which
// compiles this file on the fly. The `_processed.fs` output is a GENERATED,
// committed file — edit the `.fs`/`.glsl` sources and regenerate, never the
// processed file.
//
// Semantics and limits, all of which matter when a shader misbehaves:
//  - Only `#include "quoted"` on a line of its own is recognised (see the
//    regex). Angle-bracket includes and includes with trailing tokens are
//    passed through untouched.
//  - Paths resolve relative to the INCLUDING file's directory.
//  - Include-once, globally per run: a file already expanded anywhere in the
//    run is replaced by a `// File already included:` marker, so a header
//    pulled in by two different files is emitted only the first time.
//  - A missing include is NOT an error. It is replaced by a
//    `// ERROR: Include file not found:` comment and the tool still exits 0 —
//    the failure surfaces later as a GLSL compile error. Grep the output for
//    `// ERROR:` if a shader suddenly stops compiling.
//  - Expanded regions are bracketed by `// === BEGIN/END INCLUDE:` markers.
//  - Output is opened in BINARY mode on purpose (see the comment in
//    `process_file`) so the committed processed shader stays LF-only.
//  - Line numbers shift relative to the source; there is no `#line` emission,
//    so driver error line numbers refer to the PROCESSED file.

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <regex>
#include <cstdlib>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <climits>
#endif

// Recursive quoted-`#include` flattener. Reusable across files: `process_file`
// resets the visited set each call, so the include-once rule is scoped to one
// input file, not to the object's lifetime. `verbose_` only controls progress
// chatter on stdout; errors always go to stderr.
class ShaderPreprocessor {
public:
    explicit ShaderPreprocessor(bool verbose = false) : verbose_(verbose) {}
    
    // Expand `input_file` and write the result to `output_file`, truncating it.
    // Returns false only on an EMPTY expansion (which in practice means the
    // input file itself was empty — a missing file still expands to an error
    // comment) or on failure to open the output.
    bool process_file(const std::string& input_file, const std::string& output_file) {
        processed_files_.clear();
        
        if (verbose_) {
            std::cout << "Processing shader: " << input_file << " -> " << output_file << std::endl;
        }
        
        std::string content = process_includes(input_file);
        if (content.empty()) {
            std::cerr << "Error: Failed to process input file: " << input_file << std::endl;
            return false;
        }
        
        // Write output in binary mode: on Windows, text mode silently rewrites
        // every '\n' in `content` to '\r\n', turning a plain rebuild into a
        // whole-file CRLF diff against the committed (LF) processed shader.
        std::ofstream out(output_file, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "Error: Cannot open output file: " << output_file << std::endl;
            return false;
        }
        
        out << content;
        out.close();
        
        if (verbose_) {
            std::cout << "Shader processed successfully! Output: " << output_file << std::endl;
        }
        
        return true;
    }

private:
    std::string get_absolute_path(const std::string& path) {
#ifdef _WIN32
        // Use GetFullPathName on Windows
        char buffer[MAX_PATH];
        DWORD result = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
        if (result == 0 || result > MAX_PATH) {
            return path; // Return original path if GetFullPathName fails
        }
        return std::string(buffer);
#else
        char* real_path = realpath(path.c_str(), nullptr);
        if (real_path) {
            std::string result(real_path);
            free(real_path);
            return result;
        }
        return path; // fallback to original path if realpath fails
#endif
    }
    
    std::string get_parent_dir(const std::string& file_path) {
        size_t last_slash = file_path.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            return file_path.substr(0, last_slash);
        }
        return "."; // current directory
    }
    
    // Returns the fully expanded text of `file_path`. Never throws and never
    // signals failure through the return value: a cycle or a repeat inclusion
    // yields a `// File already included:` marker and an unreadable file yields
    // a `// ERROR: Include file not found:` marker, both as valid GLSL
    // comments. Recursion depth is bounded only by the include graph, which the
    // visited set keeps acyclic.
    std::string process_includes(const std::string& file_path) {
        // Prevent infinite recursion
        std::string abs_path = get_absolute_path(file_path);
        if (processed_files_.find(abs_path) != processed_files_.end()) {
            return "// File already included: " + file_path + "\n";
        }
        processed_files_.insert(abs_path);
        
        // Read file
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "// ERROR: Include file not found: " + file_path + "\n";
        }
        
        std::string result;
        std::string line;
        std::regex include_pattern("^\\s*#include\\s+\"([^\"]+)\"\\s*$");
        
        while (std::getline(file, line)) {
            std::smatch match;
            if (std::regex_match(line, match, include_pattern)) {
                std::string include_file = match[1].str();
                
                // Resolve relative path
                std::string current_dir = get_parent_dir(file_path);
                std::string include_path = current_dir + "/" + include_file;
                
                // Add comment showing what's being included
                result += "// === BEGIN INCLUDE: " + include_file + " ===\n";
                
                // Recursively process the included file
                std::string included_content = process_includes(include_path);
                result += included_content;
                
                // Remove trailing newlines and add our own
                while (!result.empty() && result.back() == '\n') {
                    result.pop_back();
                }
                result += "\n// === END INCLUDE: " + include_file + " ===\n";
            } else {
                result += line + "\n";
            }
        }
        
        return result;
    }
    
    std::set<std::string> processed_files_;
    bool verbose_;
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.fs> <output.fs>" << std::endl;
        return 1;
    }
    
    std::string input_file = argv[1];
    std::string output_file = argv[2];
    
    ShaderPreprocessor preprocessor(true); // verbose mode
    
    if (!preprocessor.process_file(input_file, output_file)) {
        return 1;
    }
    
    return 0;
}