#include "mesh_worker_fixture.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cerrno>
#include <signal.h>
#include <unistd.h>

using namespace worker_fixture;
int main(int argc,char **argv) {
    std::filesystem::path dir;
    try {
        check(argc==2,"fake worker path required");
        std::string pattern=(std::filesystem::temp_directory_path()/"matter-worker-tests-XXXXXX").string();
        check(mkdtemp(pattern.data())!=nullptr,"test directory"); dir=pattern;
        auto worker=[&](const char *mode) {
            auto path=dir/mode;
            if(!std::filesystem::exists(path)) std::filesystem::create_symlink(argv[1],path);
            return g::PrimitiveGeometry({}, {path.string()});
        };
        auto p=product(); auto r=request(p); auto o=options();
        std::string original; check(d::encode(p.document,original).ok(),"encode fixture");
        check(!advertised(g::PrimitiveGeometry{}),"empty path advertised");
        check(!advertised(g::PrimitiveGeometry({}, {"/nonexistent/matter-worker"})),"missing worker advertised");
        check(!advertised(worker("wrong")),"wrong protocol advertised");
        auto start=std::chrono::steady_clock::now();
        check(!advertised(worker("probe_hang")),"hanging probe advertised");
        check(std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"probe was unbounded");
        auto echo=worker("echo"); check(advertised(echo),"valid worker unavailable");
        auto success=echo.process(job,p,r,o,{}); check(success.ok(),"echo processing failed");
        check(s::validate(*success.value()).ok(),"echo asset invalid");
        const auto &history=std::get<d::Value::Array>(std::get<d::Value::Object>(success.value()->document.metadata.data)
            .at("matter.geometry.modifiers").data);
        s::ProcessingRequest recorded; recorded.settings=history.back();
        const auto &defaults=parameters(recorded);
        check(std::get<double>(defaults.at("target_ratio").data)==1 && std::get<double>(defaults.at("iterations").data)==3 &&
            std::get<double>(defaults.at("seed").data)==0 && std::get<double>(defaults.at("timeout_seconds").data)==60,
            "legacy resolved defaults not recorded");
        auto custom=r; auto &forward=parameters(custom);
        forward["target_ratio"]=0.5; forward["iterations"]=7; forward["seed"]=4294967295.0; forward["timeout_seconds"]=12;
        check(worker("options").process(job,p,custom,o,{}).ok(),"legacy parameters not forwarded");
        for(auto mode:{"crash","truncated","huge","material","trailing","nonzero","flood","nan","index"}) {
            auto instance=worker(mode);
            failed(instance.process(job,p,r,o,{}),d::ErrorCode::backend_failure);
            std::ifstream pidfile((dir/(std::string(mode)+".pid")).string()); int pid=0; pidfile>>pid;
            check(pid>0 && kill(pid,0)==-1 && errno==ESRCH,"failed worker not reaped");
        }
        auto stacked=r; stacked.required_capabilities.push_back({"matter.operation.mesh.smooth",1});
        auto &regions=std::get<d::Value::Array>(std::get<d::Value::Object>(stacked.settings.data).at("regions").data);
        auto &stages=std::get<d::Value::Array>(std::get<d::Value::Object>(regions[0].data).at("modifiers").data);
        stages.insert(stages.begin(),d::Value::Object{{"operation","mesh.smooth"}});
        failed(worker("crash").process(job,p,stacked,o,{}),d::ErrorCode::backend_failure);
        auto hang=worker("hang");
        s::CancellationSource cancellation; s::JobControl control; control.cancellation=cancellation.token();
        bool started=false;
        std::thread cancel([&] {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(std::chrono::steady_clock::now()<deadline) {
                if(std::filesystem::exists(dir/"hang.pid")) { started=true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            cancellation.cancel();
        });
        auto cancelled=hang.process(job,p,r,o,control); cancel.join();
        check(started,"cancelled before child dispatched"); failed(cancelled,d::ErrorCode::cancelled);
        { std::ifstream f(dir/"hang.pid"); int pid=0; f>>pid; check(pid>0 && kill(pid,0)==-1 && errno==ESRCH,"cancelled child not reaped"); }
        o.budget.execution_microseconds=100000;
        failed(hang.process(job,p,r,o,{}),d::ErrorCode::resource_limit);
        o=options(); parameters(r)["timeout_seconds"]=1;
        failed(hang.process(job,p,r,o,{}),d::ErrorCode::resource_limit);
        r=request(p); parameters(r)["target_ratio"]=0;
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::invalid_input);
        r=request(p); parameters(r)["seed"]=4294967295.0;
        check(echo.process(job,p,r,o,{}).ok(),"uint32 seed rejected");
        parameters(r)["seed"]=4294967296.0;
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::invalid_input);
        r=request(p); r.required_capabilities.clear();
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::invalid_input);
        r=request(p); parameters(r)["operation_version"]=2;
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::unsupported_version);
        r=request(p); o.budget.working_bytes=65536;
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::resource_limit);
        o=options(); o.budget.output_bytes=16;
        failed(echo.process(job,p,r,o,{}),d::ErrorCode::resource_limit);
        // A worker can disappear between calls; availability is not cached.
        std::filesystem::remove(dir/"echo"); check(!advertised(echo),"removed worker still advertised");
        failed(echo.process(job,p,r,options(),{}),d::ErrorCode::unsupported_capability);
        std::string after; check(d::encode(p.document,after).ok() && after==original,"input asset mutated");
        std::filesystem::remove_all(dir);
        std::cout<<"MESH-WORKER: availability, crash, malformed output, cancellation, reaping, budgets, atomic publication pass\n";
        return 0;
    } catch(const std::exception &e) {
        if(!dir.empty()) std::filesystem::remove_all(dir);
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
