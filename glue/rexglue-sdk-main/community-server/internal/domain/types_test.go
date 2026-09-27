package domain

import "testing"

func TestSessionPeerBoundaries(t *testing.T) {
	session := SessionRecord{Mode: "free_mode", Episode: "base", Region: "na", PublicSlots: 64, BuildID: "build", ProtocolVersion: 2, TitleID: "0x545407f2", MediaID: "0x12345678", TitleVersion: "0x00000008", SessionID: "0x0000000000000001", ExchangeKey: "0x00000000000000000000000000000001", Contexts: map[string]uint32{}, Properties: map[string]string{}}
	for peer := 0; peer < 64; peer++ {
		session.Members = append(session.Members, SessionMember{XUID: NewID("xuid"), AccountID: NewID("acct"), Route: Route{PeerID: peer}})
	}
	if err := session.Validate(64); err != nil {
		t.Fatalf("64-member session rejected: %v", err)
	}
	session.Members[63].Route.PeerID = 64
	if err := session.Validate(64); err == nil {
		t.Fatal("peer ID 64 was accepted")
	}
	session.Members[63].Route.PeerID = 16
	if err := session.Validate(64); err == nil {
		t.Fatal("duplicate sparse peer ID was accepted")
	}
}
