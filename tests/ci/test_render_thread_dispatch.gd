extends SceneTree

# #104: executable harness for the render-thread dispatch characterization.
#
# The equivalent C++ [RequiresGPU] doctests in
# modules/gaussian_splatting/tests/test_renderer_pipeline.h can never actually run:
# Godot's `--test` boot does not create a RenderingServer, so those cases only ever
# skip (a vacuous "pass" with zero assertions). This script drives the SAME
# GaussianSplatRenderer test hooks under the FULL engine (live RenderingServer +
# render loop). The baseline_qa harness must launch it with `--render-thread separate`
# so this script runs OFF the render thread — the dispatch characterization requires it.
#
# Fail-closed: if the live-stack preconditions are not met we FAIL (never skip), because
# skipping is exactly the gap #104 closes. On any failure we print [RUNTIME_FAIL] and
# quit(1) so tests/ci/run_baseline_qa.py classifies the run as failed.

var _ran := false
var _failures: Array[String] = []

func _fail(msg: String) -> void:
	_failures.append(msg)
	push_error("[RUNTIME_FAIL] " + msg)
	print("[RUNTIME_FAIL] " + msg)

func _require_live_stack() -> bool:
	if RenderingServer == null:
		_fail("RenderingServer unavailable — launch under the full engine (not --test/--headless)")
		return false
	if RenderingServer.is_on_render_thread():
		_fail("Running ON the render thread — launch with --render-thread separate")
		return false
	if not RenderingServer.is_render_loop_enabled():
		_fail("Render loop disabled — the dispatch characterization requires a live render loop")
		return false
	return true

func _new_renderer() -> Object:
	var r: Object = ClassDB.instantiate("GaussianSplatRenderer")
	if r == null:
		_fail("Could not instantiate GaussianSplatRenderer")
	return r

# Blocking dispatch times out when the callback never signals completion.
func _case_timeout() -> void:
	var r := _new_renderer()
	if r == null:
		return
	var original: int = r.test_get_render_thread_dispatch_timeout_usec()
	r.test_set_render_thread_dispatch_timeout_usec(10000) # 10 ms
	var start := Time.get_ticks_usec()
	var dispatched: bool = r.test_dispatch_call_on_render_thread_blocking_without_completion()
	var elapsed := Time.get_ticks_usec() - start
	r.test_set_render_thread_dispatch_timeout_usec(original)
	if dispatched:
		_fail("timeout: a dispatch that never completes must return false (timed out)")
	if elapsed < 10000:
		_fail("timeout: elapsed %d us must be >= the 10000 us timeout" % elapsed)
	if elapsed >= 2000000:
		_fail("timeout: elapsed %d us must stay bounded (< 2 s)" % elapsed)

# Completion state only advances on a successful dispatch.
func _case_completion_advances_only_on_success() -> void:
	var r := _new_renderer()
	if r == null:
		return
	var original: int = r.test_get_render_thread_dispatch_timeout_usec()
	var completed_before: int = r.test_get_render_thread_dispatch_completed_request_id()
	r.test_set_render_thread_dispatch_timeout_usec(10000)
	var timed_out: bool = r.test_dispatch_call_on_render_thread_blocking_without_completion()
	if timed_out:
		_fail("completion: the timed-out dispatch must return false")
	if r.test_get_render_thread_dispatch_completed_request_id() != completed_before:
		_fail("completion: completed request id must NOT advance on a timed-out dispatch")
	var completed: bool = r.test_dispatch_call_on_render_thread_blocking_with_completion()
	if not completed:
		_fail("completion: a dispatch that signals completion must return true")
	if r.test_get_render_thread_dispatch_completed_request_id() <= completed_before:
		_fail("completion: completed request id must advance after a successful dispatch")
	r.test_set_render_thread_dispatch_timeout_usec(original)

# Forward progress is preserved after a timeout escape (+ stale completion is ignored).
func _case_forward_progress_after_timeout() -> void:
	var r := _new_renderer()
	if r == null:
		return
	var original: int = r.test_get_render_thread_dispatch_timeout_usec()
	r.test_set_render_thread_dispatch_timeout_usec(10000)
	var timed_out: bool = r.test_dispatch_call_on_render_thread_blocking_without_completion()
	if timed_out:
		_fail("forward-progress: the initial dispatch must time out (false)")
	var recovered: bool = r.test_dispatch_call_on_render_thread_blocking_with_completion()
	if not recovered:
		_fail("forward-progress: a subsequent dispatch must still succeed after a timeout")
	r.test_set_render_thread_dispatch_timeout_usec(original)
	var completed: int = r.test_get_render_thread_dispatch_completed_request_id()
	if completed > 0:
		r.test_notify_render_thread_dispatch_completed(completed - 1)
		if r.test_get_render_thread_dispatch_completed_request_id() != completed:
			_fail("forward-progress: a stale (lower) completion id must not regress the completed id")

# The dispatch-path probe stays true through a wait-for-completion timeout.
func _case_path_probe_stays_active() -> void:
	var r := _new_renderer()
	if r == null:
		return
	if not r.test_is_render_thread_dispatch_path_active():
		_fail("path-probe: the dispatch path must be active before any dispatch (loop on, off render thread)")
	var original: int = r.test_get_render_thread_dispatch_timeout_usec()
	r.test_set_render_thread_dispatch_timeout_usec(10000)
	var timed_out: bool = r.test_dispatch_call_on_render_thread_blocking_without_completion()
	if timed_out:
		_fail("path-probe: the dispatch must time out (false)")
	if not r.test_is_render_thread_dispatch_path_active():
		_fail("path-probe: a wait-for-completion timeout must NOT tear the dispatch path down (probe stays true)")
	r.test_set_render_thread_dispatch_timeout_usec(original)

# Teardown after a dispatch cycle remains bounded (no hang) after timeout recovery.
func _case_teardown_bounded() -> void:
	var start := Time.get_ticks_usec()
	var r := _new_renderer()
	if r == null:
		return
	var original: int = r.test_get_render_thread_dispatch_timeout_usec()
	r.test_set_render_thread_dispatch_timeout_usec(10000)
	var timed_out: bool = r.test_dispatch_call_on_render_thread_blocking_without_completion()
	if timed_out:
		_fail("teardown: the dispatch must time out (false)")
	var recovered: bool = r.test_dispatch_call_on_render_thread_blocking_with_completion()
	if not recovered:
		_fail("teardown: the recovery dispatch must succeed")
	r.test_set_render_thread_dispatch_timeout_usec(original)
	r = null # release the RefCounted renderer -> exercise teardown
	var elapsed := Time.get_ticks_usec() - start
	if elapsed >= 5000000:
		_fail("teardown: a dispatch cycle + teardown must remain bounded (< 5 s), was %d us" % elapsed)

func _process(_delta: float) -> bool:
	# Run once, a couple frames in so the render thread is fully pumping.
	if _ran:
		return true
	_ran = true

	print("[GS-RTD] Render-thread dispatch characterization harness (#104)")
	if not _require_live_stack():
		print("[GS-RTD] RESULT: FAIL (live-stack preconditions not met)")
		quit(1)
		return true

	_case_timeout()
	_case_completion_advances_only_on_success()
	_case_forward_progress_after_timeout()
	_case_path_probe_stays_active()
	_case_teardown_bounded()

	if _failures.is_empty():
		print("[GS-RTD] RESULT: PASS — 5 render-thread dispatch characterizations executed under a live stack")
		quit(0)
	else:
		print("[GS-RTD] RESULT: FAIL — %d assertion failure(s)" % _failures.size())
		quit(1)
	return true
