package handler

import (
	"image"
	"image/color"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// solid builds a w×h image filled with c.
func solid(w, h int, c color.RGBA) *image.RGBA {
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.SetRGBA(x, y, c)
		}
	}
	return img
}

func TestFitToPanelDimensions(t *testing.T) {
	cases := []struct {
		name       string
		srcW       int
		srcH       int
		w, h       int
		mode       string
		bg         string
		wantCover  bool // true: corners are photo. false: corners are the fill.
		wantCorner color.RGBA
	}{
		{
			// Wide source into a square panel, cover: fills, no bars.
			name: "cover fills the panel", srcW: 200, srcH: 100,
			w: 100, h: 100, mode: "cover", bg: "", wantCover: true,
		},
		{
			// Tall source into a wide panel, fit: letterboxed top and
			// bottom, and the bars are the requested background.
			name: "fit letterboxes with the background", srcW: 100, srcH: 200,
			w: 200, h: 100, mode: "fit", bg: "#0000FF",
			wantCorner: color.RGBA{0, 0, 0xff, 0xff},
		},
		{
			// Unset background falls back to white, matching the CLI default,
			// rather than to something new.
			name: "fit with no background defaults to white", srcW: 100, srcH: 200,
			w: 200, h: 100, mode: "fit", bg: "",
			wantCorner: color.RGBA{0xff, 0xff, 0xff, 0xff},
		},
		{
			// An unknown scale mode degrades to fit rather than panicking.
			name: "unknown mode behaves as fit", srcW: 100, srcH: 200,
			w: 200, h: 100, mode: "nonsense", bg: "#000000",
			wantCorner: color.RGBA{0, 0, 0, 0xff},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			src := solid(tc.srcW, tc.srcH, color.RGBA{0xff, 0, 0, 0xff})
			out := fitToPanel(src, tc.w, tc.h, tc.mode, tc.bg)

			// Output is always exactly the panel size, which is what the
			// firmware's X-Display-* geometry promised.
			assert.Equal(t, image.Pt(tc.w, tc.h), out.Bounds().Size(),
				"output must be exactly the panel size")

			// The centre always shows the photo, never a letterbox bar.
			cx, cy := out.Bounds().Dx()/2, out.Bounds().Dy()/2
			r, g, b, _ := out.At(cx, cy).RGBA()
			assert.Greater(t, r, uint32(0x8000), "centre should be the red source, not a bar")
			assert.Equal(t, uint32(0), g)
			assert.Equal(t, uint32(0), b)

			// Corners are the photo under cover, and the fill under fit.
			r, g, b, a := out.At(0, 0).RGBA()
			if tc.wantCover {
				assert.Greater(t, r, uint32(0x8000), "cover should reach the corner")
				return
			}
			got := color.RGBA{uint8(r >> 8), uint8(g >> 8), uint8(b >> 8), uint8(a >> 8)}
			assert.Equal(t, tc.wantCorner, got, "corner should be the letterbox fill")
		})
	}
}

// A cover crop must sample the centre of the source, not scale the whole
// source into view, otherwise tall photos lose their subject.
func TestFitToPanelCoverCropsFromCentre(t *testing.T) {
	// 100 wide, 400 tall: only the middle band is green, the rest blue.
	src := solid(100, 400, color.RGBA{0, 0, 0xff, 0xff})
	for y := 190; y < 210; y++ {
		for x := 0; x < 100; x++ {
			src.SetRGBA(x, y, color.RGBA{0, 0xff, 0, 0xff})
		}
	}

	// Cover into a 100×100 panel keeps only ~the middle 25 rows.
	out := fitToPanel(src, 100, 100, "cover", "")
	_, g, _, _ := out.At(50, 50).RGBA()
	assert.Greater(t, g, uint32(0x8000), "centre of the panel should be the green band")

	// Fit into the same panel shrinks the whole photo, so the green band is
	// thinner but the corners are still background.
	out = fitToPanel(src, 100, 100, "fit", "")
	_, g, _, _ = out.At(50, 50).RGBA()
	assert.Greater(t, g, uint32(0x8000), "fit also keeps the green band at the centre")
}

// Degenerate inputs must pass the source through instead of allocating a
// zero-sized canvas.
func TestFitToPanelPassesThroughOnBadInput(t *testing.T) {
	src := solid(10, 10, color.RGBA{1, 2, 3, 4})
	assert.Same(t, src, fitToPanel(src, 0, 10, "cover", ""))
	assert.Same(t, src, fitToPanel(src, 10, 0, "cover", ""))
	assert.Same(t, src, fitToPanel(src, -1, -1, "cover", ""))
}

func TestParsePanelColor(t *testing.T) {
	cases := []struct {
		in   string
		want color.RGBA
	}{
		{"", color.RGBA{0xff, 0xff, 0xff, 0xff}},
		{"#FFFFFF", color.RGBA{0xff, 0xff, 0xff, 0xff}},
		{"FFFFFF", color.RGBA{0xff, 0xff, 0xff, 0xff}},
		{"#000000", color.RGBA{0, 0, 0, 0xff}},
		{"#f00", color.RGBA{0xff, 0, 0, 0xff}},
		{"0f0", color.RGBA{0, 0xff, 0, 0xff}},
		{"#00FF00", color.RGBA{0, 0xff, 0, 0xff}},
		{"  #123456  ", color.RGBA{0x12, 0x34, 0x56, 0xff}},
		{"#11223344", color.RGBA{0x11, 0x22, 0x33, 0x44}},
		// Anything unparseable falls back to the converter's own default so a
		// bad UI value cannot introduce a colour the dithered path never had.
		{"nonsense", color.RGBA{0xff, 0xff, 0xff, 0xff}},
		{"#12345", color.RGBA{0xff, 0xff, 0xff, 0xff}},
		{"#GGGGGG", color.RGBA{0xff, 0xff, 0xff, 0xff}},
	}
	for _, tc := range cases {
		t.Run(tc.in, func(t *testing.T) {
			require.Equal(t, tc.want, parsePanelColor(tc.in))
		})
	}
}
