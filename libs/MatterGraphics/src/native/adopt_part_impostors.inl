void VkSceneRenderer::adopt_part_impostors(const VkScenePart& part,
                                           uint32_t part_slot,
                                           uint32_t vertex_base) {
    const auto started_at = std::chrono::steady_clock::now();
    if (impostor_atlas_.image == VK_NULL_HANDLE) {
        // Create it on demand. Parts register BEFORE VkSceneRenderer::init()
        // in the production publish path -- measured on RockGallery, ten of
        // eleven impostor-bearing parts arrived first -- so an atlas built
        // only in init() exists for whichever parts happen to come last.
        // That is not a race to tolerate: it silently halves the tier.
        std::string create_error;
        if (!create_impostor_atlas(create_error)) {
            if (!impostor_slots_exhausted_logged_) {
                impostor_slots_exhausted_logged_ = true;
                MATTER_LOGE("vk",
                    "impostor atlas could not be created (%s); part "
                    "%016llx's %zu impostor(s) will not draw\n",
                    create_error.c_str(),
                    static_cast<unsigned long long>(part.part_hash),
                    part.impostors.size());
                std::fflush(stderr);
            }
            return;
        }
    }
    if (impostor_atlas_.image == VK_NULL_HANDLE) {
        // Never silently. A part arriving with atlases and no image to put
        // them in is the tier being absent, and the whole point of M2.5 is
        // that such a state announces itself.
        if (!impostor_slots_exhausted_logged_) {
            impostor_slots_exhausted_logged_ = true;
            MATTER_LOGE("vk",
                "impostor atlas image does not exist; part %016llx's "
                "%zu impostor(s) will not draw\n",
                static_cast<unsigned long long>(part.part_hash),
                part.impostors.size());
            std::fflush(stderr);
        }
        return;
    }

    std::vector<uint8_t> staging_bytes;
    const bool prepared = !part.impostor_upload_bytes.empty();
    std::vector<VkBufferImageCopy> regions;
    std::vector<uint32_t> assigned;
    std::vector<size_t> assigned_impostors;
    // ordinal -> slot, for the vertex patch below.
    std::vector<uint32_t> slot_for_ordinal(part.impostors.size(), 0);
    uint32_t first_layer = UINT32_MAX;

    for (const auto& imp : part.impostors) {
        // Compared against the LATCH (what the image was created with), not a
        // fresh impostor::atlas_bytes(): those differ exactly when the
        // resolution changed after init, which is the case this catches.
        const bool valid_prepared = prepared &&
            imp.upload_mips.size() == impostor::filtered_mip_count(
                impostor_layer_px_ / impostor::kGridDim) &&
            !imp.upload_mips.empty() &&
            imp.upload_mips.front().edge == impostor_layer_px_ &&
            std::all_of(imp.upload_mips.begin(), imp.upload_mips.end(),
                [&](const VkScenePartImpostor::UploadMip& mip) {
                    const size_t mip_index = &mip - imp.upload_mips.data();
                    const uint32_t expected_edge = impostor_layer_px_ >> mip_index;
                    const size_t count = size_t(mip.edge) * mip.edge * 4;
                    return mip.edge == expected_edge &&
                        mip.shade_offset <= part.impostor_upload_bytes.size() &&
                        count <= part.impostor_upload_bytes.size() - mip.shade_offset &&
                        mip.tint_offset <= part.impostor_upload_bytes.size() &&
                        count <= part.impostor_upload_bytes.size() - mip.tint_offset;
                });
        if (prepared ? !valid_prepared :
            imp.atlas.size() != impostor_atlas_bytes_) {
            if (prepared) {
                MATTER_LOGE("vk",
                    "impostor mip upload for part %016llx cluster %u has "
                    "invalid offsets or resolution (expected cell %u px) "
                    "-- this impostor will not draw\n",
                    static_cast<unsigned long long>(part.part_hash),
                    imp.cluster, impostor_layer_px_ / impostor::kGridDim);
            } else {
                MATTER_LOGE("vk",
                    "impostor atlas for part %016llx cluster %u is %zu bytes, "
                    "expected %zu (atlas built for cell %u px) -- this impostor "
                    "will not draw\n",
                    static_cast<unsigned long long>(part.part_hash),
                    imp.cluster, imp.atlas.size(), impostor_atlas_bytes_,
                    impostor_layer_px_ / impostor::kGridDim);
            }
            std::fflush(stderr);
            continue;
        }
        uint32_t slot;
        if (!impostor_free_slots_.empty()) {
            slot = impostor_free_slots_.back();
            impostor_free_slots_.pop_back();
        } else if (impostor_next_slot_ < kImpostorMaxSlots) {
            slot = impostor_next_slot_++;
        } else {
            // Exhaustion is a LOAD FAILURE and gets said out loud, once. The
            // impostors that miss out keep slot 0 (zero coverage) and discard,
            // so the coarsest mesh rung is what shows -- degraded, never a
            // white card, and never silent.
            if (!impostor_slots_exhausted_logged_) {
                impostor_slots_exhausted_logged_ = true;
                MATTER_LOGW("vk",
                    "impostor atlas slots exhausted (%u of %u in use) at "
                    "part %016llx -- further impostors will not draw\n",
                    impostor_resident_, kImpostorMaxSlots,
                    static_cast<unsigned long long>(part.part_hash));
                std::fflush(stderr);
            }
            break;
        }
        if (imp.ordinal < slot_for_ordinal.size())
            slot_for_ordinal[imp.ordinal] = slot;
        assigned.push_back(slot);
        assigned_impostors.push_back(&imp - part.impostors.data());
        const auto mips = prepared ? std::vector<impostor::FilteredMip>{}
                                   : impostor::filtered_mips(imp.atlas,
                                                             impostor_layer_px_);
        for (uint32_t layer = 0; layer < 2; ++layer) {
          const uint32_t mip_count = prepared
              ? static_cast<uint32_t>(imp.upload_mips.size())
              : static_cast<uint32_t>(mips.size());
          for (uint32_t mip = 0; mip < mip_count; ++mip) {
            size_t offset;
            uint32_t edge;
            if (prepared) {
                const auto& upload = imp.upload_mips[mip];
                offset = layer == 0 ? upload.shade_offset : upload.tint_offset;
                edge = upload.edge;
            } else {
                offset = staging_bytes.size();
                const auto& bytes = layer == 0 ? mips[mip].shade : mips[mip].tint;
                staging_bytes.insert(staging_bytes.end(), bytes.begin(), bytes.end());
                edge = mips[mip].edge;
            }
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(offset);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = slot * 2 + layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {edge, edge, 1};
            regions.push_back(region);
            first_layer = std::min(first_layer, slot * 2 + layer);
          }
        }
    }
    if (regions.empty()) return;
    const auto filtered_at = std::chrono::steady_clock::now();
    const auto& upload_bytes = prepared ? part.impostor_upload_bytes : staging_bytes;

    // Layers are assigned from a free list, so a part's slots need not be
    // contiguous. The barrier covers the whole span they touch; layers inside
    // it that belong to other parts are already SHADER_READ_ONLY and are
    // transitioned to the same layout they were in, which is a no-op for them.
    uint32_t last_layer = 0;
    for (const auto& r : regions)
        last_layer = std::max(last_layer, r.imageSubresource.baseArrayLayer);

    std::string error;
    matter::VkBufferResource staging;
    if (!matter::create_buffer(*vulkan_,
                               static_cast<VkDeviceSize>(upload_bytes.size()),
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging,
                               error) ||
        !matter::map_buffer(staging, error)) {
        MATTER_LOGE("vk",
            "impostor atlas staging failed for part %016llx: %s -- "
            "%zu impostor(s) will not draw\n",
            static_cast<unsigned long long>(part.part_hash), error.c_str(),
            assigned.size());
        std::fflush(stderr);
        for (uint32_t slot : assigned) impostor_free_slots_.push_back(slot);
        return;
    }
    std::memcpy(staging.mapped, upload_bytes.data(), upload_bytes.size());
    if (!matter::flush_buffer(staging, 0, upload_bytes.size(), error)) {
        MATTER_LOGE("vk",
            "impostor atlas flush failed for part %016llx: %s\n",
            static_cast<unsigned long long>(part.part_hash), error.c_str());
        std::fflush(stderr);
        for (uint32_t slot : assigned) impostor_free_slots_.push_back(slot);
        return;
    }
    const auto staged_at = std::chrono::steady_clock::now();
    ImpostorUploadRecord upload;
    upload.image = impostor_atlas_.image;
    upload.staging = staging.buffer;
    upload.regions = regions;
    upload.layer_count = last_layer - first_layer + 1;
    upload.rt_available = vulkan_->ray_tracing_available();
    // The record's barrier keys off regions.front()'s layer, so sort by layer
    // and let first_layer be the front.
    std::sort(upload.regions.begin(), upload.regions.end(),
              [](const VkBufferImageCopy& a, const VkBufferImageCopy& b) {
                  return a.imageSubresource.baseArrayLayer <
                         b.imageSubresource.baseArrayLayer;
              });
    if (!matter::submit_immediate(*vulkan_, record_impostor_upload, &upload,
                                  error,
                                  matter::ImmediateSubmitPhase::staging_upload,
                                  {staging.lifetime, impostor_atlas_.lifetime})) {
        MATTER_LOGE("vk",
            "impostor atlas upload failed for part %016llx: %s -- "
            "%zu impostor(s) will not draw\n",
            static_cast<unsigned long long>(part.part_hash), error.c_str(),
            assigned.size());
        std::fflush(stderr);
        for (uint32_t slot : assigned) impostor_free_slots_.push_back(slot);
        return;
    }
    const auto submitted_at = std::chrono::steady_clock::now();

    // Patch the STAGED vertices, not the caller's copy: the billboard's tint
    // channel transports its atlas slot to raster.vert, and the slot is not
    // known until now. `ordinal` (tint.b at bake time) is how a vertex names
    // which of the part's impostors it belongs to.
    const uint8_t marker_hi = 0;  // see the surface.x test below
    (void)marker_hi;
    size_t patched = 0;
    for (size_t vi = 0; vi < part.vertices.size(); ++vi) {
        const VkRasterVertex& src = part.vertices[vi];
        if (!(src.surface.x > 1.0e29f)) continue;
        const uint32_t ordinal =
            static_cast<uint32_t>(src.tint.z * 255.0f + 0.5f);
        const uint32_t slot =
            ordinal < slot_for_ordinal.size() ? slot_for_ordinal[ordinal] : 0u;
        VkRasterVertex& dst = vertex_staging_[vertex_base + vi];
        dst.tint.x = static_cast<float>(slot & 0xFFu) / 255.0f;
        dst.tint.y = static_cast<float>((slot >> 8) & 0xFFu) / 255.0f;
        ++patched;
    }
    const auto finished_at = std::chrono::steady_clock::now();
    const auto elapsed_ms = [](auto begin, auto end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    if (elapsed_ms(started_at, finished_at) > 100.0)
        MATTER_LOGW("vk", "slow impostor adoption hash=%016llx slots=%zu "
            "bytes=%zu filter=%.3f staging=%.3f submit=%.3f patch=%.3f ms",
            static_cast<unsigned long long>(part.part_hash), assigned.size(),
            upload_bytes.size(), elapsed_ms(started_at, filtered_at),
            elapsed_ms(filtered_at, staged_at),
            elapsed_ms(staged_at, submitted_at),
            elapsed_ms(submitted_at, finished_at));
    if (patched == 0 && !assigned.empty()) {
        // The atlas is resident but nothing references it: the ladder and the
        // vertex stream disagree, which is exactly the class of silent
        // mismatch this milestone exists to make impossible.
        MATTER_LOGE("vk",
            "part %016llx carries %zu impostor atlas(es) but no billboard "
            "vertices -- the impostor rung is missing from the vertex stream\n",
            static_cast<unsigned long long>(part.part_hash), assigned.size());
        std::fflush(stderr);
    }
    impostor_slots_of_part_[part_slot] = assigned;
    impostor_resident_ += static_cast<uint32_t>(assigned.size());

    // MATTER_IMPOSTOR_DEBUG=1: the four numbers that separate every way this
    // tier can be present-but-invisible -- did an atlas arrive, does it have
    // any coverage, which layer pair did it land in, and did the vertices
    // actually take the slot.
    static const bool debug = std::getenv("MATTER_IMPOSTOR_DEBUG") != nullptr;
    if (debug) {
        for (size_t k = 0; k < assigned.size(); ++k) {
            const auto& imp = part.impostors[assigned_impostors[k]];
            uint32_t max_cov = 0, covered = 0;
            const auto& source = prepared ? part.impostor_upload_bytes : imp.atlas;
            const size_t shade_offset = prepared
                ? imp.upload_mips[0].shade_offset : 0;
            for (size_t t = 0; t + 3 < impostor_atlas_bytes_ / 2; t += 4) {
                const uint8_t a = source[shade_offset + t + 3];
                if (a > max_cov) max_cov = a;
                if (a > 0) ++covered;
            }
            MATTER_LOGD("vk",
                "impostor part=%016llx cluster=%u ordinal=%u slot=%u "
                "layers=%u/%u max_coverage=%u covered_texels=%u patched_verts=%zu\n",
                static_cast<unsigned long long>(part.part_hash), imp.cluster,
                imp.ordinal, assigned[k], assigned[k] * 2, assigned[k] * 2 + 1,
                max_cov, covered, patched);
        }
        std::fflush(stderr);
    }
}
