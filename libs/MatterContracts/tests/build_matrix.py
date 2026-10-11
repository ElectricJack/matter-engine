"""Configure/build isolated public consumers; never claim runtime extraction."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run(command):
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode:
        raise RuntimeError(f"Command failed: {command!r}\n{completed.stdout}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--make-program")
    args = parser.parse_args()
    contracts = Path(__file__).resolve().parents[1]
    root = contracts.parents[1]
    manifest = json.loads((contracts / "component-ownership.json").read_text())
    modules = {item["target"]: item for item in manifest["modules"]}
    allowed = {"matter_contracts": [], **{name: item["dependencies"] for name, item in modules.items()}}

    def closure(names):
        reached = set()
        visiting = set()

        def visit(name):
            if name in visiting:
                raise AssertionError(f"component cycle at {name}")
            if name in reached:
                return
            visiting.add(name)
            for dependency in allowed[name]:
                visit(dependency)
            visiting.remove(name)
            reached.add(name)
        for name in names:
            visit(name)
        return reached

    closure(allowed)  # Fail before configure if manifest is cyclic.
    cases = [("contracts-off", contracts, [], "matter_contracts")]
    cases += [(name + "-only", contracts, [name], name) for name in modules]
    cases += [("all-scaffolds", contracts, list(modules), "matter_contracts")]
    cases += [(name + "-standalone", root / item["directory"], [], name) for name, item in modules.items()]
    for label, source, selected, consumer in cases:
        with tempfile.TemporaryDirectory(prefix="matter-interface-matrix-") as temporary:
            work = Path(temporary)
            build = work / "build"
            query = build / ".cmake/api/v1/query"
            query.mkdir(parents=True)
            (query / "codemodel-v2").touch()
            commands = ["cmake_minimum_required(VERSION 3.25)", "project(public_consumer LANGUAGES C CXX)",
                        "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)"]
            for name, item in modules.items():
                value = "ON" if name in selected else "OFF"
                commands.append(f'set({item["option"]} {value} CACHE BOOL "" FORCE)')
            commands += [f'add_subdirectory("{source.as_posix()}" component)',
                         'matter_write_component_graph("${CMAKE_BINARY_DIR}/matrix-graph.txt")',
                         'add_executable(public_consumer probe.cpp)',
                         f'target_link_libraries(public_consumer PRIVATE {consumer})']
            (work / "CMakeLists.txt").write_text("\n".join(commands) + "\n")
            (work / "probe.cpp").write_text(
                '#include <matter/contracts/generation.h>\n#include <matter/contracts/graphics.h>\n'
                '#include <matter/contracts/terrain.h>\n#include <matter/contracts/hosts.h>\n'
                'int main() { return matter::services::validate(matter::services::JobEnvelope{"probe", "1", 1}).ok() ? 0 : 1; }\n')
            command = [args.cmake, "-S", str(work), "-B", str(build), "-G", args.generator,
                       "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
            if args.make_program:
                command.append(f"-DCMAKE_MAKE_PROGRAM={args.make_program}")
            run(command)
            actual = {}
            for line in (build / "matrix-graph.txt").read_text().splitlines():
                name, state, dependencies = line.split("|")
                assert name not in actual, f"duplicate target {name}"
                assert state == ("implemented" if name == "matter_contracts" else "scaffold")
                actual[name] = dependencies.split(",") if dependencies else []
            expected = closure(["matter_contracts", *selected, consumer])
            assert set(actual) == expected, (label, actual, expected)
            for name, dependencies in actual.items():
                assert dependencies == allowed[name], (label, name, dependencies)
            replies = build / ".cmake/api/v1/reply"
            index = json.loads(next(replies.glob("index-*.json")).read_text())
            model = json.loads((replies / index["reply"]["codemodel-v2"]["jsonFile"]).read_text())
            targets = model["configurations"][0]["targets"]
            assert {t["name"] for t in targets} == {"matter_contracts", "public_consumer"}, (label, targets)
            for target in targets:
                detail = json.loads((replies / target["jsonFile"]).read_text())
                for entry in detail.get("sources", []):
                    path = Path(entry["path"])
                    if not path.is_absolute():
                        path = work / path
                    assert path.is_relative_to(contracts) or path == work / "probe.cpp", (label, path)
            run([args.cmake, "--build", str(build), "--target", "public_consumer", "--parallel", "2"])
            executable = build / ("public_consumer.exe" if __import__("os").name == "nt" else "public_consumer")
            run([str(executable)])
        print(f"MATRIX {label}: passed (public-interface scaffold only)", flush=True)
    print(f"{len(cases)} configurations passed; disabled components absent; graph acyclic")


if __name__ == "__main__":
    main()
