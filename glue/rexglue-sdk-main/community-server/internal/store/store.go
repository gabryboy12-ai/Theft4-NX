package store

import (
	"context"
	"time"

	"libertyrecomp/community-server/internal/domain"
)

type IdempotencyResult struct {
	Status      int
	Body        []byte
	RequestHash string
}

type IdempotencyState uint8

const (
	IdempotencyReserved IdempotencyState = iota
	IdempotencyReplay
	IdempotencyInProgress
	IdempotencyConflict
)

type SessionQuery struct {
	TitleID, MediaID, TitleVersion, Requestor string
	ProtocolVersion                           int
	Contexts                                  map[string]uint32
	Properties                                map[string]string
	Limit                                     int
}

type Store interface {
	Ready(context.Context) error
	Maintain(context.Context, time.Time) error
	Close()
	CreateDocument(context.Context, domain.Document) (domain.Document, error)
	GetDocument(context.Context, string, string) (domain.Document, error)
	UpdateDocument(context.Context, domain.Document, int64) (domain.Document, error)
	PutDocumentsAtomic(context.Context, []domain.Document) ([]domain.Document, error)
	DeleteDocument(context.Context, string, string, int64) error
	DeleteSession(context.Context, string, int64, []domain.Event) error
	MigrateSession(context.Context, domain.Document, domain.Document, int64) (domain.Document, domain.Document, error)
	ListDocuments(context.Context, string, string, int) ([]domain.Document, error)
	SearchSessions(context.Context, SessionQuery) ([]domain.Document, error)
	PutDevice(context.Context, domain.Device) error
	GetDevice(context.Context, string) (domain.Device, error)
	GetDeviceByXUID(context.Context, string) (domain.Device, error)
	PutChallenge(context.Context, domain.Challenge) error
	ConsumeChallenge(context.Context, string, string, time.Time) (domain.Challenge, error)
	PutRefresh(context.Context, domain.RefreshSession) error
	ConsumeRefresh(context.Context, string, time.Time) (domain.RefreshSession, error)
	ReserveIdempotency(context.Context, string, string, string, string, string, time.Time) (IdempotencyResult, IdempotencyState, error)
	FinalizeIdempotency(context.Context, string, string, string, string, IdempotencyResult, time.Time) error
	AppendEvent(context.Context, domain.Event) (domain.Event, error)
	AppendEvents(context.Context, []domain.Event) error
	ReplayEvents(context.Context, string, int64, int) ([]domain.Event, error)
	AppendAudit(context.Context, string, string, string, string, []byte) error
	LeaseOutbox(context.Context, int) ([]domain.Event, error)
	MarkOutboxDelivered(context.Context, []int64) error
}
