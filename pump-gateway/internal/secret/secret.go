// Package secret holds the shared Arduino API secret in a type that resists
// accidental disclosure.
//
// The zero value is an empty secret. String, GoString, Format, LogValue and
// MarshalJSON all render a fixed placeholder, so a Secret cannot leak through
// fmt, log/slog or encoding/json even when a caller passes a whole struct to a
// logger. The real bytes are only reachable through Reveal, which is easy to
// grep for during review.
package secret

import (
	"crypto/subtle"
	"fmt"
	"log/slog"
	"strings"
)

// Placeholder is what a Secret renders as in any human- or machine-readable
// output.
const Placeholder = "[REDACTED]"

// Value is a shared secret. Copying it is cheap and safe.
type Value string

// New trims surrounding whitespace, which is what makes a secret file written
// by `printf ... > file` or an editor that appends a newline behave the same.
func New(raw string) Value {
	return Value(strings.TrimSpace(raw))
}

// Reveal returns the secret in the clear. Every call site is security relevant.
func (v Value) Reveal() string { return string(v) }

// Empty reports whether no secret is configured.
func (v Value) Empty() bool { return len(v) == 0 }

// Len is the length in bytes. Safe to log: it is not the secret.
func (v Value) Len() int { return len(v) }

// Equal is a constant-time comparison, used by the mock Arduino server.
func (v Value) Equal(other string) bool {
	return subtle.ConstantTimeCompare([]byte(v), []byte(other)) == 1
}

func (v Value) String() string   { return Placeholder }
func (v Value) GoString() string { return Placeholder }

// Format implements fmt.Formatter so that even %q, %v, %#v and %s render the
// placeholder rather than the secret.
func (v Value) Format(f fmt.State, verb rune) {
	switch verb {
	case 'q':
		fmt.Fprintf(f, "%q", Placeholder)
	default:
		fmt.Fprint(f, Placeholder)
	}
}

// LogValue implements slog.LogValuer.
func (v Value) LogValue() slog.Value { return slog.StringValue(Placeholder) }

// MarshalJSON makes it impossible to serialise a Secret into an API response.
func (v Value) MarshalJSON() ([]byte, error) {
	return []byte(`"` + Placeholder + `"`), nil
}
