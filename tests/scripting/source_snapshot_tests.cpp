#include "module_resolver.h"
#include <cstdio>
#include <map>
#include <stdexcept>

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        const std::string root = "import { a } from 'shared-lib/a.js'; import 'shared-lib/a';";
        std::map<std::string, std::string> sources{
            {"shared-lib/a", "import 'shared-lib/b'; export const a = 2;"},
            {"shared-lib/b", "import 'shared-lib/a'; export const b = 3;"}};
        unsigned reads = 0;
        const module_resolver::SourceLoader loader =
            [&](const std::string& spec, module_resolver::ResolvedModule& module, std::string& err) {
                ++reads;
                auto found = sources.find(spec);
                if (found == sources.end()) { err = "missing " + spec; return false; }
                module = {spec, found->second, "memory://" + spec};
                return true;
            };
        module_resolver::FoldResult fold;
        std::string err;
        check(module_resolver::fold_sources_with_loader(root, loader, fold, err), "transitive fold");
        check(reads == 2 && fold.modules.size() == 2, "cycles and .js aliases deduplicate");
        std::string expected = root;
        for (const auto& entry : sources) { expected.push_back('\0'); expected += entry.second; }
        check(std::string(fold.folded.begin(), fold.folded.end()) == expected, "canonical actual byte fold");
        sources.clear();
        check(fold.modules[0].source.find("const a = 2") != std::string::npos, "fold owns source snapshot");
        check(!module_resolver::fold_sources_with_loader(root, loader, fold, err), "missing is fail closed");
        check(err == "missing shared-lib/a", "loader diagnostic survives");
        const auto calls = reads;
        check(!module_resolver::fold_sources_with_loader("import 'shared-lib/../bad';", loader, fold, err),
              "invalid module name rejected");
        check(reads == calls, "invalid names never reach loader");
        std::puts("SOURCE-SNAPSHOT: transitive actual bytes, aliases, cycles, ownership and fail-closed lookup passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
