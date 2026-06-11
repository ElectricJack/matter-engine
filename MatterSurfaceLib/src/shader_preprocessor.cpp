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

class ShaderPreprocessor {
public:
    explicit ShaderPreprocessor(bool verbose = false) : verbose_(verbose) {}
    
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
        
        // Write output
        std::ofstream out(output_file);
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