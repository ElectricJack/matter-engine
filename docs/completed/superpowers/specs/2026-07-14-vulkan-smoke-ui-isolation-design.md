# Vulkan Smoke UI Isolation Design

## Goal

Keep the normal Vulkan viewer demo screenshot representative, including its debug UI, while making Cornell material-region verification independent of UI layout at both normal and resized window dimensions.

## Runtime behavior

The viewer accepts `MATTER_HIDE_UI=1`. It continues the ImGui begin/end-frame lifecycle but skips drawing the debug, worlds, and camera panels. This keeps Vulkan UI backend behavior intact while producing an unobstructed scene capture. The option is off by default.

## Smoke behavior

The smoke script captures three relevant images:

1. A 1280x720 demo image with the UI visible. It verifies expected overlay pixels and does not use this image for Cornell material thresholds.
2. A 1280x720 hidden-UI Cornell verification image. It verifies the UI overlay is absent and applies the existing red, green, gray, and nonblack scene thresholds unchanged.
3. A 960x540 hidden-UI resize verification image. It verifies the UI overlay is absent and applies the same unchanged scene thresholds.

The material-override case also runs hidden-UI because its purpose is rendered-material and diagnostic verification rather than UI presentation.

## Failure handling and tests

Environment variables are restored after the smoke run. The source gate requires the runtime option, visible-demo assertion, hidden-verification assertion, and unchanged Cornell thresholds. Runtime smoke remains the authoritative check for capture dimensions, Vulkan validation errors, material diagnostics, and image content.
