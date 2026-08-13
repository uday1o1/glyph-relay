CMAKE := uvx --from cmake==4.1.0 cmake
CLANG_FORMAT := uvx --from clang-format==22.1.8 clang-format
HOST_SYSTEM := $(shell uname -s)
PRESET := $(if $(filter Darwin,$(HOST_SYSTEM)),macos-local,linux-cpu)
CXX_FILES := $(shell find include src tests -type f \( -name '*.cpp' -o -name '*.hpp' \) | sort)

.PHONY: all analyze build check clean clean-tree-check configure format format-check lint linux-cpu-check python-check secret-scan test typecheck

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
	corepack pnpm exec prettier --write package.json tsconfig.json 'tooling/**/*.ts' 'tests/typescript/**/*.ts' '.github/**/*.yml'

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(CXX_FILES)
	uv run ruff format --check tools tests/python
	corepack pnpm exec prettier --check package.json tsconfig.json 'tooling/**/*.ts' 'tests/typescript/**/*.ts' '.github/**/*.yml'

analyze:
	bash tools/run_clang_analyzer.sh

typecheck:
	corepack pnpm exec tsc --noEmit

python-check:
	uv run ruff check tools tests/python
	uv run mypy tools tests/python
	uv run pytest -q tests/python

secret-scan:
	uv run python tools/check_repository.py

lint: format-check analyze typecheck python-check secret-scan

linux-cpu-check:
	bash scripts/ci/check_linux_cpu.sh

check: lint test
	corepack pnpm test

clean:
	$(CMAKE) -E remove_directory build

clean-tree-check:
	bash scripts/ci/check_clean_tree.sh
