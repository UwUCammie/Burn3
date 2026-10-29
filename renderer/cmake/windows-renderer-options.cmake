# Initial cache for the Windows parallel-gs-stream build (cmake -C). Matches the
# release renderer: system SDL3 (SDL3.dll import) and DXGI interop enabled.
# TAKEDOWN_NATIVE_MODEL_REPLAY_AVAILABLE is deliberately not set.
set(GRANITE_SYSTEM_SDL ON CACHE BOOL "Use system SDL3 instead of vendored submodule." FORCE)
set(GRANITE_VULKAN_DXGI_INTEROP ON CACHE BOOL "Enable DXGI interop when running on Windows." FORCE)
# Release settings checked by tools/package_runtime_kit.py.
set(GRANITE_RENDERDOC_CAPTURE OFF CACHE BOOL "Enable support for RenderDoc capture from API." FORCE)
set(PARALLEL_GS_DEBUG OFF CACHE BOOL "Add extra debug logging." FORCE)
