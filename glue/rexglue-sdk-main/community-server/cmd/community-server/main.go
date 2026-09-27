package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"libertyrecomp/community-server/internal/auth"
	"libertyrecomp/community-server/internal/config"
	"libertyrecomp/community-server/internal/httpapi"
	"libertyrecomp/community-server/internal/migrate"
	"libertyrecomp/community-server/internal/realtime"
	"libertyrecomp/community-server/internal/relay"
	"libertyrecomp/community-server/internal/roles"
	"libertyrecomp/community-server/internal/store"
)

func main() {
	if err := run(); err != nil {
		slog.Error("server stopped", "error", err)
		os.Exit(1)
	}
}
func run() error {
	if len(os.Args) > 1 && os.Args[1] == "healthcheck" {
		return checkHealth("http://127.0.0.1:8080/health/ready")
	}
	cfg, err := config.Load()
	if err != nil {
		return err
	}
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()
	if len(os.Args) > 1 && os.Args[1] == "migrate" {
		if cfg.DatabaseURL == "" {
			return errors.New("database URL required for migrations")
		}
		return migrate.Run(ctx, cfg.DatabaseURL)
	}
	var repository store.Store
	if cfg.DatabaseURL != "" {
		repository, err = store.NewPostgres(ctx, cfg.DatabaseURL)
	} else {
		repository = store.NewMemory()
	}
	if err != nil {
		return err
	}
	defer repository.Close()
	authService := auth.New(repository, cfg.TokenSigningKey, cfg.AccessTTL, cfg.RefreshTTL, cfg.ChallengeTTL)
	hub, err := realtime.New(repository, cfg.RedisURL)
	if err != nil {
		return err
	}
	defer hub.Close()
	relayService, err := relay.New(cfg.RedisURL)
	if err != nil {
		return err
	}
	defer relayService.Close()
	api, err := httpapi.New(cfg, repository, authService, hub, relayService)
	if err != nil {
		return err
	}
	defer api.Close()
	server := &http.Server{Addr: cfg.ListenAddress, Handler: api.Handler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 35 * time.Second, WriteTimeout: 35 * time.Second, IdleTimeout: 90 * time.Second, MaxHeaderBytes: 32768}
	go func() {
		<-ctx.Done()
		shutdown, done := context.WithTimeout(context.Background(), 10*time.Second)
		defer done()
		_ = server.Shutdown(shutdown)
	}()
	go func() {
		if err := hub.RunSubscriber(ctx); err != nil && !errors.Is(err, context.Canceled) {
			slog.Error("event subscriber stopped", "error", err)
		}
	}()
	switch cfg.Role {
	case "worker":
		go func() { _ = roles.RunWorker(ctx, repository, hub) }()
	case "matchmaker":
		go func() { _ = roles.RunMatchmaker(ctx, repository, hub) }()
	}
	slog.Info("community server starting", "role", cfg.Role, "listen", cfg.ListenAddress)
	err = server.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func checkHealth(endpoint string) error {
	client := &http.Client{Timeout: 3 * time.Second}
	response, err := client.Get(endpoint)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("health endpoint returned %s", response.Status)
	}
	return nil
}
