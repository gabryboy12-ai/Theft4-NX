package relay

import (
	"context"
	"fmt"
	"testing"
	"time"

	"libertyrecomp/community-server/internal/domain"
)

func TestVirtualIPv4AllocationStableAndUnique(t *testing.T) {
	service, err := New("")
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	seen := map[string]bool{}
	for index := 0; index < 256; index++ {
		account := fmt.Sprintf("account_%d", index)
		first, err := service.EnsureVirtualIPv4(context.Background(), account)
		if err != nil {
			t.Fatal(err)
		}
		second, err := service.EnsureVirtualIPv4(context.Background(), account)
		if err != nil || first != second {
			t.Fatalf("unstable allocation %q %q %v", first, second, err)
		}
		if seen[first] {
			t.Fatalf("duplicate allocation %s", first)
		}
		seen[first] = true
	}
}

func TestSendBatchPreservesOrderAndRejectsAtomically(t *testing.T) {
	service, err := New("")
	if err != nil {
		t.Fatal(err)
	}
	defer service.Close()
	ctx := context.Background()
	source, err := service.Register(ctx, "source", "session_one", 3074, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	destination, err := service.Register(ctx, "destination", "session_one", 3075, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	foreign, err := service.Register(ctx, "foreign", "session_two", 3076, time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	batch := []Datagram{
		{DestinationIPv4: destination.VirtualIPv4, DestinationPort: destination.LocalPort, SourcePort: source.LocalPort, Payload: "Zmlyc3Q="},
		{DestinationIPv4: destination.VirtualIPv4, DestinationPort: destination.LocalPort, SourcePort: source.LocalPort, Payload: "c2Vjb25k"},
	}
	if err := service.SendBatch(ctx, map[int]Route{source.LocalPort: source}, batch); err != nil {
		t.Fatal(err)
	}
	received, err := service.Receive(ctx, destination.AccountID, destination.LocalPort, 1024, 0)
	if err != nil || len(received) != 2 || received[0].Payload != batch[0].Payload || received[1].Payload != batch[1].Payload {
		t.Fatalf("ordered receive=%+v err=%v", received, err)
	}
	rejected := append(append([]Datagram{}, batch...), Datagram{DestinationIPv4: foreign.VirtualIPv4, DestinationPort: foreign.LocalPort, SourcePort: source.LocalPort, Payload: "YmFk"})
	if err := service.SendBatch(ctx, map[int]Route{source.LocalPort: source}, rejected); err != domain.ErrForbidden {
		t.Fatalf("expected forbidden, got %v", err)
	}
	received, err = service.Receive(ctx, destination.AccountID, destination.LocalPort, 1024, 0)
	if err != nil || len(received) != 0 {
		t.Fatalf("rejected batch partially queued: %+v err=%v", received, err)
	}
}
