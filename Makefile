.PHONY: format format-check install-hooks build run dev \
        up up-build down restart logs ps sync backup shell test test-go test-web

# --- Docker Compose (the normal way to run this project) ---------------------
#
# One container serves both the Go API and the built Vue SPA, which is also what
# the Home Assistant add-on ships. `up` starts it in the background and the
# compose file's restart:unless-stopped keeps it up across reboots.
#
# Use `make run` (below) only if you specifically want a raw container without
# the healthcheck, TZ and stop-grace-period settings that compose adds.

up:
	docker compose up -d

# Rebuild the image first. Use this after pulling code changes.
up-build:
	docker compose up -d --build

down:
	docker compose down

restart:
	docker compose restart

logs:
	docker compose logs -f

ps:
	docker compose ps

# Copy the database out before anything destructive. This is the only stateful
# thing in the deployment, so it is the only thing worth backing up. The -wal and
# -shm files are part of a live SQLite database in WAL mode: copying only the .db
# would capture a torn state.
backup:
	@mkdir -p data/backup-$$(date +%Y%m%d-%H%M%S) && \
	cp -a data/photoframe.db data/photoframe.db-wal data/photoframe.db-shm \
	      data/backup-$$(date +%Y%m%d-%H%M%S)/ 2>/dev/null || true; \
	echo "Backed up to data/backup-$$(date +%Y%m%d-%H%M%S)/"

shell:
	docker compose exec photoframe-server sh

# Tell the frame to fetch a new image right now, without going through the web
# UI. Talks to the frame's own /api/sync directly, so it needs no login and does
# not depend on the server being up.
#
# Override the address if the frame is not at .50:
#     make sync PF_FRAME_HOST=192.168.0.120
PF_FRAME_HOST ?= 192.168.0.50

# The frame handles this synchronously -- it downloads and decodes the image
# before replying -- so the timeout has to be generous. 180s covers the
# firmware's worst case (two 45s fetch attempts).
sync:
	@curl -sS -m 180 "http://$(PF_FRAME_HOST)/api/sync" && echo

# --- Verification ------------------------------------------------------------
#
# Go is not installed on a host that only runs the container, so the toolchain
# comes from the same image the build uses. go-sqlite3 needs CGO, hence the
# gcc/musl-dev install.

test-go:
	docker run --rm -v "$(PWD):/src" -w /src -e CGO_ENABLED=1 \
		golang:1.24-alpine sh -c \
		"apk add --no-cache gcc musl-dev >/dev/null && \
		 echo '== gofmt ==' && test -z \"\$$(gofmt -l backend/)\" && \
		 echo '== go vet ==' && go vet ./... && \
		 echo '== go test ==' && go test ./..."

test-web:
	cd webapp && npm run type-check && npm run format:check && npm run test

test: test-go test-web

# --- Native (no Docker) ------------------------------------------------------
# Both of these need Go and Node on the host; keep them for when you are not
# using the container.

format:
	@echo "Formatting Go code..."
	gofmt -w backend/
	@echo "Formatting Frontend code..."
	cd webapp && npm run format

format-check:
	@echo "Checking Go formatting..."
	@unformatted=$$(gofmt -l backend/); \
	if [ -n "$$unformatted" ]; then \
		echo "These Go files need formatting (run 'make format'):"; \
		echo "$$unformatted"; \
		exit 1; \
	fi
	@echo "Checking Frontend formatting..."
	@cd webapp && [ -d node_modules ] || npm ci
	@cd webapp && npm run format:check
	@echo "All files are properly formatted!"

# Enable the repo's git hooks (pre-commit runs `make format-check`).
install-hooks:
	@git config core.hooksPath .githooks
	@echo "Git hooks enabled (core.hooksPath = .githooks)."
	@echo "Commits now run 'make format-check' first."

build:
	docker build -t photoframe-server .

run:
	docker rm -f photoframe-server || true
	docker run -d -p 9607:9607 -v "$(PWD)/data:/data" --name photoframe-server photoframe-server

dev:
	@if ! command -v epaper-image-convert >/dev/null 2>&1; then \
		echo "Installing epaper-image-convert..."; \
		npm install -g @aitjcize/epaper-image-convert; \
	else \
		echo "epaper-image-convert already installed, skipping..."; \
	fi
	@if [ ! -f bin/fonts/NotoSans-Regular.ttf ]; then \
		echo "Downloading NotoSans font..."; \
		mkdir -p bin/fonts; \
		curl -sL "https://github.com/google/fonts/raw/main/ofl/notosans/NotoSans-Regular.ttf" -o bin/fonts/NotoSans-Regular.ttf; \
	fi
	@if [ ! -f bin/fonts/MaterialSymbolsOutlined.ttf ]; then \
		echo "Downloading Material Symbols font..."; \
		mkdir -p bin/fonts; \
		curl -sL "https://github.com/google/material-design-icons/raw/master/variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf" -o bin/fonts/MaterialSymbolsOutlined.ttf; \
	fi
	@echo "Building frontend..."
	@cd webapp && npm install && npm run build
	@echo "Starting server locally..."
	@cd backend && CGO_ENABLED=1 DATA_DIR=$(PWD)/data DB_PATH=$(PWD)/data/photoframe.db STATIC_DIR=../webapp/dist go run .
