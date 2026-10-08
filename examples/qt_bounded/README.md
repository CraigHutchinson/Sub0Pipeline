# Bounded Qt execution

Enable `SUB0PIPELINE_BUILD_QT_EXAMPLE=ON` with Qt 6.2+ Core available, then run
`ctest --test-dir build --output-on-failure`. This executable validates fan-out
saturation, cooperative stop-aware waiting, no ACK after cancellation and joined
owner teardown. It does not require a GUI event loop.

The private QThreadPool has a fixed maximum worker count and no queued backlog:
`tryStart` either starts a worker or the dispatching thread executes the task.
This caller-runs overflow rule avoids queue-capacity deadlocks when a worker
schedules successors. Job affinity/priority/stack hints are intentionally ignored.
Jobs must tolerate inline recursion, and must not synchronously wait for sibling
jobs. Worker count bounds do not bound recursion, caller concurrency, callable
allocations or Qt's internal allocations.

Use `RunScope` off the GUI thread. Its destructor requests stop and joins all
borrowed-state work; declare it after those borrowed members. Jobs must not wait
for GUI callbacks if that GUI thread is joining the scope. To post UI results,
use a Qt receiver/context whose lifetime is independently managed.

Reference: [QThreadPool contract](https://doc.qt.io/qt-6/qthreadpool.html).

Runnable preparation precedes local accounting. A `tryStart(QRunnable*)`
exception rolls back that accounting without accepting work; ordinary overflow
still runs on the caller. This relies on Qt rejecting before handing the runnable
to a worker. The inspected
[Qt 6.4.2 implementation](https://github.com/qt/qtbase/blob/v6.4.2/src/corelib/thread/qthreadpool.cpp)
and [Qt 6.8.3 implementation](https://github.com/qt/qtbase/blob/v6.8.3/src/corelib/thread/qthreadpool.cpp)
allocate worker/queue storage before handoff. The actual receiving executable
prints its Qt version. Host protocol mocks check preparation, rejection, reuse
and caller-runs behavior; they do not validate Qt's internals or replace the real
Qt full-CI run. Bodies, completion callbacks and callable destructors must not throw.
