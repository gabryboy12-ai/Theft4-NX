package roles

import "encoding/json"

func jsonBody(raw []byte, target any) error { return json.Unmarshal(raw, target) }
func marshal(value any) json.RawMessage     { raw, _ := json.Marshal(value); return raw }
