#pragma once

namespace TakedownRenderer
{
// Windows: on a crash (unhandled exception, abort, std::terminate) write the
// exception, the faulting module and offset, and a stack trace (module+offset
// per frame) to stderr, which the launcher keeps as the renderer log. The
// offsets resolve against the shipped executable's symbol table (nm). No-op
// elsewhere.
void install_crash_report();
}
