bool VkSceneRenderer::set_part_water_field_binding(
    std::uint64_t part_hash, WaterFieldBinding binding, std::string& error) {
    error.clear();
    if (fail_if_poisoned(error)) return false;
    if (binding.valid() && water_fields_.lookup(binding) == nullptr) {
        error = "water-field part binding is stale or unpublished";
        return false;
    }
    const auto found = slot_of_.find(part_hash);
    if (found == slot_of_.end()) {
        error = "water-field part binding references an unregistered part";
        return false;
    }
    const std::uint32_t part_slot = static_cast<std::uint32_t>(found->second);
    PartRecord& part = parts_[part_slot];
    if (part.water_binding_slot == binding.slot &&
        part.water_generation == binding.generation)
        return true;

    part.water_binding_slot = binding.slot;
    part.water_generation = binding.generation;
    for (std::size_t index = 0u; index != instance_staging_.size(); ++index) {
        if (index < instance_part_slots_.size() &&
            instance_part_slots_[index] == part_slot) {
            instance_staging_[index].water_binding_slot = binding.slot;
            instance_staging_[index].water_generation = binding.generation;
        }
    }
    bool dynamic_changed = false;
    for (std::size_t index = 0u; index != dynamic_instance_staging_.size();
         ++index) {
        if (index < dynamic_instance_part_slots_.size() &&
            dynamic_instance_part_slots_[index] == part_slot) {
            dynamic_instance_staging_[index].water_binding_slot = binding.slot;
            dynamic_instance_staging_[index].water_generation =
                binding.generation;
            dynamic_changed = true;
        }
    }
    dynamic_dirty_ = dynamic_dirty_ || dynamic_changed;
    ++instance_generation_;
    instance_snapshot_valid_ = false;
    return true;
}

#ifdef MATTER_VK_TEST_FAULT_INJECTION
bool VkSceneRenderer::part_is_raster_water(uint64_t part_hash) const noexcept {
    const auto found = slot_of_.find(part_hash);
    return found != slot_of_.end() && found->second >= 0 &&
           static_cast<size_t>(found->second) < parts_.size() &&
           parts_[static_cast<size_t>(found->second)].live &&
           parts_[static_cast<size_t>(found->second)].raster_water_surface;
}
#endif
