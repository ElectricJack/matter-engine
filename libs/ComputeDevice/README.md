# Neutral compute device

`matter_compute_device` supplies synchronous, owned CPU readback and device
identity without any graphics or geometry dependency. Its optional
`matter_compute_vulkan` implementation owns per-call resources through proven
fence completion and can either create a headless device or retain an injected
host device/queue owner with a shared queue mutex.

The compute request is a trusted backend interface for ordered storage-buffer
kernels, not an untrusted shader sandbox. All buffers bind in declaration order,
every dispatch has a compute barrier, and readback happens after the final host
barrier and fence. Device-local handoff is intentionally absent.

See [the solid backend](../GeometryCompute/README.md) for ownership, cooperative
cancellation, bounded requests, failure retirement and exact tested build commands.
This target can be built independently with `cmake -S libs/ComputeDevice ...`;
`MATTER_COMPUTE_VULKAN` defaults OFF. No engine, window, scene, world or authoring
source is part of the device target.
