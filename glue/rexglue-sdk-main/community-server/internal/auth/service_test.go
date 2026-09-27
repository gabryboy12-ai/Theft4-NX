package auth

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"testing"
	"time"

	"libertyrecomp/community-server/internal/store"
)

func TestChallengeEnrollmentRefreshAndReplayProtection(t *testing.T) {
	repository := store.NewMemory()
	service := New(repository, []byte("0123456789abcdef0123456789abcdef"), time.Minute, time.Hour, time.Minute)
	seed := []byte("libertyrecomp-test-device-seed!!")
	if len(seed) != ed25519.SeedSize {
		t.Fatalf("fixture seed has %d bytes", len(seed))
	}
	privateKey := ed25519.NewKeyFromSeed(seed)
	publicKey := base64.RawURLEncoding.EncodeToString(privateKey.Public().(ed25519.PublicKey))
	challenge, err := service.Challenge(context.Background(), "device_fixture", "0xe000000000000001", publicKey)
	if err != nil {
		t.Fatal(err)
	}
	signature := base64.RawURLEncoding.EncodeToString(ed25519.Sign(privateKey, []byte(challenge.Nonce)))
	pair, err := service.Enroll(context.Background(), EnrollRequest{DeviceID: "device_fixture", XUID: "0xe000000000000001", MachineID: "machine_fixture", PlayerName: "Niko", PublicKey: publicKey, ChallengeID: challenge.ID, Signature: signature})
	if err != nil {
		t.Fatal(err)
	}
	claims, err := service.Verify(pair.AccessToken)
	if err != nil || claims.Subject == "" {
		t.Fatalf("verify: %v", err)
	}
	if _, err := service.Enroll(context.Background(), EnrollRequest{DeviceID: "device_fixture", XUID: "0xe000000000000001", MachineID: "machine_fixture", PlayerName: "Niko", PublicKey: publicKey, ChallengeID: challenge.ID, Signature: signature}); err == nil {
		t.Fatal("challenge replay was accepted")
	}
	rotated, err := service.Refresh(context.Background(), pair.RefreshToken)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := service.Verify(rotated.AccessToken); err != nil {
		t.Fatal(err)
	}
	if _, err := service.Refresh(context.Background(), pair.RefreshToken); err == nil {
		t.Fatal("refresh replay was accepted")
	}
}
