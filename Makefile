CMAKE := uvx --from cmake==4.1.0 cmake
CLANG_FORMAT := uvx --from clang-format==22.1.8 clang-format
HOST_SYSTEM := $(shell uname -s)
PRESET := $(if $(filter Darwin,$(HOST_SYSTEM)),macos-local,linux-cpu)
CXX_FILES := $(shell find include src tests tools -type f \( -name '*.cpp' -o -name '*.hpp' \) | sort)

.PHONY: all analyze browser-harness-check browser-probe build check clean clean-tree-check configure dependency-check format format-check handoff-check lint linux-cpu-check protocol-check python-check secret-scan test transport-check typecheck

all: build

configure:
	$(CMAKE) --preset $(PRESET)

build: configure
	$(CMAKE) --build --preset $(PRESET) --parallel

test: build
	$(CMAKE) --build --preset $(PRESET) --target test
	uv run python tools/validate_doctor.py build/$(PRESET)/glyphrelay schemas/doctor-v1.schema.json

format:
	$(CLANG_FORMAT) -i $(CXX_FILES)
	uv run ruff format tools tests/python
	corepack pnpm exec prettier --write package.json tsconfig.json 'signaling/**/*.ts' 'tooling/**/*.ts' 'tests/typescript/**/*.ts' 'receiver/**/*.{html,css,js}' 'protocols/browser_oracle_v1/*.json' 'qualification/*.json' 'schemas/*.json' '.github/**/*.yml'

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(CXX_FILES)
	uv run ruff format --check tools tests/python
	corepack pnpm exec prettier --check package.json tsconfig.json 'signaling/**/*.ts' 'tooling/**/*.ts' 'tests/typescript/**/*.ts' 'receiver/**/*.{html,css,js}' 'protocols/browser_oracle_v1/*.json' 'qualification/*.json' 'schemas/*.json' '.github/**/*.yml'

analyze:
	bash tools/run_clang_analyzer.sh

typecheck:
	corepack pnpm run typecheck

browser-harness-check:
	corepack pnpm run browser:harness

browser-probe:
	corepack pnpm run browser:probe

handoff-check:
	uv run python -m tools.gpu.disposable_remote --repository .

python-check:
	uv run ruff check tools tests/python
	uv run mypy tools tests/python
	uv run pytest -q tests/python

dependency-check:
	uv run python tools/check_dependency_lock.py

protocol-check:
	uv run python tools/check_m0_protocol.py

transport-check:
	bash scripts/verify_libdatachannel_patch.sh

secret-scan:
	uv run python tools/check_repository.py

lint: format-check analyze typecheck python-check dependency-check protocol-check secret-scan

linux-cpu-check:
	bash scripts/ci/check_linux_cpu.sh

check: lint test
	corepack pnpm test

clean:
	$(CMAKE) -E remove_directory build

clean-tree-check:
	bash scripts/ci/check_clean_tree.sh
