#!/usr/bin/env python3
"""One explicit bounded offline HLOD job (native Windows Python, no resume).

Usage: python scripts/bake_gsplatworld_offline.py --manifest job.json --godot engine.exe
       --evidence NEW_DIRECTORY

The manifest is author-supplied and frozen before launch. Required fields:
schema_version=1; source={path,sha256,bytes,count,sh_degree};
implementation={source_sha,build_sha256}; output (must be a fresh name); limits (see REQUIRED_LIMITS);
quality must equal the complete frozen QUALITY dictionary below.
The first route admits binary little-endian scalar-vertex PLY and uncompressed v3
worlds without metadata/instances. This is bounded resident preparation, not V4.

Process observations follow the existing S1b native verifier's win_process.py.
Hard admission adds a suspended child and a Job Object before its first instruction.
Win32 contracts: learn.microsoft.com/en-us/windows/win32/procthread/job-objects
and /windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information.
"""

import argparse
import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes as W
from pathlib import Path


REQUIRED_LIMITS = (
    "job_wall_seconds", "batch_wall_seconds", "cancel_and_drain_seconds",
    "process_private_bytes", "minimum_free_host_physical_and_commit_bytes",
    "minimum_free_output_volume_bytes", "max_source_bytes", "max_input_splats",
    "max_prepared_splats", "max_nodes", "max_output_bytes", "max_scratch_bytes",
    "concurrent_jobs", "pending_jobs", "bake_workers", "image_payload_bytes",
    "metric_scratch_peak_bytes", "node_work", "bake_work",
)
FIXED_METRIC = {"image_payload_bytes": 1 << 26, "metric_scratch_peak_bytes": 1 << 28,
                "node_work": 1 << 28, "bake_work": 1 << 34}
QUALITY = {"bake_rule_version": 2, "views": 22, "dc_semantics": "preserve", "sh_semantics": "full",
           "leaf_max_splats": 16384, "node_max_splats": 16384, "merge_grid": 64,
           "reduction": 4, "eps_growth": 1.25, "max_eps_steps": 1024, "merged_preference": 0.01}
CONTROL_BYTES = 16 << 20  # bounded log, manifest/report, project and engine startup files
MAX_LOG_BYTES = 1 << 20


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path, check=lambda: None):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            check()
            digest.update(block)
    check()
    return digest.hexdigest()


def write_json(path, value):
    encoded = json.dumps(value, indent=2, allow_nan=False).encode() + b"\n"
    require(len(encoded) <= 1 << 20, "Control report exceeded its reservation")
    Path(path).write_bytes(encoded)


def validate_manifest(manifest):
    require(len(json.dumps(manifest, allow_nan=False).encode()) <= 65536, "Manifest control reservation")
    require(manifest.get("schema_version") == 1, "Unsupported manifest version")
    limits = manifest.get("limits", {})
    for name in REQUIRED_LIMITS:
        value = limits.get(name)
        require(type(value) is int and (value >= 0 if name == "pending_jobs" else value > 0)
                and value <= (1 << 53) - 1, "Missing/nonfinite/noninteger limit: " + name)
    require((limits["concurrent_jobs"], limits["pending_jobs"], limits["bake_workers"]) == (1, 0, 1),
            "Only one job/one worker/no pending queue is implemented")
    require(limits["job_wall_seconds"] <= limits["batch_wall_seconds"], "Job exceeds batch deadline")
    require(limits["max_input_splats"] <= limits["max_prepared_splats"] <= 0xffffffff
            and limits["max_nodes"] <= 0xffffffff, "Counts exceed the indexed representation")
    require(limits["max_scratch_bytes"] <= limits["process_private_bytes"], "Scratch exceeds process reservation")
    require(limits["max_source_bytes"] + limits["max_output_bytes"] + CONTROL_BYTES <= limits["max_scratch_bytes"],
            "Scratch disk reservation cannot hold a private input, staged output and controls")
    for name, maximum in FIXED_METRIC.items():
        require(limits[name] <= maximum, "Cannot raise the rule-2 metric limit: " + name)
    require(manifest.get("quality") == QUALITY, "Unrecognized quality rule or reduced-view request")
    source = manifest.get("source", {})
    require(type(source.get("count")) is int and 0 < source["count"] <= limits["max_input_splats"], "Input count limit")
    require(type(source.get("bytes")) is int and 0 < source["bytes"] <= limits["max_source_bytes"], "Source byte limit")
    require(type(source.get("sh_degree")) is int and 0 <= source["sh_degree"] <= 4, "SH0-4 required")
    for owner, key, length in ((source, "sha256", 64), (manifest.get("implementation", {}), "source_sha", 40),
                               (manifest.get("implementation", {}), "build_sha256", 64)):
        value = owner.get(key, "")
        require(isinstance(value, str) and len(value) == length and all(c in "0123456789abcdef" for c in value),
                "Missing exact identity: " + key)
    require(isinstance(source.get("path"), str) and isinstance(manifest.get("output"), str), "Missing paths")
    return limits


def refuse_alias(source, output):
    source, output = Path(source), Path(output)
    require(os.path.normcase(str(source.resolve())) != os.path.normcase(str(output.resolve())),
            "Source/output path alias")
    if output.exists():
        require(not os.path.samefile(source, output), "Source/output hardlink alias")
    require(not output.is_symlink(), "Output must not be a symlink")


def require_fresh_output(source, output):
    refuse_alias(source, output)
    require(not os.path.lexists(output), "Existing output is never replaced; select a fresh destination name")


def inspect_source(path, source, limits):
    """Small admission parser only. The engine still performs the complete strict load."""
    path = Path(path)
    require(path.stat().st_size == source["bytes"], "Source size identity mismatch")
    count, degree = source["count"], source["sh_degree"]
    decoded = count
    with path.open("rb") as stream:
        if path.suffix.lower() == ".ply":
            header = bytearray()
            while True:
                line = stream.readline(4097)
                require(line and len(line) <= 4096, "PLY admission header line bound")
                header.extend(line)
                require(len(header) <= 65536, "PLY admission header byte bound")
                if line.strip() == b"end_header":
                    break
            lines = header.decode("ascii").splitlines()
            require(lines[:2] == ["ply", "format binary_little_endian 1.0"], "Only binary little-endian PLY is admitted")
            elements = [line.split() for line in lines if line.startswith("element ")]
            require(elements == [["element", "vertex", str(count)]], "Only the frozen scalar vertex element is admitted")
            props = [line.split() for line in lines if line.startswith("property ")]
            sizes = {"char": 1, "uchar": 1, "int8": 1, "uint8": 1, "short": 2, "ushort": 2,
                     "int16": 2, "uint16": 2, "int": 4, "uint": 4, "int32": 4, "uint32": 4,
                     "float": 4, "float32": 4, "double": 8, "float64": 8}
            require(props and len(props) <= 128 and all(len(p) == 3 and p[1] in sizes for p in props), "PLY property bound/type")
            names = [p[2] for p in props]
            require(len(names) == len(set(names)), "Duplicate PLY property")
            require(set(("x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2")).issubset(names), "Missing position/DC")
            rest = {name for name in names if name.startswith("f_rest_")}
            require(rest == {"f_rest_" + str(i) for i in range(3 * ((degree + 1) ** 2 - 1))}, "PLY SH identity mismatch")
            require(len(header) + count * sum(sizes[p[1]] for p in props) == source["bytes"], "PLY physical extent mismatch")
            nodes = 0
        else:
            require(path.suffix.lower() == ".gsplatworld", "Unsupported source format")
            require(not Path(str(path) + ".import").exists(), "Imported source remaps are not admitted")
            header = stream.read(192)
            require(len(header) >= 112, "Truncated world header")
            magic, version, flags, actual, sh_degree, first, high = struct.unpack_from("<7I", header)
            require(magic == 0x57505347 and version == 3 and not flags & 0x11, "Require uncompressed v3 world without metadata")
            require((actual, sh_degree, first, high) == (count, degree, min(3, (degree + 1) ** 2 - 1),
                                                        max(0, (degree + 1) ** 2 - 4)), "World count/SH identity mismatch")
            require(struct.unpack_from("<I", header, 52)[0] == 0, "Only worlds without explicit chunk tables are admitted")
            require(struct.unpack_from("<Q", header, 96)[0] == 0, "World metadata byte limit")
            nodes = 0
            if flags & 64:
                require(len(header) == 192, "Truncated HLOD header")
                rule, interior = struct.unpack_from("<II", header, 136)
                nodes, top = struct.unpack_from("<II", header, 152)
                instances = struct.unpack_from("<I", header, 176)[0]
                require(rule in (1, 2) and not top and not instances, "Unsupported HLOD identity/instances")
                require(nodes <= limits["max_nodes"] and count + interior <= limits["max_prepared_splats"], "Source HLOD count limits")
                decoded += interior
    # PLY loader has a rounded bulk buffer, decoded storage, its snapshot and final copy.
    # Use SH4 storage even for lower-degree sources, plus bounded loader/node/control data.
    # This phase finishes before the C++ bake reservation; the two peaks do not overlap.
    bulk = 1 << (source["bytes"] - 1).bit_length()
    reserved = bulk + 4 * decoded * (144 + 21 * 12) + nodes * 2048 + CONTROL_BYTES
    require(reserved <= limits["max_scratch_bytes"], "Source-load allocation reservation exceeds scratch cap")
    return {"load_reserved_bytes": reserved, "source_nodes": nodes}


class MemoryStatus(ctypes.Structure):
    _fields_ = [("length", W.DWORD), ("load", W.DWORD)] + [(name, ctypes.c_ulonglong) for name in (
        "total_physical", "available_physical", "total_commit", "available_commit",
        "total_virtual", "available_virtual", "available_extended_virtual")]


class BasicLimit(ctypes.Structure):
    _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong), ("flags", W.DWORD),
                ("min_working_set", ctypes.c_size_t), ("max_working_set", ctypes.c_size_t),
                ("active_processes", W.DWORD), ("affinity", ctypes.c_size_t), ("priority", W.DWORD), ("scheduling", W.DWORD)]


class ExtendedLimit(ctypes.Structure):
    _fields_ = [("basic", BasicLimit), ("io", ctypes.c_ulonglong * 6)] + [(name, ctypes.c_size_t) for name in (
        "process_memory", "job_memory", "peak_process_memory", "peak_job_memory")]


class NativeJob:
    """Owned Windows process only. No signal/kill is ever sent to an unrelated PID."""
    def __init__(self, limits):
        require(os.name == "nt", "Bounded process admission requires native Windows Python")
        require(ctypes.sizeof(ctypes.c_void_p) == 8, "Bounded process admission requires 64-bit native Python")
        self.limits, self.handles, self.process, self.thread = limits, [], None, None
        self.attached = False
        self.pid = None
        self.drain = {"status": "NOT_NEEDED"}
        self.marker_owned = False
        self.publication_uncertain = False
        self.publication_started = False
        self.poison_path = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir())) / "GodotGS" / "offline-hlod-drain-failed.json"
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self.kernel
        for name, args, result in (
            ("CreateJobObjectW", [ctypes.c_void_p, W.LPCWSTR], W.HANDLE),
            ("SetInformationJobObject", [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD], W.BOOL),
            ("QueryInformationJobObject", [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD, ctypes.c_void_p], W.BOOL),
            ("AssignProcessToJobObject", [W.HANDLE, W.HANDLE], W.BOOL),
            ("TerminateJobObject", [W.HANDLE, W.UINT], W.BOOL),
            ("TerminateProcess", [W.HANDLE, W.UINT], W.BOOL),
            ("ResumeThread", [W.HANDLE], W.DWORD),
            ("WaitForSingleObject", [W.HANDLE, W.DWORD], W.DWORD),
            ("GetExitCodeProcess", [W.HANDLE, ctypes.POINTER(W.DWORD)], W.BOOL),
            ("GetProcessTimes", [W.HANDLE] + [ctypes.POINTER(W.FILETIME)] * 4, W.BOOL),
            ("CloseHandle", [W.HANDLE], W.BOOL),
            ("CreateMutexW", [ctypes.c_void_p, W.BOOL, W.LPCWSTR], W.HANDLE),
            ("CreateFileW", [W.LPCWSTR, W.DWORD, W.DWORD, ctypes.c_void_p, W.DWORD, W.DWORD, W.HANDLE], W.HANDLE),
            ("GlobalMemoryStatusEx", [ctypes.POINTER(MemoryStatus)], W.BOOL),
            ("CreatePipe", [ctypes.POINTER(W.HANDLE), ctypes.POINTER(W.HANDLE), ctypes.c_void_p, W.DWORD], W.BOOL),
            ("SetHandleInformation", [W.HANDLE, W.DWORD, W.DWORD], W.BOOL),
            ("PeekNamedPipe", [W.HANDLE, ctypes.c_void_p, W.DWORD, ctypes.c_void_p, ctypes.POINTER(W.DWORD), ctypes.c_void_p], W.BOOL),
            ("ReadFile", [W.HANDLE, ctypes.c_void_p, W.DWORD, ctypes.POINTER(W.DWORD), ctypes.c_void_p], W.BOOL),
        ):
            method = getattr(k, name)
            method.argtypes, method.restype = args, result
        self.job = None

    def must(self, value, operation):
        if not value:
            raise OSError(ctypes.get_last_error(), operation)
        return value

    def own(self, handle):
        self.must(handle and handle != ctypes.c_void_p(-1).value, "Create native handle")
        self.handles.append(handle)
        return handle

    def __enter__(self):
        try:
            require(not self.poison_path.exists(), "Previous job failed to drain; operator inspection required: " + str(self.poison_path))
            # No wait/queue: a second supervisor refuses admission immediately.
            ctypes.set_last_error(0)
            mutex = self.kernel.CreateMutexW(None, False, "Local\\GodotGSOfflineHlodSingleJobV1")
            already = ctypes.get_last_error() == 183
            self.own(mutex)
            require(not already, "Another offline job is active; queue capacity is zero")
            # Persist before any process exists. A supervisor crash or failed drain must
            # block later admission even if writing a failure report would itself fail.
            self.poison_path.parent.mkdir(parents=True, exist_ok=True)
            with self.poison_path.open("x", encoding="utf-8") as marker:
                self.marker_owned = True
                json.dump({"status": "ACTIVE_OR_UNDRAINED", "supervisor_pid": os.getpid()}, marker)
            self.job = self.own(self.kernel.CreateJobObjectW(None, None))
            info = ExtendedLimit()
            info.basic.flags = 0x8 | 0x100 | 0x200 | 0x2000  # active/process-memory/job-memory/kill-on-close
            info.basic.active_processes = 1
            info.process_memory = info.job_memory = self.limits["process_private_bytes"]
            self.must(self.kernel.SetInformationJobObject(self.job, 9, ctypes.byref(info), ctypes.sizeof(info)), "Set hard job limits")
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def lock_source(self, path):
        # Permit readers only: source cannot be written, renamed or removed during the job.
        # CreateHardLinkW can publish a fresh name while this lock also protects the
        # candidate path; no FILE_SHARE_DELETE or path-replacement window is needed.
        return self.own(self.kernel.CreateFileW(str(Path(path).resolve()), 0x80000000, 1, None, 3, 0x80, None))

    def host_memory(self):
        value = MemoryStatus()
        value.length = ctypes.sizeof(value)
        self.must(self.kernel.GlobalMemoryStatusEx(ctypes.byref(value)), "GlobalMemoryStatusEx")
        return {name: int(getattr(value, name)) for name in (
            "total_physical", "available_physical", "total_commit", "available_commit")}

    def peak(self):
        info = ExtendedLimit()
        self.must(self.kernel.QueryInformationJobObject(self.job, 9, ctypes.byref(info), ctypes.sizeof(info), None), "Query job peak")
        return int(info.peak_process_memory)

    def cpu_times(self):
        creation, exit_time, kernel, user = (W.FILETIME() for _ in range(4))
        self.must(self.kernel.GetProcessTimes(self.process, ctypes.byref(creation), ctypes.byref(exit_time),
                                              ctypes.byref(kernel), ctypes.byref(user)), "Get owned process CPU times")
        def seconds(value):
            return ((int(value.dwHighDateTime) << 32) | int(value.dwLowDateTime)) / 10000000
        return {"kernel_seconds": seconds(kernel), "user_seconds": seconds(user),
                "total_seconds": seconds(kernel) + seconds(user)}

    def launch(self, command, cwd):
        import _winapi
        read, write = W.HANDLE(), W.HANDLE()
        self.must(self.kernel.CreatePipe(ctypes.byref(read), ctypes.byref(write), None, 65536), "Create bounded log pipe")
        self.read_pipe, self.write_pipe = self.own(read.value), self.own(write.value)
        self.must(self.kernel.SetHandleInformation(self.write_pipe, 1, 1), "Inherit worker log pipe")
        stdin = self.own(self.kernel.CreateFileW("NUL", 0x80000000, 3, None, 3, 0x80, None))
        self.must(self.kernel.SetHandleInformation(stdin, 1, 1), "Inherit worker null input")
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESTDHANDLES
        startup.hStdInput, startup.hStdOutput, startup.hStdError = stdin, self.write_pipe, self.write_pipe
        startup.lpAttributeList = {"handle_list": [stdin, self.write_pipe]}
        process, thread, pid, _ = _winapi.CreateProcess(None, subprocess.list2cmdline(command), None, None, True,
                                                       0x4 | 0x08000000, None, str(cwd), startup)
        self.process, self.thread = self.own(process), self.own(thread)
        self.pid, self.attached = pid, False
        self.must(self.kernel.AssignProcessToJobObject(self.job, self.process), "Attach suspended worker to job")
        self.attached = True
        require(self.kernel.ResumeThread(self.thread) != 0xffffffff, "Resume owned worker")
        self.kernel.CloseHandle(self.write_pipe)
        self.handles.remove(self.write_pipe)
        self.write_pipe = None
        return pid

    def read_log(self):
        available = W.DWORD()
        if not self.kernel.PeekNamedPipe(self.read_pipe, None, 0, None, ctypes.byref(available), None):
            if ctypes.get_last_error() == 109:  # owned child closed pipe
                return b""
            self.must(False, "Peek owned log pipe")
        if not available.value:
            return b""
        buffer = ctypes.create_string_buffer(min(available.value, 65536))
        read = W.DWORD()
        self.must(self.kernel.ReadFile(self.read_pipe, buffer, len(buffer), ctypes.byref(read), None), "Read owned log pipe")
        return buffer.raw[:read.value]

    def poll(self):
        status = self.kernel.WaitForSingleObject(self.process, 0)
        if status == 258:
            return None
        require(status == 0, "Wait on owned process failed")
        code = W.DWORD()
        self.must(self.kernel.GetExitCodeProcess(self.process, ctypes.byref(code)), "Get owned process exit")
        return int(code.value)

    def stop(self):
        if self.process is not None and self.poll() is None:
            started = time.monotonic()
            if self.attached:
                self.kernel.TerminateJobObject(self.job, 1)
            else:
                self.kernel.TerminateProcess(self.process, 1)
            deadline = started + self.limits["cancel_and_drain_seconds"]
            while self.poll() is None and time.monotonic() < deadline:
                time.sleep(.05)
            drained = self.poll() is not None
            self.drain = {"status": "DRAINED" if drained else "FAILED", "pid": self.pid,
                          "seconds": time.monotonic() - started}
            if not drained:
                self.poison_path.parent.mkdir(parents=True, exist_ok=True)
                write_json(self.poison_path, self.drain)
                raise RuntimeError("Owned job exceeded cancellation/drain deadline; future admission blocked: " + str(self.poison_path))

    def __exit__(self, *_):
        drained = False
        try:
            self.stop()
            drained = self.process is None or self.poll() is not None
        finally:
            for handle in reversed(self.handles):
                if not self.kernel.CloseHandle(handle):
                    drained = False
            self.handles.clear()
            if not drained:
                self.drain["status"] = "FAILED"
                self.publication_uncertain = self.publication_uncertain or self.publication_started
            if self.marker_owned and self.publication_uncertain:
                write_json(self.poison_path, {"status": "UNCONFIRMED_PUBLICATION", "pid": self.pid,
                                              "drain": self.drain})
            elif self.marker_owned and drained and self.drain["status"] != "FAILED":
                self.poison_path.unlink()
                self.marker_owned = False
            if not drained:
                raise RuntimeError("Owned process exit or handle closure was not confirmed; admission remains blocked")


def publish_candidate(candidate, output, expected_hash, source, check_before_publish=lambda: None):
    """Called only after native strict validation, process success and final checks."""
    require(sha256(candidate, check_before_publish) == expected_hash, "Validated candidate identity changed")
    publish_verified_candidate(candidate, output, source, file_identity(candidate), check_before_publish)


def publish_verified_candidate(candidate, output, source, identity, check):
    """Atomic fresh-name publication, called inside the bounded verification worker."""
    require_fresh_output(source, output)
    require(file_identity(candidate) == identity, "Candidate identity changed before publication")
    check()
    # One complete, already validated file gains a new directory entry. This is an
    # atomic no-clobber operation (CreateHardLinkW on Windows); an existing/racing
    # target causes failure. Unsupported filesystems refuse. Retain the staging link
    # as evidence so no second operation is needed at the publication commit point.
    os.link(candidate, output)


def file_identity(path):
    value = Path(path).stat()
    return [value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns]


def control_identities(stage):
    """Only bounded control files; the supervisor never hashes source/output payloads."""
    identities = {}
    for name, limit in (("worker.gd", 65536), ("project.godot", 65536), ("request.json", 1 << 20)):
        path = Path(stage) / name
        require(path.stat().st_size <= limit, "Private control byte reservation: " + name)
        identities[name] = sha256(path)
    return identities


def verify_control_identity(stage, admitted):
    require(control_identities(stage) == admitted.get("control_sha256"), "Admitted private control identity changed")


def private_phase(phase, request_path):
    """Two private I/O phases, supervised exactly like the engine. No scheduler."""
    request_path = Path(request_path)
    require(request_path.stat().st_size <= 1 << 20, "Request reservation")
    request = json.loads(request_path.read_text(encoding="utf-8"))
    limits = validate_manifest(request)
    stage = request_path.parent
    result = {"status": "FAILED", "phase": phase}
    try:
        original = Path(request["original_source_path"])
        godot = Path(request["godot_path"])
        require(original.stat().st_size == request["source"]["bytes"], "Source physical byte identity")
        if phase == "admit":
            result["admission"] = inspect_source(original, request["source"], limits)
        require(sha256(original) == request["source"]["sha256"], "Source hash identity mismatch")
        require(sha256(godot) == request["implementation"]["build_sha256"], "Binary identity mismatch")
        if phase == "admit":
            private_source = Path(request["source"]["path"])
            if private_source != original:
                with original.open("rb") as read, private_source.open("xb") as write:
                    copied = 0
                    while block := read.read(1 << 20):
                        require(copied + len(block) <= limits["max_source_bytes"], "Private source-copy byte cap")
                        write.write(block)
                        copied += len(block)
                require(sha256(private_source) == request["source"]["sha256"], "Private source copy mismatch")
            result["control_sha256"] = control_identities(stage)
            result["supervisor_sha256"] = sha256(__file__)
        elif phase == "verify":
            admission_path = stage / "admit.json"
            require(admission_path.stat().st_size <= 1 << 20, "Admission receipt reservation")
            admitted = json.loads(admission_path.read_text(encoding="utf-8"))
            require(admitted.get("status") == "CONTROL_COMPLETED", "No admission receipt")
            verify_control_identity(stage, admitted)
            worker_path = stage / "result.json"
            require(worker_path.stat().st_size <= 1 << 20, "Worker report reservation")
            worker = json.loads(worker_path.read_text(encoding="utf-8"))
            require(worker.get("status") == "VALIDATED_CANDIDATE" and worker.get("error") == 0
                    and worker.get("preparation", {}).get("bake_rule_version") == 2, "No strict validation receipt")
            candidate = Path(request["candidate_path"])
            identity = file_identity(candidate)
            require(identity[2] <= limits["max_output_bytes"], "Output byte limit")
            require(sha256(candidate) == worker["candidate_sha256"], "Strictly validated candidate hash changed")
            require(file_identity(candidate) == identity, "Candidate changed during final hash")
            result.update(candidate_identity=identity, candidate_sha256=worker["candidate_sha256"], worker=worker)
            def before_publish():
                require(time.monotonic() < request["deadline_monotonic"], "Job deadline before publication")
                require(not Path(request["limits"]["cancel_path"]).exists(), "Cancelled before publication")
            publish_verified_candidate(candidate, request["output"], original, identity, before_publish)
            result["publication_committed"] = True
            result["published_identity"] = file_identity(request["output"])
            before_publish()
        else:
            raise ValueError("Unknown private phase")
        result["status"] = "PUBLISHED" if phase == "verify" else "CONTROL_COMPLETED"
    except BaseException as error:
        result["failure"] = repr(error)
        if result.get("publication_committed"):
            result["status"] = "UNCONFIRMED"
    write_json(stage / (phase + ".json"), result)
    return 0 if result["status"] in ("CONTROL_COMPLETED", "PUBLISHED") else 1


def run_owned_phase(job, phase, command, stage, log, record, check):
    """One of exactly three sequential owned processes; shared deadline, no queue."""
    check()
    phase_record = {"phase": phase, "command": command, "status": "FAILED"}
    record.setdefault("phases", []).append(phase_record)
    started = time.monotonic()
    phase_record["pid"] = job.launch(command, stage)
    phase_record["job_attached_before_resume"] = True
    try:
        while True:
            block = job.read_log()
            require(record["log_bytes"] + len(block) <= MAX_LOG_BYTES, "Worker logs exhausted their shared reservation")
            log.write(block)
            record["log_bytes"] += len(block)
            check()
            require(job.peak() <= job.limits["process_private_bytes"], "Hard process RAM limit exceeded")
            host = job.host_memory()
            require(min(host["available_physical"], host["available_commit"]) >=
                    job.limits["minimum_free_host_physical_and_commit_bytes"], "Host physical/commit headroom exhausted")
            phase_record["minimum_observed_free_physical_bytes"] = min(
                phase_record.get("minimum_observed_free_physical_bytes", host["available_physical"]), host["available_physical"])
            phase_record["minimum_observed_free_commit_bytes"] = min(
                phase_record.get("minimum_observed_free_commit_bytes", host["available_commit"]), host["available_commit"])
            code = job.poll()
            if code is not None:
                while block := job.read_log():
                    require(record["log_bytes"] + len(block) <= MAX_LOG_BYTES, "Worker logs exhausted their shared reservation")
                    log.write(block)
                    record["log_bytes"] += len(block)
                phase_record["exit"] = code
                require(code == 0, "Owned phase failed: " + phase)
                phase_record["status"] = "COMPLETED"
                break
            time.sleep(.05)
    finally:
        phase_record.update(wall_seconds=time.monotonic() - started, process_cpu=job.cpu_times(),
                            peak_job_process_commit_bytes=job.peak())
        log.flush()


def run_job(manifest, godot, evidence):
    # Starts before input paths/headers/hashes are processed. Every larger I/O step
    # runs in a private owned process under this same deadline, including verification.
    started = time.monotonic()
    limits = validate_manifest(manifest)
    source = Path(manifest["source"]["path"]).resolve(strict=True)
    output = Path(manifest["output"]).absolute()
    require(output.suffix.lower() == ".gsplatworld" and output.parent.is_dir(), "Output directory/extension")
    require_fresh_output(source, output)
    godot = Path(godot).resolve(strict=True)
    require(os.name == "nt", "Unsupported platform: no hard owned-process enforcement")
    evidence = Path(evidence)
    evidence.mkdir(parents=True, exist_ok=False)
    record = {"status": "FAILED", "manifest": manifest, "log_bytes": 0,
              "python": {"path": sys.executable, "version": sys.version},
              "scope": "CPU offline preparation only; quality/runtime acceptance separate",
              "deadline_scope": "One shared job deadline from first source processing through admission/hash/copy, engine, final hash and atomic fresh-name publication; batch deadline is an additional cap"}
    stage = job = None
    try:
        def check():
            elapsed = time.monotonic() - started
            require(elapsed < limits["job_wall_seconds"], "Complete job wall deadline")
            require(elapsed < limits["batch_wall_seconds"], "Additional batch wall deadline")
            require(stage is None or not (stage / "cancel").exists(), "Job cancelled")
        with NativeJob(limits) as job:
            check()
            worker = Path(__file__).with_suffix(".gd")
            for path in (source, godot, Path(__file__).resolve(), worker):
                job.lock_source(path)
            host = job.host_memory()
            require(min(host["available_physical"], host["available_commit"]) >=
                    limits["minimum_free_host_physical_and_commit_bytes"] + limits["process_private_bytes"],
                    "Free host memory cannot reserve the process cap plus required headroom")
            free_disk = shutil.disk_usage(output.parent).free
            require(free_disk >= limits["minimum_free_output_volume_bytes"] + limits["max_scratch_bytes"],
                    "Output volume cannot reserve scratch plus required headroom")
            record.update(host=host, output_volume_free_bytes=free_disk, hardware={
                "logical_cpus": os.cpu_count(), "machine": os.environ.get("PROCESSOR_IDENTIFIER", "")})
            stage = Path(tempfile.mkdtemp(prefix=".hlod-job-", dir=output.parent))
            candidate = stage / "candidate.gsplatworld"
            cancel = stage / "cancel"
            require(worker.stat().st_size <= 65536, "Private worker script reservation")
            shutil.copyfile(worker, stage / "worker.gd")
            (stage / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Bounded HLOD offline job"\n'
                '[threading]\nworker_pool/max_threads=1\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
                'gaussian_splatting/import/use_gsplatworld_cache=false\n', encoding="utf-8")
            request = json.loads(json.dumps(manifest))
            request["original_source_path"] = str(source)
            request["godot_path"] = str(godot)
            request["output"] = str(output)
            request["deadline_monotonic"] = started + min(limits["job_wall_seconds"], limits["batch_wall_seconds"])
            request["source"]["path"] = str(stage / "input.gsplatworld" if source.suffix.lower() == ".gsplatworld" else source)
            request["limits"]["cancel_path"] = str(cancel)
            request["candidate_path"] = str(candidate)
            request_path = stage / "request.json"
            write_json(request_path, request)
            job.lock_source(request_path)
            job.lock_source(stage / "worker.gd")
            job.lock_source(stage / "project.godot")
            record.update(stage=str(stage), cancel_file=str(cancel))
            write_json(evidence / "manifest.json", record)
            with (evidence / "engine.log").open("wb") as log:
                control_command = [sys.executable, str(Path(__file__).resolve()), "--request", str(request_path), "--private-phase"]
                run_owned_phase(job, "admit", control_command + ["admit"], stage, log, record, check)
                admission_path = stage / "admit.json"
                job.lock_source(admission_path)
                require(admission_path.stat().st_size <= 1 << 20, "Admission receipt reservation")
                admission = json.loads(admission_path.read_text(encoding="utf-8"))
                require(admission.get("status") == "CONTROL_COMPLETED", "Admission phase has no valid receipt")
                verify_control_identity(stage, admission)
                record["admission"] = admission
                if source.suffix.lower() == ".gsplatworld":
                    job.lock_source(request["source"]["path"])
                engine = [str(godot), "--headless", "--path", str(stage), "--script", "res://worker.gd"]
                run_owned_phase(job, "engine", engine, stage, log, record, check)
                # Disallow candidate writes/deletion/replacement through publication.
                job.lock_source(candidate)
                job.lock_source(stage / "result.json")
                job.publication_started = True
                job.publication_uncertain = True
                run_owned_phase(job, "verify", control_command + ["verify"], stage, log, record, check)
            verify_path = stage / "verify.json"
            require(verify_path.stat().st_size <= 1 << 20, "Verification receipt reservation")
            verified = json.loads(verify_path.read_text(encoding="utf-8"))
            require(verified.get("status") == "PUBLISHED" and verified.get("publication_committed") is True,
                    "Missing publication receipt")
            require(file_identity(output) == verified["published_identity"], "Published identity mismatch")
            check()
            record.update(worker=verified["worker"], output_sha256=verified["candidate_sha256"],
                          output_bytes=verified["candidate_identity"][2], peak_job_process_commit_bytes=job.peak())
            job.publication_uncertain = False
            record["status"] = "PUBLISHED"
    except BaseException as error:
        record["failure"] = repr(error)
        if job is not None and job.publication_uncertain:
            record["status"] = "UNCONFIRMED"
    finally:
        record["elapsed_seconds"] = time.monotonic() - started
        if job is not None:
            record["drain"] = job.drain
        write_json(evidence / "result.json", record)
        # Each attempt keeps its own stage as evidence and restarts fresh. Partial files
        # are never checkpoint inputs. A failed drain additionally poisons future admission.
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--godot", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--private-phase", choices=("admit", "verify"), help=argparse.SUPPRESS)
    parser.add_argument("--request", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.private_phase:
        require(args.request is not None, "Private phase requires an owned request")
        return private_phase(args.private_phase, args.request)
    require(args.manifest is not None and args.godot is not None and args.evidence is not None,
            "Required: --manifest, --godot, --evidence")
    require(args.manifest.stat().st_size <= 65536, "Manifest control reservation")
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    record = run_job(manifest, args.godot, args.evidence)
    print(json.dumps({"status": record["status"], "evidence": str(args.evidence), "failure": record.get("failure")}))
    return 0 if record["status"] == "PUBLISHED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
