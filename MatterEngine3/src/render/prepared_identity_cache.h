#pragma once
#include "asset_pages.h"
#include "asset_binary.h"
#include "matter/log.h"
#include <filesystem>
#include <map>
#include <mutex>

namespace viewer::prepared_identity {
// One session owns writes. Worker callers serialize small metadata operations;
// warm lookup only touches the in-memory map after its first binary read.
class Cache {
public:
    static constexpr uint32_t kind = 0x31444953; // SID1
    static constexpr size_t capacity = 65536;
    static constexpr const char* reference = "prepared-identities-v1";
    Cache(std::string directory, bool writable) : writable_(writable) {
        config_.store.dir=std::move(directory); config_.resident_bytes=4u<<20;
        config_.bank=asset_store::PageBank::create(config_.resident_bytes,256);
        config_.limits.max_bytes=2u<<20; config_.max_read_bytes=2u<<20;
    }
    ~Cache() {
        std::string error;
        if (!flush(error)) MATTER_LOGW("geometry","prepared identity commit failed: %s",error.c_str());
    }
    uint64_t lookup(uint64_t request) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!load()) return 0;
        const auto found=entries_.find(request);
        return found==entries_.end()?0:found->second;
    }
    bool remember(uint64_t request, uint64_t resolved, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!writable_ || !request || !resolved || !load()) {error="identity cache is unavailable or read-only";return false;}
        const auto found=entries_.find(request);
        if (found!=entries_.end()) {
            if(found->second!=resolved){error="conflicting prepared identity";return false;}
            error.clear();return true;
        }
        if(entries_.size()>=capacity){error="prepared identity capacity exceeded";return false;}
        entries_.emplace(request,resolved);++dirty_;
        if(dirty_>=64)return flush_locked(error);
        error.clear();return true;
    }
    bool flush(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);return flush_locked(error);
    }
    static bool decode(const asset_store::PageView& page, std::map<uint64_t,uint64_t>& out) {
        const auto* rows=page.find(1);
        if(page.kind!=kind || page.sections.size()!=1 || !page.dependencies.empty() ||
           !rows || rows->schema!=1 || rows->stride!=16 || rows->count>capacity || rows->size!=size_t(rows->count)*16)return false;
        std::map<uint64_t,uint64_t> decoded;uint64_t previous=0;
        for(uint32_t i=0;i<rows->count;++i){
            const auto* row=rows->data+size_t(i)*16;
            const auto request=asset_store::get_u64(row), resolved=asset_store::get_u64(row+8);
            if(!request || !resolved || request<=previous)return false;
            decoded.emplace(request,resolved);previous=request;
        }
        out=std::move(decoded);return true;
    }
private:
    bool load() {
        if(loaded_)return healthy_;
        loaded_=true;
        std::error_code ec;
        if(!std::filesystem::exists(config_.store.dir,ec))return healthy_=!ec;
        std::string error;
        auto reader=asset_store::PageCache::open(config_,error);
        if(!reader)return healthy_=false;
        const auto result=reader->read_manifest(reference);
        if(result.status==asset_store::PageStatus::Missing)return true;
        return healthy_=result.page && decode(result.page->view,entries_);
    }
    bool flush_locked(std::string& error) {
        if(!dirty_){error.clear();return true;}
        asset_store::PageSection rows;rows.type=1;rows.stride=16;
        rows.bytes.reserve(entries_.size()*16);
        for(const auto& entry:entries_){asset_store::push_u64(rows.bytes,entry.first);asset_store::push_u64(rows.bytes,entry.second);}
        std::vector<uint8_t> bytes;
        if(!asset_store::encode_page(kind,{rows},{},config_.limits,bytes,error))return false;
        auto store=asset_store::BlobStore::open(config_.store,&error);if(!store)return false;
        asset_store::BlobHash hash;
        if(store->put(bytes.data(),bytes.size(),&hash)!=asset_store::Status::Ok || !store->flush_index()){
            error=store->last_error();return false;
        }
        auto refs=asset_store::RefTable::open(*store,{},&error);
        if(!refs || !refs->put(reference,hash,kind,bytes.size()) || !refs->flush()){
            if(error.empty())error="prepared identity reference commit failed";return false;
        }
        dirty_=0;error.clear();return true;
    }
    asset_store::PageCacheConfig config_;
    bool writable_=false,loaded_=false,healthy_=true;
    size_t dirty_=0;
    std::map<uint64_t,uint64_t> entries_;
    std::mutex mutex_;
};
}
