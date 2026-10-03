#ifndef GS_RENDER_THREAD_DISPATCHER_H
#define GS_RENDER_THREAD_DISPATCHER_H

#include "core/error/error_list.h"
#include "core/variant/callable.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include <atomic>

class IRenderThreadDispatcher {
public:
    virtual ~IRenderThreadDispatcher() = default;

    // Argument-order contract. The dispatcher submits
    // `p_callable.bind(request_id)` and the render thread calls it with no
    // arguments. Godot passes call arguments BEFORE bound ones
    // (CallableCustomBind::call, core/variant/callable_bind.cpp), so when
    // p_callable already carries binds, e.g.
    // `callable_mp(this, &T::f).bind(data)`, the dispatcher's request_id is
    // passed ahead of them: the target is called as f(request_id, data).
    // Every target therefore takes `uint64_t p_request_id` as its FIRST
    // parameter, followed by its own bound arguments in bind order, and must
    // call notify_completed(p_request_id). A target with p_request_id last
    // receives its arguments swapped and nothing reports the call error. An
    // int argument becomes the request id and the int is "completed" in its
    // place, which can push completed_request_id past every outstanding
    // request so that later blocking dispatches return before their callback
    // has run. An Object or Transform3D argument converts to null or identity
    // while p_request_id becomes 0, so that request never completes and the
    // caller blocks for the full timeout.
    virtual bool dispatch_call_on_render_thread_blocking(const Callable &p_callable, bool *r_dispatched,
            bool p_allow_timeout, uint64_t *r_request_id, const char *p_log_prefix) = 0;
    // Returns true iff a call to dispatch_call_on_render_thread_blocking()
    // would actually attempt a dispatch (i.e. the early-exit guard would
    // not short-circuit it). This is the discrimination signal callers need
    // when reasoning about "is the render-thread dispatch path live?",
    // because the bool return of dispatch_call_on_render_thread_blocking()
    // conflates "no dispatch path active" (early-exit returned false) with
    // "dispatched but timed out waiting for completion" (also returned
    // false). The fixture in test_renderer_lifetime_proof.h depends on
    // this distinction to decide whether the renderer dtor will fall
    // through to the synchronous teardown path. (Codex PR #386 review,
    // P1.)
    virtual bool is_render_thread_dispatch_path_active() const = 0;
    virtual void notify_completed(uint64_t p_request_id) = 0;
    virtual void set_timeout_usec(uint64_t p_timeout_usec) = 0;
    virtual uint64_t get_timeout_usec() const = 0;
    virtual uint64_t get_next_request_id() const = 0;
    virtual uint64_t get_completed_request_id() const = 0;
    virtual void promote_latest_data_request_id(uint64_t p_request_id) = 0;
    virtual uint64_t get_latest_data_request_id() const = 0;
    virtual void set_latest_data_result(Error p_error) = 0;
    virtual Error get_latest_data_result() const = 0;
};

class RenderThreadDispatcher : public IRenderThreadDispatcher {
public:
    RenderThreadDispatcher() = default;
    ~RenderThreadDispatcher() override = default;

    bool dispatch_call_on_render_thread_blocking(const Callable &p_callable, bool *r_dispatched,
            bool p_allow_timeout, uint64_t *r_request_id, const char *p_log_prefix) override;
    bool is_render_thread_dispatch_path_active() const override;
    void notify_completed(uint64_t p_request_id) override;
    void set_timeout_usec(uint64_t p_timeout_usec) override;
    uint64_t get_timeout_usec() const override;
    uint64_t get_next_request_id() const override;
    uint64_t get_completed_request_id() const override;
    void promote_latest_data_request_id(uint64_t p_request_id) override;
    uint64_t get_latest_data_request_id() const override;
    void set_latest_data_result(Error p_error) override;
    Error get_latest_data_result() const override;

protected:
    // Protected rather than private so a test double can issue request ids
    // exactly as dispatch_call_on_render_thread_blocking() does.
    mutable Mutex dispatch_mutex;
    mutable Semaphore dispatch_semaphore;
    std::atomic<uint64_t> next_request_id{1};
    std::atomic<uint64_t> completed_request_id{0};
    std::atomic<uint64_t> timeout_usec{15000000};
    std::atomic<uint64_t> latest_data_request_id{0};
    std::atomic<Error> latest_data_result{OK};
};

#endif // GS_RENDER_THREAD_DISPATCHER_H
