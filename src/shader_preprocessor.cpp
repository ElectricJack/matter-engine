#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <regex>
#include <filesystem>

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
    std::string process_includes(const std::string& file_path) {
        // Prevent infinite recursion
        std::filesystem::path abs_path = std::filesystem::absolute(file_path);
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
                std::filesystem::path current_dir = std::filesystem::path(file_path).parent_path();
                std::filesystem::path include_path = current_dir / include_file;
                
                // Add comment showing what's being included
                result += "// === BEGIN INCLUDE: " + include_file + " ===\n";
                
                // Recursively process the included file
                std::string included_content = process_includes(include_path.string());
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
    
    std::set<std::filesystem::path> processed_files_;
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