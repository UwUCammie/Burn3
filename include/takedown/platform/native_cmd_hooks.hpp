#pragma once

// Hooks the GS stream writer calls into the native command emitter (NS-3), so that the emitter can keep the quadwords it holds back
// (a PATH1 strip it may replace by a token) in stream order with the non-GIF records the writer writes itself.
namespace takedown::platform {

class NativeCmdStreamHooks {
  public:
    virtual ~NativeCmdStreamHooks() = default;
    // Called before the writer ends the open Transfer for a non-GIF record, a flush or a close: hand every held quadword to the
    // writer unchanged (a strip that was not yet complete is forwarded as it is). Not called for the emitter's own records.
    virtual void flush_held() = 0;
    // Called at the start of write_vblank, before anything else: the frame boundary (the emitter writes its FrameInfo, frees).
    virtual void on_vblank() = 0;
};

void set_native_cmd_stream_hooks(NativeCmdStreamHooks* hooks) noexcept;
[[nodiscard]] NativeCmdStreamHooks* native_cmd_stream_hooks() noexcept;

} // namespace takedown::platform
