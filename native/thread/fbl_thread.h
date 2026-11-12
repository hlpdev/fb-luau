#ifndef FBL_THREAD_H
#define FBL_THREAD_H

// Luau's threads, its coroutines: making one for a function, resuming it, with values or with an
// error raised where it yielded, what it is now, and closing it. A function of the program's yields
// the thread that called it by giving FBL_YIELD, with what it yields left in its frame.

#include "core/fbl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What a thread is now, as coroutine.status says it.
#define FBL_THREAD_SUSPENDED 0 // made, or yielded: a resume goes on with it
#define FBL_THREAD_RUNNING   1 // running: the frame's own thread
#define FBL_THREAD_NORMAL    2 // it resumed another, or called the program, which is running
#define FBL_THREAD_FINISHED  3 // its function returned, or it was closed
#define FBL_THREAD_FAILED    4 // an error ended it

// Adds a new thread to the frame, which runs the function in `slot` when it's first resumed. A
// status.
FBL_API int32_t fbl_thread_new(uint64_t state, int32_t frame, int32_t slot);

// Adds the thread the frame is on to it: in a function of the program's, the thread that called
// it. A status.
FBL_API int32_t fbl_thread_current(uint64_t state, int32_t frame);

// What the thread in `slot` is now, an FBL_THREAD_ constant, or -1 for a slot that isn't a thread.
FBL_API int32_t fbl_thread_status(uint64_t state, int32_t frame, int32_t slot);

// The function the thread in `slot` is in, as a number of its own while the function lives: the
// one running, or the one it yielded in, as a C function yields; or 0 for a thread in none, done
// or not started, or a slot that isn't a thread. A wait keeps it, to tell that the thread is
// still in that wait when it's due.
FBL_API int64_t fbl_thread_waits_in(uint64_t state, int32_t frame, int32_t slot);

// Whether the thread in `slot` is the calling script's: 1 when its main function's environment is
// the nearest Luau function's calling the program's function the frame is in, or no script's
// code calls, or its main function is C; 0 for another script's; -1 for a slot that isn't a
// thread.
FBL_API int32_t fbl_thread_is_callers(uint64_t state, int32_t frame, int32_t slot);

// The thread in `slot`'s id, a number of its own from 1, the same for each value of the thread, and
// never another thread's; or 0 for a slot that isn't a thread, or with no memory to number it.
FBL_API uint64_t fbl_thread_id(uint64_t state, int32_t frame, int32_t slot);

// Whether a function of the program's that has the frame can yield its thread: 1, or 0 when nothing
// can wait for it, on a call of the program's, which runs to its end, or in a metamethod or a C
// function Luau called; -1 for a frame it can't use.
FBL_API int32_t fbl_thread_can_yield(uint64_t state, int32_t frame);

// Resumes the thread in `slot` with the values after it as its arguments, or, when `raise` isn't 0,
// raises the one value after it in it where it yielded, as an error, which its pcall can catch. The
// thread and the values are taken off the frame, and when it yields or returns, what it yields or
// returns is added; `finished` says which, 1 for a return. A status: an error ends the thread,
// whose value and traceback are the State's error. FBL_RUNTIME for a thread that isn't suspended,
// or one a call of the program's runs on. Each resume from the program, not a function Luau
// called, has its own time budget.
FBL_API int32_t fbl_thread_resume(uint64_t state, int32_t frame, int32_t slot, int32_t raise,
                                  int32_t* finished);

// Closes the thread in `slot`, as coroutine.close does: it's finished, and lets go of what it
// held. A status: FBL_RUNTIME for one that's running, or that resumed one that is.
FBL_API int32_t fbl_thread_close(uint64_t state, int32_t frame, int32_t slot);

#ifdef __cplusplus
}
#endif

#endif
