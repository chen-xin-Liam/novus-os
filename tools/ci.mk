# ---------------------------------------------------------------------------
# Automated test pipeline (included from the top-level Makefile)
# ---------------------------------------------------------------------------
#   make check            fast hard gate: syntax, generated drift, hygiene,
#                         cargo check for both freestanding Rust crates
#   make check-lint       clippy/rustfmt baseline gate (fails on new issues)
#   make check-lint-update
#                         deliberately refresh tools/ci/lint-baseline.json
#   make smoke            build the live ISO, then a 300 s bounded QEMU boot
#   make pipeline / ci    run all stages in order and write out/ci/report.txt
#
# The QEMU smoke test stops as soon as the desktop is up, but never runs for
# more than SMOKE_TIMEOUT seconds (hard-capped at 300 in ci_smoke.py).

PYTHON ?= python3
CI_DIR := out/ci
SMOKE_TIMEOUT ?= 300
SMOKE_SETTLE ?= 15
SMOKE_IMAGE ?= out/novus-live.iso

.PHONY: check check-lint check-lint-update smoke pipeline ci

$(CI_DIR):
	mkdir -p $@

check: | $(CI_DIR)
	$(PYTHON) scripts/ci_check.py

check-lint: | $(CI_DIR)
	$(PYTHON) scripts/ci_lint.py

check-lint-update: | $(CI_DIR)
	$(PYTHON) scripts/ci_lint.py --update-baseline

smoke: $(SMOKE_IMAGE) | $(CI_DIR)
	$(PYTHON) scripts/ci_smoke.py --image $(SMOKE_IMAGE) --timeout $(SMOKE_TIMEOUT) --settle $(SMOKE_SETTLE) --output $(CI_DIR)/smoke

pipeline ci:
	$(PYTHON) scripts/ci_pipeline.py
