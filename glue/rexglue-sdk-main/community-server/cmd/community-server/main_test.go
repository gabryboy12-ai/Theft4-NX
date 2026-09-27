package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCheckHealth(t *testing.T) {
	for _, status := range []int{http.StatusOK, http.StatusServiceUnavailable} {
		t.Run(http.StatusText(status), func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(status)
			}))
			defer server.Close()

			err := checkHealth(server.URL)
			if status == http.StatusOK && err != nil {
				t.Fatalf("checkHealth() error = %v", err)
			}
			if status != http.StatusOK && err == nil {
				t.Fatal("checkHealth() accepted an unhealthy response")
			}
		})
	}
}
