// Package buildinfo carries the version identifiers stamped into the binary at
// link time.
//
// The Makefile sets these with -ldflags -X. When the binary is built without
// them (a plain `go build` or `go test`), values are recovered from the module
// build info where possible so the gateway still reports something honest.
package buildinfo

import (
	"runtime"
	"runtime/debug"
	"strings"
	"sync"
)

// Overridden at link time:
//
//	-X github.com/srschreiber/remote-firepump/pump-gateway/internal/buildinfo.version=0.1.0
var (
	version   = ""
	commit    = ""
	buildDate = ""
)

type Info struct {
	Version   string `json:"version"`
	Commit    string `json:"commit"`
	BuildDate string `json:"build_date"`
	GoVersion string `json:"go_version"`
	Platform  string `json:"platform"`
}

var get = sync.OnceValue(func() Info {
	i := Info{
		Version:   version,
		Commit:    commit,
		BuildDate: buildDate,
		GoVersion: runtime.Version(),
		Platform:  runtime.GOOS + "/" + runtime.GOARCH,
	}
	if bi, ok := debug.ReadBuildInfo(); ok {
		for _, s := range bi.Settings {
			switch s.Key {
			case "vcs.revision":
				if i.Commit == "" {
					i.Commit = shortCommit(s.Value)
				}
			case "vcs.time":
				if i.BuildDate == "" {
					i.BuildDate = s.Value
				}
			case "vcs.modified":
				if s.Value == "true" && i.Commit != "" && !strings.HasSuffix(i.Commit, "-dirty") {
					i.Commit += "-dirty"
				}
			}
		}
	}
	if i.Version == "" {
		i.Version = "0.0.0-dev"
	}
	if i.Commit == "" {
		i.Commit = "unknown"
	}
	if i.BuildDate == "" {
		i.BuildDate = "unknown"
	}
	return i
})

func shortCommit(rev string) string {
	if len(rev) > 12 {
		return rev[:12]
	}
	return rev
}

// Get returns the stamped build identifiers.
func Get() Info { return get() }

// Version is the short form used in the User-Agent and the UI footer.
func Version() string { return Get().Version }

// String is a one-line summary for startup logs.
func (i Info) String() string {
	return "pump-gateway " + i.Version + " (" + i.Commit + ", built " + i.BuildDate + ", " + i.GoVersion + " " + i.Platform + ")"
}
