# syntax=docker/dockerfile:1.7
FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN --mount=type=cache,target=/go/pkg/mod go mod download
COPY . .
RUN --mount=type=cache,target=/root/.cache/go-build CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/community-server ./cmd/community-server

FROM gcr.io/distroless/static-debian12:nonroot
COPY --from=build /out/community-server /community-server
USER nonroot:nonroot
EXPOSE 8080
ENTRYPOINT ["/community-server"]

