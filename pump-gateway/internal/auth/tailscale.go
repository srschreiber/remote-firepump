// Package auth turns the identity headers injected by `tailscale serve` into an
// authenticated, authorised operator identity.
//
// # Trust model
//
// Tailscale Serve terminates TLS on the tailnet, authenticates the caller
// against the tailnet identity provider, and then proxies the request to a
// loopback backend, adding:
//
//	Tailscale-User-Login:       sam@example.com
//	Tailscale-User-Name:        Sam Schreiber
//	Tailscale-User-Profile-Pic: https://...
//
// Those headers are only trustworthy because the backend is reachable
// exclusively through Serve. Any client that can open a TCP connection to the
// backend directly can set the headers itself. That is why the gateway binds to
// 127.0.0.1, why config.Load refuses to combine a non-loopback listener with
// TRUST_TAILSCALE_HEADERS=true, and why nothing else -- no cookie, no query
// parameter, no bearer token -- may stand in for the login header.
//
// Tailscale-User-Profile-Pic is deliberately ignored: rendering it would mean
// loading a third-party image, which the Content-Security-Policy forbids.
package auth

import (
	"errors"
	"fmt"
	"net/http"
	"slices"
	"strings"
	"unicode"
	"unicode/utf8"
)

// Identity header names as set by Tailscale Serve.
const (
	HeaderLogin      = "Tailscale-User-Login"
	HeaderName       = "Tailscale-User-Name"
	HeaderProfilePic = "Tailscale-User-Profile-Pic"
)

// Sentinel errors. The HTTP layer maps ErrNoIdentity to 401 and ErrForbidden to
// 403; the distinction is "we do not know who you are" versus "we know, and you
// are not on the list".
var (
	ErrNoIdentity = errors.New("no tailscale identity")
	ErrForbidden  = errors.New("operator not authorised")
)

const (
	maxLoginLen       = 254
	maxDisplayNameLen = 64
)

// Identity is an authenticated operator.
type Identity struct {
	Login       string `json:"login"`
	DisplayName string `json:"display_name,omitempty"`
	// DevMode is true when this identity came from DEV_USER rather than from a
	// Tailscale header. The UI must label it visibly.
	DevMode bool `json:"dev_mode"`
}

// Config is the subset of gateway configuration the authenticator needs. It is
// passed explicitly rather than as a *config.Config so that this package has no
// dependency on configuration loading.
type Config struct {
	// RequireIdentity is production mode: a valid, allow-listed
	// Tailscale-User-Login header is mandatory on every browser request.
	RequireIdentity bool
	// TrustHeaders permits reading the Tailscale-User-* headers at all.
	TrustHeaders bool
	// AllowedUsers is the set of logins permitted to view and operate the pump.
	AllowedUsers []string
	// DevUser is the identity used when RequireIdentity is false.
	DevUser string
}

// Authenticator resolves request identity. It is immutable after construction
// and safe for concurrent use.
type Authenticator struct {
	requireIdentity bool
	trustHeaders    bool
	allowed         map[string]struct{}
	allowedList     []string
	devUser         string
}

// New validates the authentication configuration and returns an Authenticator.
func New(c Config) (*Authenticator, error) {
	a := &Authenticator{
		requireIdentity: c.RequireIdentity,
		trustHeaders:    c.TrustHeaders,
		allowed:         make(map[string]struct{}, len(c.AllowedUsers)),
	}

	for _, raw := range c.AllowedUsers {
		login := NormalizeLogin(raw)
		if login == "" {
			continue
		}
		if err := validateLogin(login); err != nil {
			return nil, fmt.Errorf("allowed user %q: %w", raw, err)
		}
		if _, dup := a.allowed[login]; dup {
			continue
		}
		a.allowed[login] = struct{}{}
		a.allowedList = append(a.allowedList, login)
	}
	slices.Sort(a.allowedList)

	if c.RequireIdentity {
		if !c.TrustHeaders {
			return nil, errors.New("REQUIRE_TAILSCALE_IDENTITY=true needs TRUST_TAILSCALE_HEADERS=true: there would be no way to identify anyone")
		}
		if len(a.allowed) == 0 {
			return nil, errors.New("REQUIRE_TAILSCALE_IDENTITY=true needs a non-empty ALLOWED_TAILSCALE_USERS")
		}
	} else {
		a.devUser = NormalizeLogin(c.DevUser)
		if a.devUser == "" {
			return nil, errors.New("REQUIRE_TAILSCALE_IDENTITY=false (development mode) needs DEV_USER")
		}
		if err := validateLogin(a.devUser); err != nil {
			return nil, fmt.Errorf("DEV_USER %q: %w", c.DevUser, err)
		}
	}
	return a, nil
}

// DevMode reports whether identities come from DEV_USER.
func (a *Authenticator) DevMode() bool { return !a.requireIdentity }

// AllowedUsers returns the normalised allow-list. Used for the startup log.
func (a *Authenticator) AllowedUsers() []string { return slices.Clone(a.allowedList) }

// Identify authenticates and authorises a request.
//
// It returns ErrNoIdentity when no usable identity is present (HTTP 401) and
// ErrForbidden when the identity is valid but not allow-listed (HTTP 403).
func (a *Authenticator) Identify(r *http.Request) (Identity, error) {
	if !a.requireIdentity {
		// Development mode. Request headers are ignored entirely: a developer
		// build must not be a lax version of the production auth path, it must
		// be a different path that cannot be reached in production.
		if a.devUser == "" {
			return Identity{}, fmt.Errorf("development mode without DEV_USER: %w", ErrNoIdentity)
		}
		return Identity{
			Login:       a.devUser,
			DisplayName: a.devUser,
			DevMode:     true,
		}, nil
	}

	if !a.trustHeaders {
		// Unreachable through config.Load, kept as a fail-closed backstop.
		return Identity{}, fmt.Errorf("identity headers are not trusted: %w", ErrNoIdentity)
	}

	values := r.Header.Values(HeaderLogin)
	if len(values) == 0 {
		return Identity{}, fmt.Errorf("missing %s: %w", HeaderLogin, ErrNoIdentity)
	}
	if len(values) > 1 {
		// Two logins means something between the browser and here is adding
		// headers. Refuse rather than pick one.
		return Identity{}, fmt.Errorf("multiple %s headers: %w", HeaderLogin, ErrNoIdentity)
	}

	login := NormalizeLogin(values[0])
	if login == "" {
		return Identity{}, fmt.Errorf("empty %s: %w", HeaderLogin, ErrNoIdentity)
	}
	if err := validateLogin(login); err != nil {
		return Identity{}, fmt.Errorf("malformed %s: %w: %w", HeaderLogin, err, ErrNoIdentity)
	}
	if _, ok := a.allowed[login]; !ok {
		return Identity{Login: login}, fmt.Errorf("%q is not in ALLOWED_TAILSCALE_USERS: %w", login, ErrForbidden)
	}

	return Identity{
		Login:       login,
		DisplayName: sanitizeDisplayName(r.Header.Get(HeaderName)),
	}, nil
}

// NormalizeLogin makes header and allow-list comparison safe: surrounding
// whitespace is dropped, control characters are removed, and the login is
// lower-cased. Tailscale logins are already lower-case in practice; normalising
// here means a stray capital in the environment file does not silently lock
// someone out.
func NormalizeLogin(raw string) string {
	s := strings.TrimSpace(raw)
	// A header value cannot legitimately contain a NUL or a newline; strip any
	// control characters before comparing so smuggled bytes cannot match.
	s = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, s)
	s = strings.TrimSpace(s)
	return strings.ToLower(s)
}

// validateLogin enforces the shape of a tailnet login: printable ASCII, exactly
// one "@", no spaces. Tailnet logins look like sam@example.com or sam@github.
func validateLogin(login string) error {
	if login == "" {
		return errors.New("empty")
	}
	if len(login) > maxLoginLen {
		return fmt.Errorf("longer than %d bytes", maxLoginLen)
	}
	if strings.Count(login, "@") != 1 {
		return errors.New(`must contain exactly one "@"`)
	}
	if strings.HasPrefix(login, "@") || strings.HasSuffix(login, "@") {
		return errors.New(`must have text on both sides of the "@"`)
	}
	for _, r := range login {
		if r <= ' ' || r > unicode.MaxASCII {
			return errors.New("must be printable ASCII without spaces")
		}
	}
	return nil
}

// sanitizeDisplayName prepares a human name for echoing back to the browser:
// valid UTF-8, no control characters, bounded length.
func sanitizeDisplayName(raw string) string {
	s := strings.ToValidUTF8(strings.TrimSpace(raw), "")
	s = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return ' '
		}
		return r
	}, s)
	s = strings.Join(strings.Fields(s), " ")
	if utf8.RuneCountInString(s) > maxDisplayNameLen {
		runes := []rune(s)
		s = strings.TrimSpace(string(runes[:maxDisplayNameLen]))
	}
	return s
}
