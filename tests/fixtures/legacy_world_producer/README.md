# Captured legacy world producer fixtures

These fixtures were captured natively from the pinned producer. The actual capture passed one selected case and 126 assertions; the exact identities and SHA256 values are in `provenance.json`.

The six fixtures are emitted by the production plain-v1 (104-byte header) and HLOD-v2 (184-byte header) savers at pinned source 012960050d29427bd00a42e1cd86840806d6bf5c. The writer, Gaussian ABI, atomic publisher, HLOD bake/tree/merge and staged payload source set (17 files) is byte-identical to 8e3aa741ea3b54ed8c127a153fe8c3c7f5451e3a. The isolated producer commits only a capture test helper, at 8aede40654d86896429e11bca05ddaa1b5563480; its production code is unchanged. The rebuilt producer binary is a test-instrumented native binary, not the untouched earlier 8e3 executable.

Each world contains two original leaves with distinct painterly IDs 23/24, flags 5, positions (4,5,6)/(5,5,6), opacity 0.75, scale (0.25,0.5,0.75), color (0.25,0.5,0.75), normal (0,1,0), and brush axes (1,1). HLOD fixtures are baked through the actual production bake. The capture helper asserts the old reader decodes both original leaves; HLOD leaf order is identified by metadata rather than assumed.

For each schema, the streamable file uses actual saver.save(), resident uses save_resident_uncompressed(), and compressed uses save_resident_compressed(). Generic ResourceSaver flags do not select compression. Header version, HLOD bit, compressed bit, resident bit and Gaussian section offset are asserted for all six outputs. No header has been patched to create a positive legacy fixture.

The provenance manifest contains the exact base/helper tree, helper/patch/source hashes, native binary hashes/version, effective MSVC/SDK/build flags, successful build/capture exit, executed case/assertion counts, and six fixture SHA256 values. Logs remain external evidence artifacts. The consumer tests require all six fixture files and fail when any is missing; malformed cases make separately labeled mutations of these actual captures.

Native producer proof establishes historical writer output. Candidate compatibility, editor import/remap, streaming/GPU qualification and performance gates require their own runs and review. None follows merely from these files or the producer capture.

The exact compiled capture helper is retained as `capture_producer.h` for reproducibility and SHA256 verification. It is a producer fixture-generation input and is not registered in the candidate test suite.
