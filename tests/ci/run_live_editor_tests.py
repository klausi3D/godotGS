"""Strict original Editor lane in a disposable genuine EditorNode runtime.

Local native entry is deliberately unavailable until an enclosing lease launcher
is reviewed. Synthetic protocol controls are not captured native producer proof.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import queue
import re
import stat
import subprocess
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = '*GaussianSplatting*][Editor]*'
EXCLUDE = '*][RequiresGPU]*'
PREFIX = '[GS-LIVE-EDITOR] '
INPUT_RECIPE = 'gs-live-editor-inputs-v1'
INPUT_NAMES = ('test_splats.ply', 'test_splats.spz')
RATIONALE = ('GaussianThumbnailGenerator::_project_to_canvas hits: injected allocation failure '
             '(test seam); leaving the output empty instead of writing out of bounds.')
HEADERS = ('test_gaussian_importer.h', 'test_gaussian_splat_node.h', 'test_gaussian_thumbnail_generator.h')


def selected_cases(root=ROOT):
    names = []
    for header in HEADERS:
        text = (root / 'modules/gaussian_splatting/tests' / header).read_text(encoding='utf-8')
        names.extend(n for n in re.findall(r'TEST_CASE\("([^"\n]+)"\)', text)
                     if '[GaussianSplatting]' in n and '[Editor]' in n and '[RequiresGPU]' not in n)
    if len(names) != 23 or len(set(names)) != 23:
        raise ValueError('Original strict Editor inventory changed')
    return names



def fixture_cases(root=ROOT):
    """Derive the seven actual factory consumers from the original test source."""
    source = (root / 'modules/gaussian_splatting/tests/test_gaussian_importer.h').read_text(encoding='utf-8')
    fixtures = set()
    for part in source.split('TEST_CASE("')[1:]:
        name = part.split('"', 1)[0]
        if 'GSLiveEditorFixtureLease live_fixture;' in part:
            fixtures.add(name)
    if len(fixtures) != 7 or not fixtures.issubset(set(selected_cases(root))):
        raise ValueError('Original seven dialog fixture consumers changed')
    return fixtures


def _validate_input_rows(rows):
    if not isinstance(rows, list) or len(rows) != 2:
        raise ValueError('Missing/duplicate native input inventory')
    for row, name, producer in zip(rows, INPUT_NAMES, ('write_gaussian_ply', 'write_synthetic_spz')):
        if (not isinstance(row, dict) or set(row) != {'path', 'format', 'producer', 'bytes', 'sha256', 'count', 'loader_ok', 'values_ok'}
                or row.get('path') != name or row.get('format') != name.rsplit('.', 1)[1] or row.get('producer') != producer
                or type(row.get('bytes')) is not int or not 1 <= row['bytes'] <= 65536
                or type(row.get('count')) is not int or row['count'] != 3
                or row.get('loader_ok') is not True or row.get('values_ok') is not True
                or not isinstance(row.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', row['sha256'])):
            raise ValueError('Invalid native writer/loader input premise')
    return rows


def _input_path_is_plain(info):
    attributes = getattr(info, 'st_file_attributes', None if os.name == 'nt' else 0)
    return type(attributes) is int and not (attributes & 0x400) and not stat.S_ISLNK(info.st_mode)


def _physical_input_fingerprints(project):
    # Open only these parent-owned names, never a producer-supplied path.
    project = Path(project)
    for ancestor in (project, *project.parents):
        info = ancestor.lstat()
        if not _input_path_is_plain(info) or not stat.S_ISDIR(info.st_mode):
            raise ValueError('Unsafe/missing owned input project')
    fingerprints = []
    for name in INPUT_NAMES:
        path = project / name
        before = path.lstat()
        if not _input_path_is_plain(before) or not stat.S_ISREG(before.st_mode) or not 1 <= before.st_size <= 65536:
            raise ValueError('Unsafe/missing/bounded native input file')
        with path.open('rb') as file:
            opened = os.fstat(file.fileno())
            data = file.read(65537)
            closed = os.fstat(file.fileno())
        after = path.lstat()
        identities = [(x.st_size, x.st_mtime_ns, x.st_ino, x.st_dev, x.st_ctime_ns) for x in (before, opened, closed, after)]
        if (any(not _input_path_is_plain(x) or not stat.S_ISREG(x.st_mode) for x in (opened, closed, after)) or any(x != identities[0] for x in identities)
                or len(data) != before.st_size):
            raise ValueError('Changed native input file during parent read')
        fingerprints.append({'path': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    return fingerprints


def _validate_live_editor_inputs(records, cases, final, fingerprints):
    stages = [x for x in records if x.get('kind') == 'input-fixtures']
    if (len(stages) != 1 or len(records) < 3 or records[1] is not stages[0]
            or set(stages[0]) != {'kind', 'recipe', 'files', 'token', 'pid'} or stages[0].get('recipe') != INPUT_RECIPE):
        raise ValueError('Missing/duplicate/late native input preparation')
    rows = _validate_input_rows(stages[0].get('files'))
    expected = [{'path': r['path'], 'bytes': r['bytes'], 'sha256': r['sha256']} for r in rows]
    if fingerprints != expected:
        raise ValueError('Parent physical inputs differ from native preparation')
    for closure in [*cases, final]:
        if closure.get('inputs_valid') is not True or closure.get('input_files') != rows:
            raise ValueError('Missing/changed native input case/final closure')
        _validate_input_rows(closure['input_files'])


def _positive(value):
    return isinstance(value, str) and value.isascii() and value.isdigit() and int(value) > 0


def _validate_live_editor_cases(cases, names):
    if len(cases) != 23 or set(x.get('name') for x in cases) != set(names) or [x.get('ordinal') for x in cases] != list(range(1, 24)):
        raise ValueError('Missing/duplicate/original case coverage')
    expected_fixtures = fixture_cases()
    for case in cases:
        if not isinstance(case.get('assertions'), int) or case['assertions'] < 1 or case.get('failed') != 0 or any(case.get(k) is not True for k in ('borrowed_valid', 'logger_before_restore', 'callback_closed')):
            raise ValueError('Hollow/failed/unsafe original case')
        if 'Advanced import settings reimport' in case['name'] and case['assertions'] < 21:
            raise ValueError('Original advanced assertions missing')
        fixture = case.get('fixture_created')
        if fixture != int(case['name'] in expected_fixtures) or case.get('fixture_closed') != fixture or (fixture and case.get('fixture_parented') is not True):
            raise ValueError('Unclosed/unparented fixture')
    if sum(x['fixture_created'] for x in cases) != 7:
        raise ValueError('Seven original fixtures not proven')


def _validate_live_editor_event_catalogue(events):
    if [x.get('sequence') for x in events] != list(range(1, len(events) + 1)):
        raise ValueError('Dropped/duplicate/reset records')
    for event in events:
        if event.get('event') not in range(1, 10) or not _positive(event.get('thread')):
            raise ValueError('Unknown event/thread')
        if event['event'] == 1 and event.get('expected') is not True and event.get('value') != 1:
            raise ValueError('Unexpected core error, including suppressed worker error')
    if any(sum(e['event'] == kind for e in events) != 2 for kind in range(2, 10)):
        raise ValueError('Unexpected/reset/missing source-scoped cohort count')


def _validate_live_editor_injection(events, cases, owner, injection_line):
    thumbnail = [c for c in cases if 'A failed thumbnail canvas allocation' in c['name']]
    if len(thumbnail) != 1:
        raise ValueError('Original injection case missing')
    tid = thumbnail[0]['ordinal']
    expected = [e for e in events if e['event'] == 1 and e.get('expected') is True]
    if len(expected) != 2:
        raise ValueError('Missing/disconnected/duplicate original error observation')
    for phase in (1, 2):
        arms = [e for e in events if e['event'] == 2 and e.get('arm') == str(phase)]
        scoped = [e for e in events if e.get('case') == tid and e.get('phase') == phase]
        chain = []
        for kind in (4, 3, 1, 5):
            found = [e for e in scoped if e['event'] == kind]
            if len(found) != 1:
                raise ValueError('Missing/reset injection cohort')
            chain.append(found[0])
        if len(arms) != 1 or arms[0].get('case') != tid or arms[0]['thread'] != owner or arms[0].get('message') != 'GaussianThumbnailGenerator::_project_to_canvas hits':
            raise ValueError('Actual arm attribution mismatch')
        if [arms[0]['sequence']] + [e['sequence'] for e in chain] != sorted([arms[0]['sequence']] + [e['sequence'] for e in chain]):
            raise ValueError('Injection chronology mismatch')
        first = chain[0]
        if first.get('value') != 16 or not all(_positive(first.get(k)) for k in ('generator', 'asset', 'token_id')) or first.get('token_id') != str(phase):
            raise ValueError('Actual invocation/input premise missing')
        for e in chain:
            if e['thread'] != owner or e.get('arm') != str(phase) or any(e.get(k) != first[k] for k in ('generator', 'asset', 'token_id')):
                raise ValueError('Cross-thread/object/reset injection')
        error = chain[2]
        if (error.get('expected') is not True or error.get('value') != 0 or error.get('notify') is not False
                or error.get('function') != 'gs_resize_or_fail' or error.get('line') != injection_line
                or not str(error.get('file', '')).replace('\\', '/').endswith('modules/gaussian_splatting/core/gs_vector_alloc.h')
                or error.get('error') != 'Method/function failed. Returning: false' or error.get('message') != RATIONALE):
            raise ValueError('Original source-derived error identity mismatch')


def _validate_live_editor_previews(cases, events, owner):
    previews = [c for c in cases if '(#592)' in c['name']]
    if len(previews) != 2:
        raise ValueError('Original preview cases absent')
    for case in previews:
        chain = []
        for kind in (6, 9, 7, 8):
            found = [e for e in events if e.get('case') == case['ordinal'] and e['event'] == kind]
            if len(found) != 1:
                raise ValueError('Missing/reset original preview cohort')
            chain.append(found[0])
        schedule, join, entry, complete = chain
        if (schedule.get('value') != 0 or schedule['thread'] == owner or join.get('token_id') != schedule['thread']
                or any(e['thread'] != owner for e in (join, entry, complete))
                or not _positive(schedule.get('generator')) or not _positive(schedule.get('token_id'))
                or any(e.get('generator') != schedule['generator'] for e in chain)
                or entry.get('token_id') != schedule['token_id'] or complete.get('token_id') != schedule['token_id']
                or [e['sequence'] for e in chain] != sorted(e['sequence'] for e in chain)):
            raise ValueError('Actual worker/join/callback delivery mismatch')
    if any(e['event'] >= 6 and e.get('case') not in {c['ordinal'] for c in previews} for e in events):
        raise ValueError('Unknown late preview cohort')


def _validate_live_editor_final(output, final, ids, cases):
    elapsed = final.get('drain_elapsed_usec')
    if not isinstance(elapsed, str) or not elapsed.isascii() or not elapsed.isdigit() or int(elapsed) > 45_000_000:
        raise ValueError('Missing/late original final-drain budget')
    if (any(final.get(k) is not True for k in ('valid', 'drained', 'logger_restored', 'borrowed_valid'))
            or final.get('borrowed_ids') != ids or final.get('cases') != 23 or final.get('created') != 7 or final.get('closed') != 7
            or final.get('failed_assertions') != 0 or final.get('assertions') != sum(c['assertions'] for c in cases)):
        raise ValueError('Final closure/identity/count reconciliation failed')
    # Doctest's own measured summary is a second independent count surface.
    summary = re.findall(r'test cases:\s*23\s*\|\s*23\s*passed\s*\|\s*0\s*failed', output)
    asserts = re.findall(r'assertions:\s*(\d+)\s*\|\s*(\d+)\s*passed\s*\|\s*0\s*failed', output)
    if len(summary) != 1 or len(asserts) != 1 or tuple(map(int, asserts[0])) != (final['assertions'], final['assertions']):
        raise ValueError('Actual doctest summary does not reconcile')
    if re.search(r'(?im)(?:^|\s)(?:ERROR:|SCRIPT ERROR:|SHADER ERROR:|FATAL:|Skipping\b|Segmentation fault|recursive error)', output):
        raise ValueError('Raw error/skip/fatal after complete EOF')


def _collect_live_editor_output(q, proc, readers, directory, timeout):
    deadline = time.monotonic() + timeout
    ended = set()
    output = []
    invalid = ''
    with (directory / 'stdout.log').open('w', encoding='utf-8') as stdout, (directory / 'stderr.log').open('w', encoding='utf-8') as stderr:
        while len(ended) != 2 or proc.poll() is None:
            if time.monotonic() >= deadline:
                invalid = 'Owned Editor watchdog expired; no lifetime/semantic proof'
                proc.kill()  # Only the exact Popen-owned child; never a foreign PID.
                break
            try:
                label, line = q.get(timeout=0.1)
            except queue.Empty:
                continue
            if label == 'invalid':
                invalid = line
                continue
            if line is None:
                ended.add(label)
            else:
                (stdout if label == 'stdout' else stderr).write(line)
                output.append(line)
        proc.wait(timeout=10)
        for reader in readers:
            reader.join(timeout=10)
        # Retain all bytes already read after termination; incomplete pipe closure is INVALID.
        while not q.empty():
            label, line = q.get_nowait()
            if label == 'invalid': invalid = line
            elif line is None: ended.add(label)
            else:
                (stdout if label == 'stdout' else stderr).write(line)
                output.append(line)
    text = ''.join(output)
    return text, ended, invalid


def _reconcile_live_editor_output(text, ended, invalid, readers, proc, token, names, injection_line, directory):
    if invalid or len(ended) != 2 or any(r.is_alive() for r in readers):
        return False, text + '\n[GS-LIVE-EDITOR INVALID] ' + (invalid or 'incomplete EOF'), proc.returncode
    try:
        if proc.returncode != 0:
            raise ValueError(f'Native child exit {proc.returncode}')
        validate(text, token=token, pid=proc.pid, names=names, injection_line=injection_line,
                 input_fingerprints=_physical_input_fingerprints(directory / 'project'))
    except (ValueError, TypeError, KeyError, OSError, json.JSONDecodeError) as exc:
        return False, text + '\n[GS-LIVE-EDITOR INVALID] ' + str(exc), proc.returncode
    return True, text, proc.returncode


def validate(output, *, token, pid, names, injection_line, input_fingerprints):
    """Reconcile complete real receipts; never infer delivery from cancellation."""
    if '[GS-LIVE-EDITOR INVALID]' in output:
        raise ValueError('Explicit invalid producer closure')
    records = []
    for line in output.splitlines():
        if PREFIX not in line:
            continue
        if not line.startswith(PREFIX):
            raise ValueError('Malformed receipt prefix')
        data = json.loads(line[len(PREFIX):])
        if data.get('token') != token or data.get('pid') != pid:
            raise ValueError('Wrong child/token receipt')
        records.append(data)
    begins = [x for x in records if x.get('kind') == 'begin']
    finals = [x for x in records if x.get('kind') == 'final']
    cases = [x for x in records if x.get('kind') == 'case']
    events = [x for x in records if x.get('kind') == 'event']
    if len(begins) != 1 or len(finals) != 1 or not records or records[0] != begins[0] or records[-1] != finals[0]:
        raise ValueError('Missing/duplicate/nonterminal closure')
    if any(x.get('kind') not in ('begin', 'input-fixtures', 'case', 'event', 'final') for x in records):
        raise ValueError('Unknown/invalid nonterminal receipt')
    begin, final = begins[0], finals[0]
    owner = begin.get('owner')
    ids = begin.get('borrowed_ids')
    if not _positive(owner) or not isinstance(ids, list) or len(ids) != 14 or not all(map(_positive, ids)) or len(set(ids)) != 14:
        raise ValueError('Invalid owner/borrowed identities')
    if begin.get('logger_captured') is not True or begin.get('queues_not_flushing') is not True:
        raise ValueError('Unsafe entry/logger')
    _validate_live_editor_inputs(records, cases, final, input_fingerprints)
    _validate_live_editor_cases(cases, names)
    _validate_live_editor_event_catalogue(events)
    _validate_live_editor_injection(events, cases, owner, injection_line)
    _validate_live_editor_previews(cases, events, owner)
    _validate_live_editor_final(output, final, ids, cases)
    return final


def supervise(command, directory, token, names, injection_line, timeout=900):
    """Own child only; bounded streaming and full EOF are mandatory."""
    q = queue.Queue()
    proc = subprocess.Popen(command, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding='utf-8', errors='strict', env={**os.environ, 'PYTHONUTF8': '1', 'PYTHONIOENCODING': 'utf-8'})
    try:
        def pump(stream, label):
            try:
                for line in stream:
                    q.put((label, line))
            except (OSError, UnicodeError) as exc:
                q.put(('invalid', repr(exc)))
            finally:
                stream.close()
                q.put((label, None))
        readers = [threading.Thread(target=pump, args=(proc.stdout, 'stdout'), daemon=True),
                   threading.Thread(target=pump, args=(proc.stderr, 'stderr'), daemon=True)]
        for reader in readers:
            reader.start()
        text, ended, invalid = _collect_live_editor_output(q, proc, readers, directory, timeout)
        return _reconcile_live_editor_output(text, ended, invalid, readers, proc, token, names, injection_line, directory)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=10)


def run(godot, original_args):
    # No direct local CLI/lease bypass. Hosted CI retains its existing enclosing
    # runner ownership policy and the canonical same-parent contention observer.
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        return False, '[GS-LIVE-EDITOR INVALID] approved local lease launcher unavailable; no child launched', None
    if Path(tempfile.gettempdir()).resolve().drive.upper() not in ('C:', 'Z:'):
        return False, '[GS-LIVE-EDITOR INVALID] disposable project must be on C/Z', None
    expected = ['--headless', '--test', f'--test-case={INCLUDE}', f'--test-case-exclude={EXCLUDE}']
    if list(original_args) != expected:
        return False, '[GS-LIVE-EDITOR INVALID] canonical Editor selection changed', None
    import runner_gpu_contention as contention
    binary = Path(godot).resolve(strict=True)
    if binary.drive.upper() not in ('C:', 'Z:'):
        return False, '[GS-LIVE-EDITOR INVALID] binary must be on C/Z', None
    def git(*args):
        return subprocess.run(['git', '--no-optional-locks', '-C', str(ROOT), *args], check=True, capture_output=True, text=True, encoding='utf-8', timeout=30).stdout.strip()
    anchor = git('rev-parse', 'HEAD')
    if not re.fullmatch(r'[0-9a-f]{40}', anchor) or git('status', '--porcelain', '--untracked-files=no'):
        return False, '[GS-LIVE-EDITOR INVALID] source must be frozen clean HEAD', None
    before = hashlib.sha256(binary.read_bytes()).hexdigest()
    names = selected_cases()
    header = ROOT / 'modules/gaussian_splatting/core/gs_vector_alloc.h'
    line = next(i for i, s in enumerate(header.read_text(encoding='utf-8').splitlines(), 1) if 'GS_LIVE_EDITOR_INJECTED_ERROR(false,' in s)
    directory = Path(tempfile.mkdtemp(prefix='godotgs-live-editor-'))
    project = directory / 'project'
    addon = project / 'addons/live_editor'
    addon.mkdir(parents=True)
    (project / 'project.godot').write_text('[application]\nconfig/name="GodotGS isolated Editor test producer"\nconfig/use_custom_user_dir=true\nconfig/custom_user_dir_name="gs_live_editor_' + directory.name + '"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/live_editor/plugin.cfg")\n', encoding='utf-8')
    (addon / 'plugin.cfg').write_text('[plugin]\nname="Live editor tests"\ndescription="Owned tests-only runtime"\nauthor="GodotGS"\nversion="1"\nscript="plugin.gd"\n', encoding='utf-8')
    (addon / 'plugin.gd').write_text('@tool\nextends EditorPlugin\nvar runner: RefCounted\nfunc _enter_tree():\n\tif not ClassDB.class_exists("GSLiveEditorTestRunner"):\n\t\tpush_error("TESTS+TOOLS live runner missing")\n\t\tget_tree().quit(1)\n\t\treturn\n\trunner = ClassDB.instantiate("GSLiveEditorTestRunner")\n\tif not runner or not runner.arm():\n\t\tpush_error("Live runner could not arm")\n\t\tget_tree().quit(1)\n', encoding='utf-8')
    if any((project / name).exists() or (project / name).is_symlink() for name in INPUT_NAMES):
        return False, '[GS-LIVE-EDITOR INVALID] input names are not fresh', None
    token = uuid.uuid4().hex
    command = [str(binary), '--editor', '--path', str(project), '--rendering-method', 'forward_plus', '--rendering-driver', 'vulkan', '--', '--gs-live-editor-tests', f'--test-case={INCLUDE}', f'--test-case-exclude={EXCLUDE}', f'--gs-live-editor-token={token}']
    metadata = {'command': command, 'binary_sha256': before, 'parent_pid': os.getpid(), 'token': token, 'original_names': names, 'directory': str(directory), 'source_head': anchor, 'input_recipe': INPUT_RECIPE, 'input_names': list(INPUT_NAMES)}
    (directory / 'invocation.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    record_dir = directory / 'contention'
    options = ['--record-dir', str(record_dir), '--job-root-pid', str(os.getpid())]
    if contention.main(['preflight', *options]) != 0:
        return False, '[GS-LIVE-EDITOR INVALID] canonical preflight refused; artifacts=' + str(directory), None
    try:
        version = subprocess.run([str(binary), '--version'], capture_output=True, text=True, encoding='utf-8', errors='strict', timeout=30)
        (directory / 'binary-version.txt').write_text(version.stdout + version.stderr, encoding='utf-8')
        if version.returncode != 0 or not re.search(r'(?:^|[.\s])' + re.escape(anchor[:9]) + r'(?:[.\s]|$)', version.stdout):
            result = (False, '[GS-LIVE-EDITOR INVALID] binary version is not source HEAD', version.returncode)
        else:
            result = supervise(command, directory, token, names, line)
    finally:
        post = contention.main(['postflight', *options])
    if post != 0 or git('rev-parse', 'HEAD') != anchor or git('status', '--porcelain', '--untracked-files=no') or hashlib.sha256(binary.read_bytes()).hexdigest() != before:
        return False, result[1] + '\n[GS-LIVE-EDITOR INVALID] postflight or binary source fence failed', result[2]
    return result
