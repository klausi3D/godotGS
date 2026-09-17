extends RefCounted
## Sub-pixel displacement measurement for temporal-stage QA (#929).
##
## ## WHAT IT MEASURES AND WHY NOT SOMETHING SIMPLER
##
## "Splats swim under FSR2" is a statement about POSITION, and every cheaper
## proxy for it was measured and rejected on real captures before this existed:
##
##   * **Frame-to-frame colour delta** (mean |LSB| between consecutive frames)
##     confounds displacement with local contrast. On the real-scan rig the
##     defective splat region measured 1.089 while the CORRECTLY jittered mesh
##     control beside it measured 2.365 -- the broken layer scored BETTER,
##     because a 2 px checker has a far larger gradient than a soft scan. A gate
##     on that number, in either direction, would be measuring content.
##
##   * **Autocorrelation at the engine's jitter phase**, which is how #929's
##     assessment ATTRIBUTED the defect, is not an oracle for it. Measured on the
##     same captures, the splat region autocorrelates +1.000 at lag 8 -- and so
##     does the mesh control, +1.000. FSR2's resolve is phase-periodic under a
##     static camera whether or not the layer feeding it is jittered, so the
##     statistic cannot separate correct from incorrect integration. It is
##     reported here as a diagnostic and is deliberately NOT gated.
##
## What does separate is the displacement itself, in pixels. A first-order
## (Lucas-Kanade) estimate divides the temporal difference by the spatial
## gradient, so contrast cancels:
##
##     I(x + d) ~ I(x) + d . grad(I)   =>   [Sxx Sxy; Sxy Syy] d = [bx; by]
##
## with Sxx = sum gx^2, Sxy = sum gx*gy, Syy = sum gy^2 over the region and
## bx = sum gx*dI, by = sum gy*dI against the window-mean frame. Measured on the
## unfixed tree at 960x540, FSR2 scale 1.0:
##
##     splat region  |d| mean 0.2525 px (max 0.3945)
##     mesh control  |d| mean 0.0100 px (max 0.0198)     -> 25x
##
## and at scale 0.5, 0.3603 px vs 0.0046 px -> 78x. Under TAA and with no
## temporal stage the splat region measures exactly 0.0000 px. With the jitter
## applied the same rig measures 0.0900 px and 0.0927 px -- a clean separation
## from the defect, and still well above the control, because splats publish no
## depth write-back, no motion vectors and no reactive mask either.
##
## The caller decides the threshold; this file only measures. See
## `scenes/qa/qa_composite_production_defaults.gd` (SWIM_MAX_DISPLACEMENT_PX)
## for the gate and for why it is an absolute ceiling rather than a ratio
## against the control.
##
## ## COST
##
## Everything works on a SUBSAMPLED luma grid (see `sample_stride`) taken
## straight from the capture's byte buffer -- no per-pixel Image.get_pixel(),
## no PackedFloat32Array per full frame. One grid per captured frame per region.

## Every Nth pixel in each axis. The gradients are then evaluated on the
## subsampled grid and divided by the spacing, so the returned displacement is
## still in SOURCE pixels. 3 keeps a 480x270 region at 160x90 samples, which is
## ~2.8M inner iterations for a 16-frame, two-region, four-config sweep.
const SAMPLE_STRIDE := 3

## Below this much gradient energy the 2x2 system is ill-conditioned and the
## displacement it reports is noise. A region that trips this is reported as
## UNMEASURABLE -- never as zero displacement, which would read as a pass.
const MIN_GRADIENT_ENERGY := 1.0e-3

## Reciprocal condition number floor for the same reason: a region textured in
## only one direction (a vertical grating) can have plenty of energy and still
## pin down only one component.
const MIN_RECIPROCAL_CONDITION := 1.0e-3


## Subsampled luma grid of one Image, as {w, h, data: PackedFloat32Array}.
##
## Reads the RGBA8 byte buffer directly; `Image.get_pixel()` per sample is about
## an order of magnitude slower and this runs on every captured frame.
static func luma_grid(img: Image) -> Dictionary:
	if img == null:
		return {}
	var src := img
	if src.get_format() != Image.FORMAT_RGBA8:
		src = img.duplicate()
		src.convert(Image.FORMAT_RGBA8)
	var w := src.get_width()
	var h := src.get_height()
	if w <= 0 or h <= 0:
		return {}
	var bytes := src.get_data()
	var sw := int(ceil(float(w) / float(SAMPLE_STRIDE)))
	var sh := int(ceil(float(h) / float(SAMPLE_STRIDE)))
	var out := PackedFloat32Array()
	out.resize(sw * sh)
	var o := 0
	for sy in range(sh):
		var row := (sy * SAMPLE_STRIDE) * w * 4
		for sx in range(sw):
			var i := row + (sx * SAMPLE_STRIDE) * 4
			out[o] = 0.299 * float(bytes[i]) + 0.587 * float(bytes[i + 1]) + 0.114 * float(bytes[i + 2])
			o += 1
	return {"w": sw, "h": sh, "data": out}


## Element-wise mean of a list of grids of identical shape.
##
## Returns {} for anything it cannot average, INCLUDING an empty grid in the
## list. `luma_grid()` returns {} for a null or zero-sized capture, and a frame
## that failed to read back is a real possibility; without the emptiness checks
## below this function indexed `grid["w"]` on that {} and killed the scene with
## "Invalid access to key 'w'" instead of letting the caller report the
## UNMEASURABLE refusal this file promises. A crash and a refusal are not the
## same outcome and must not be reachable from the same input.
static func mean_grid(grids: Array) -> Dictionary:
	if grids.is_empty():
		return {}
	var first: Dictionary = grids[0]
	if first.is_empty() or not first.has("w") or not first.has("h") or not first.has("data"):
		return {}
	var w: int = first["w"]
	var h: int = first["h"]
	if w <= 0 or h <= 0:
		return {}
	var acc := PackedFloat32Array()
	acc.resize(w * h)
	for g in grids:
		if not (g is Dictionary) or not g.has("w") or not g.has("h") or not g.has("data"):
			return {}
		if int(g["w"]) != w or int(g["h"]) != h:
			return {}
		var d: PackedFloat32Array = g["data"]
		if d.size() != w * h:
			return {}
		for i in range(w * h):
			acc[i] += d[i]
	var inv := 1.0 / float(grids.size())
	for i in range(w * h):
		acc[i] *= inv
	return {"w": w, "h": h, "data": acc}


## Central-difference gradients of a grid, in units of SOURCE pixels.
## Returns {gx, gy, sxx, sxy, syy} over the interior samples.
static func gradients(grid: Dictionary) -> Dictionary:
	var w: int = grid["w"]
	var h: int = grid["h"]
	var d: PackedFloat32Array = grid["data"]
	var gx := PackedFloat32Array()
	var gy := PackedFloat32Array()
	gx.resize(w * h)
	gy.resize(w * h)
	var inv_spacing := 1.0 / (2.0 * float(SAMPLE_STRIDE))
	var sxx := 0.0
	var sxy := 0.0
	var syy := 0.0
	for y in range(1, h - 1):
		var row := y * w
		for x in range(1, w - 1):
			var i := row + x
			var vx: float = (d[i + 1] - d[i - 1]) * inv_spacing
			var vy: float = (d[i + w] - d[i - w]) * inv_spacing
			gx[i] = vx
			gy[i] = vy
			sxx += vx * vx
			sxy += vx * vy
			syy += vy * vy
	var n := float(max(1, (w - 2) * (h - 2)))
	return {"gx": gx, "gy": gy, "sxx": sxx, "sxy": sxy, "syy": syy, "samples": n}


## Displacement of `grid` relative to the reference the gradients came from.
##
## Returns {measurable: bool, reason: String, dx: float, dy: float, mag: float}.
## `measurable == false` is a REFUSAL, not a zero: a caller must fail rather than
## record 0.0 px, or an untextured region would certify perfect stability.
static func displacement(ref: Dictionary, grad: Dictionary, grid: Dictionary) -> Dictionary:
	# Same rule as mean_grid(): a frame whose capture failed arrives here as {},
	# and it has to become a refusal, never an exception.
	if not (grid is Dictionary) or not grid.has("w") or not grid.has("h") or not grid.has("data"):
		return {"measurable": false, "reason": "a captured frame produced no grid (readback failure?)",
				"dx": 0.0, "dy": 0.0, "mag": 0.0}
	var w: int = ref["w"]
	var h: int = ref["h"]
	var r: PackedFloat32Array = ref["data"]
	var f: PackedFloat32Array = grid["data"]
	if int(grid["w"]) != w or int(grid["h"]) != h:
		return {"measurable": false, "reason": "grid shape mismatch", "dx": 0.0, "dy": 0.0, "mag": 0.0}
	var gx: PackedFloat32Array = grad["gx"]
	var gy: PackedFloat32Array = grad["gy"]
	var sxx: float = grad["sxx"]
	var sxy: float = grad["sxy"]
	var syy: float = grad["syy"]
	var n: float = grad["samples"]

	# Normalised gradient energy, so the floor does not depend on region area.
	var energy := (sxx + syy) / n
	if energy < MIN_GRADIENT_ENERGY:
		return {
			"measurable": false,
			"reason": "gradient energy %.6f < %.6f: region carries no measurable structure" % [energy, MIN_GRADIENT_ENERGY],
			"dx": 0.0, "dy": 0.0, "mag": 0.0,
		}
	var det := sxx * syy - sxy * sxy
	var trace := sxx + syy
	var rcond := 0.0 if trace <= 0.0 else det / (trace * trace)
	if rcond < MIN_RECIPROCAL_CONDITION:
		return {
			"measurable": false,
			"reason": "gradient structure tensor is ill-conditioned (rcond %.6f < %.6f)" % [rcond, MIN_RECIPROCAL_CONDITION],
			"dx": 0.0, "dy": 0.0, "mag": 0.0,
		}

	var bx := 0.0
	var by := 0.0
	for y in range(1, h - 1):
		var row := y * w
		for x in range(1, w - 1):
			var i := row + x
			var dt: float = f[i] - r[i]
			bx += gx[i] * dt
			by += gy[i] * dt
	var dx := (syy * bx - sxy * by) / det
	var dy := (sxx * by - sxy * bx) / det
	return {"measurable": true, "reason": "", "dx": dx, "dy": dy, "mag": sqrt(dx * dx + dy * dy)}


## Mean/max displacement of a whole captured window against its own mean frame.
##
## Returns {measurable, reason, mean_px, max_px, per_frame: PackedFloat32Array}.
static func window_displacement(grids: Array) -> Dictionary:
	if grids.size() < 2:
		return {"measurable": false, "reason": "need at least 2 frames, got %d" % grids.size(),
				"mean_px": 0.0, "max_px": 0.0, "per_frame": PackedFloat32Array()}
	var ref := mean_grid(grids)
	if ref.is_empty():
		return {"measurable": false,
				"reason": "captured frames are missing or have inconsistent shapes -- one or more readbacks produced no image",
				"mean_px": 0.0, "max_px": 0.0, "per_frame": PackedFloat32Array()}
	var grad := gradients(ref)
	var mags := PackedFloat32Array()
	var total := 0.0
	var worst := 0.0
	for g in grids:
		var d := displacement(ref, grad, g)
		if not bool(d["measurable"]):
			return {"measurable": false, "reason": String(d["reason"]),
					"mean_px": 0.0, "max_px": 0.0, "per_frame": PackedFloat32Array()}
		var m: float = d["mag"]
		mags.append(m)
		total += m
		worst = max(worst, m)
	return {
		"measurable": true, "reason": "",
		"mean_px": total / float(mags.size()),
		"max_px": worst,
		"per_frame": mags,
	}


## Mean/max per-channel LSB difference between consecutive frames of a window.
##
## Uses Image.compute_image_metrics(), which is the engine's own C++ histogram
## metric, so a 16-frame window costs nothing measurable in GDScript.
## `use_luma = false` scores every channel, matching the #929 assessment table.
static func consecutive_frame_delta(frames: Array) -> Dictionary:
	if frames.size() < 2:
		return {"valid": false, "mean": 0.0, "max": 0.0, "per_pair": PackedFloat32Array()}
	var per_pair := PackedFloat32Array()
	var total := 0.0
	var worst := 0.0
	for i in range(1, frames.size()):
		var a: Image = frames[i - 1]
		var b: Image = frames[i]
		if a == null or b == null or a.get_size() != b.get_size():
			return {"valid": false, "mean": 0.0, "max": 0.0, "per_pair": PackedFloat32Array()}
		var m: Dictionary = b.compute_image_metrics(a, false)
		var mean_v := float(m.get("mean", INF))
		var max_v := float(m.get("max", INF))
		if is_inf(mean_v) or is_nan(mean_v):
			return {"valid": false, "mean": 0.0, "max": 0.0, "per_pair": PackedFloat32Array()}
		per_pair.append(mean_v)
		total += mean_v
		worst = max(worst, max_v)
	return {"valid": true, "mean": total / float(per_pair.size()), "max": worst, "per_pair": per_pair}


## Autocorrelation of a mean-removed signal at one lag. Diagnostic only -- see
## the header for why this is reported and never gated.
static func autocorrelation_at_lag(series: PackedFloat32Array, lag: int) -> float:
	var n := series.size()
	if lag <= 0 or n - lag < 4:
		return NAN
	var mean := 0.0
	for v in series:
		mean += v
	mean /= float(n)
	var num := 0.0
	var na := 0.0
	var nb := 0.0
	for i in range(n - lag):
		var a := series[i] - mean
		var b := series[i + lag] - mean
		num += a * b
		na += a * a
		nb += b * b
	if na <= 0.0 or nb <= 0.0:
		return NAN
	return num / sqrt(na * nb)
