#pragma once
#include "takedown/runtime/native_function_dispatcher.hpp"

namespace takedown::runtime::slus_21050 {
inline constexpr guest::GuestAddress kSubmitStaticObjectAddress=0x0026af50U;
// Temporary reference-runtime boundary around the typed native game operation.
// Rejection leaves CPU and memory unchanged. No instruction executor is called.
[[nodiscard]] std::optional<NativeFunctionExecution> try_submit_static_object(
    guest::R5900State& cpu,guest::EeMemory& memory);
[[nodiscard]] bool register_static_object_native_function(
    NativeFunctionDispatcher& dispatcher,const guest::EeMemory& memory);
}
