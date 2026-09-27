package handler

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

// Saving a device must never be able to blank its image source.
//
// The failure this guards against is quiet and expensive: the server answers
// /image with 400 once a device has no source, so the frame goes dark and keeps
// retrying, and nothing in the UI says why. Any client that simply omits the
// field would cause it.
func TestResolveDeviceSource(t *testing.T) {
	cases := []struct {
		name      string
		requested string
		existing  string
		want      string
	}{
		{
			name:      "an explicit choice always wins",
			requested: "gallery",
			existing:  "fractal",
			want:      "gallery",
		},
		{
			// The regression: a client that leaves the field blank must not
			// take a working frame offline.
			name:      "an empty value preserves what is configured",
			requested: "",
			existing:  "fractal",
			want:      "fractal",
		},
		{
			name:      "switching sources is honoured",
			requested: "url_proxy",
			existing:  "gallery",
			want:      "url_proxy",
		},
		{
			// A device that never had a source stays that way, rather than
			// being handed a default nobody asked for.
			name:      "both empty stays empty",
			requested: "",
			existing:  "",
			want:      "",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			assert.Equal(t, tc.want, resolveDeviceSource(tc.requested, tc.existing))
		})
	}
}

// The guard must not be a blanket "ignore the request": a real change has to go
// through, in both directions, and survive repeated saves.
func TestResolveDeviceSourceIsIdempotent(t *testing.T) {
	// First save sets it, second save with the same value is a no-op rather
	// than a revert, and an unrelated save never clears it.
	got := resolveDeviceSource("gallery", "")
	assert.Equal(t, "gallery", got)

	got = resolveDeviceSource("gallery", got)
	assert.Equal(t, "gallery", got)

	got = resolveDeviceSource("", got)
	assert.Equal(t, "gallery", got)
}
