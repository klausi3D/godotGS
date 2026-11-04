extends "gs_combined_1_harness.gd"

# GS-COMBINED-1 variant A. All logic lives in gs_combined_1_harness.gd; the verdict is
# computed by gs_combined_1_verdict.py from the record this run writes.


func _variant() -> String:
	return "A"
