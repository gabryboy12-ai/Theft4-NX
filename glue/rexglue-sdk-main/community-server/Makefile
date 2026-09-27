.PHONY: test build compose-up compose-down
test:
	python3 tools/audit_limits.py
	go test ./...
build:
	go build ./cmd/community-server
compose-up:
	docker compose up --build -d
compose-down:
	docker compose down
