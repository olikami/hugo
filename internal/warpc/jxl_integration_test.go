// Copyright 2026 The Hugo Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package warpc_test

import (
	"bytes"
	"image"
	"image/draw"
	"path/filepath"
	"regexp"
	"runtime"
	"testing"

	qt "github.com/frankban/quicktest"
	"github.com/gohugoio/hugo/common/himage"
	"github.com/gohugoio/hugo/htesting"
	"github.com/gohugoio/hugo/hugolib"
	"github.com/spf13/afero"
)

func TestJxlBasic(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- assets/sunset.jxl --
sourcefilename: ../../resources/testdata/jxl/sunset.jxl
-- assets/sunset.jpg --
sourcefilename: ../../resources/testdata/sunset.jpg
-- layouts/home.html --
{{ $jxl := resources.Get "sunset.jxl" }}
{{ $jpg := resources.Get "sunset.jpg" }}
{{ $ic := images.Config "/assets/sunset.jxl" }}
Width/Height: {{ $jxl.Width }}/{{ $jxl.Height }}|
ImageConfig: {{ $ic.Width }}/{{ $ic.Height }}|
MediaType: {{ $jxl.MediaType }}|
Resized: {{ with $jxl.Resize "300x" }}{{ .Width }}/{{ .Height }}{{ end }}|
JXL to JPEG: {{ ($jxl.Resize "200x jpg").RelPermalink }}|
JPEG to JXL: {{ ($jpg.Resize "200x jxl").RelPermalink }}|
JXL to JXL: {{ ($jxl.Resize "200x q50").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	b.AssertFileContent("public/index.html",
		"Width/Height: 900/562|",
		"ImageConfig: 900/562|",
		"MediaType: image/jxl|",
		"Resized: 300/187|",
	)

	b.ImageHelper(publishedImage(b, "JXL to JPEG")).AssertFormat("jpeg")
	b.ImageHelper(publishedImage(b, "JPEG to JXL")).AssertFormat("jxl").AssertIsAnimated(false)
	b.ImageHelper(publishedImage(b, "JXL to JXL")).AssertFormat("jxl")
}

func TestJxlLossless(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- hugo.toml --
[imaging.jxl]
compression = "lossless"
-- assets/fuzzy.png --
sourcefilename: ../../resources/testdata/fuzzy-cirlcle.png
-- assets/gopher.png --
sourcefilename: ../../resources/testdata/bw-gopher.png
-- layouts/home.html --
{{ $fuzzy := resources.Get "fuzzy.png" }}
{{ $gopher := resources.Get "gopher.png" }}
Fuzzy: {{ ($fuzzy.Process "jxl").RelPermalink }}|
Gopher: {{ ($gopher.Process "jxl").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	for _, test := range []struct{ key, src string }{
		{"Fuzzy", "fuzzy-cirlcle.png"},
		{"Gopher", "bw-gopher.png"},
	} {
		filename := publishedImage(b, test.key)
		b.ImageHelper(filename).AssertFormat("jxl")
		want := decodeImage(b, "../../resources/testdata/"+test.src, afero.NewOsFs())
		got := decodeImage(b, filename, b.H.Fs.WorkingDirReadOnly)
		b.Assert(toNRGBA(got).Pix, qt.DeepEquals, toNRGBA(want).Pix, qt.Commentf(test.key))
	}
}

func TestJxlTransparency(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- assets/fuzzy.webp --
sourcefilename: ../../resources/testdata/webp/fuzzy-cirlcle-transparent-32.webp
-- layouts/home.html --
{{ $img := resources.Get "fuzzy.webp" }}
{{ $jxl := $img.Process "jxl" }}
JXL: {{ $jxl.RelPermalink }}|
PNG: {{ ($jxl.Process "png").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	b.ImageHelper(publishedImage(b, "JXL")).AssertFormat("jxl")
	img := toNRGBA(decodeImage(b, publishedImage(b, "PNG"), b.H.Fs.WorkingDirReadOnly))
	b.Assert(img.NRGBAAt(0, 0).A, qt.Equals, uint8(0))
	b.Assert(img.Opaque(), qt.IsFalse)
}

func TestJxlAnimation(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- hugo.toml --
disableKinds = ["page", "section", "taxonomy", "term", "sitemap", "robotsTXT", "404"]
-- assets/giphy.gif --
sourcefilename: ../../resources/testdata/giphy.gif
-- assets/giphy.jxl --
sourcefilename: ../../resources/testdata/jxl/giphy.jxl
-- assets/anim.webp --
sourcefilename: ../../resources/testdata/webp/anim.webp
-- layouts/home.html --
{{ $gif := resources.Get "giphy.gif" }}
{{ $jxl := resources.Get "giphy.jxl" }}
{{ $webp := resources.Get "anim.webp" }}
GIF to JXL: {{ ($gif.Resize "100x jxl").RelPermalink }}|
JXL to GIF: {{ ($jxl.Resize "100x gif").RelPermalink }}|
JXL to WEBP: {{ ($jxl.Resize "100x webp").RelPermalink }}|
WEBP to JXL: {{ ($webp.Resize "100x jxl").RelPermalink }}|
JXL to PNG: {{ ($jxl.Resize "100x png").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	giphyDurations := make([]int, 14)
	for i := range giphyDurations {
		giphyDurations[i] = 200
	}
	animDurations := make([]int, 17)
	for i := range animDurations {
		animDurations[i] = 80
	}

	b.ImageHelper(publishedImage(b, "GIF to JXL")).AssertFormat("jxl").AssertIsAnimated(true).AssertLoopCount(0).AssertFrameDurations(giphyDurations)
	b.ImageHelper(publishedImage(b, "JXL to GIF")).AssertFormat("gif").AssertIsAnimated(true).AssertLoopCount(0).AssertFrameDurations(giphyDurations)
	b.ImageHelper(publishedImage(b, "JXL to WEBP")).AssertFormat("webp").AssertIsAnimated(true).AssertLoopCount(0).AssertFrameDurations(giphyDurations)
	b.ImageHelper(publishedImage(b, "WEBP to JXL")).AssertFormat("jxl").AssertIsAnimated(true).AssertLoopCount(0).AssertFrameDurations(animDurations)
	b.ImageHelper(publishedImage(b, "JXL to PNG")).AssertFormat("png").AssertIsAnimated(false)
}

func TestJxlHighBitDepth(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- assets/sunset.jxl --
sourcefilename: ../../resources/testdata/jxl/sunset-16bit.jxl
-- layouts/home.html --
{{ $img := resources.Get "sunset.jxl" }}
JXL: {{ ($img.Resize "100x").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	filename := publishedImage(b, "JXL")
	b.ImageHelper(filename).AssertFormat("jxl")
	_, ok := decodeImage(b, filename, b.H.Fs.WorkingDirReadOnly).(*image.NRGBA64)
	b.Assert(ok, qt.IsTrue)
}

// dock-75-hdr.avif has a gain map, which the AVIF decoder bakes into BT.2020/PQ.
func TestJxlHDR(t *testing.T) {
	htesting.SkipSlowWasmTestOn32Bit(t)

	files := `
-- assets/dock.avif --
sourcefilename: ../../resources/testdata/bep/dock-75-hdr.avif
-- layouts/home.html --
{{ $jxl := (resources.Get "dock.avif").Resize "200x jxl" }}
JXL: {{ $jxl.RelPermalink }}|
AVIF: {{ ($jxl.Process "avif").RelPermalink }}|
`

	b := hugolib.Test(t, files)

	for _, key := range []string{"JXL", "AVIF"} {
		img := decodeImage(b, publishedImage(b, key), b.H.Fs.WorkingDirReadOnly)
		cpp, ok := img.(himage.ColorPropertiesProvider)
		b.Assert(ok, qt.IsTrue, qt.Commentf(key))
		b.Assert(cpp.GetColorPrimaries(), qt.Equals, 9, qt.Commentf(key))           // BT.2020
		b.Assert(cpp.GetTransferCharacteristics(), qt.Equals, 16, qt.Commentf(key)) // PQ
	}
}

func TestJxlInvalid(t *testing.T) {
	files := `
-- assets/invalid.jxl --
sourcefilename: ../../resources/testdata/jxl/invalid.jxl
-- layouts/home.html --
{{ $image := resources.Get "invalid.jxl" }}
{{ $resized := $image.Resize "123x456 webp" }}
Resized RelPermalink: {{ $resized.RelPermalink }}|
`
	tempDir := t.TempDir()

	b, err := hugolib.TestE(t, files, hugolib.TestOptWithConfig(func(cfg *hugolib.IntegrationTestConfig) {
		cfg.NeedsOsFS = true
		cfg.WorkingDir = tempDir
	}))
	b.Assert(err, qt.IsNotNil)

	if runtime.GOOS != "windows" {
		b.Assert(err.Error(), qt.Contains, filepath.Join(tempDir, "assets/invalid.jxl"))
	}
}

// The libjxl encoder needs a little more than 8 MiB, so use more memory than in the WebP equivalent.
func TestJxlEncodeOutOfMemory(t *testing.T) {
	files := `
-- assets/gopher.png --
sourcefilename: ../../resources/testdata/bw-gopher.png
-- layouts/home.html --
{{ $img := resources.Get "gopher.png" }}
{{ $r := try ($img.Resize "4000x4000 jxl") }}
{{ with $r.Err }}BigErr: {{ . }}|{{ else }}BigOK|{{ end }}
{{ $small := $img.Resize "32x32 jxl" }}
SmallAfter: {{ $small.RelPermalink }}|
`

	b := hugolib.Test(t, files, hugolib.TestOptWithConfig(func(c *hugolib.IntegrationTestConfig) {
		c.WarpcMemory = 16
	}))

	b.AssertFileContent("public/index.html",
		"BigErr:",
		"out of memory allocating",
		"for blob data",
		"SmallAfter: /gopher_",
	)
}

var publishedImageRe = regexp.MustCompile(`(?m)^([^:\n]+): (/[^|]+)\|`)

// publishedImage returns the public path of the image published on the
// "key: /path|" line in public/index.html.
func publishedImage(b *hugolib.IntegrationTestBuilder, key string) string {
	b.Helper()
	for _, m := range publishedImageRe.FindAllStringSubmatch(b.FileContent("public/index.html"), -1) {
		if m[1] == key {
			return "public" + m[2]
		}
	}
	b.Fatalf("no published image for %q", key)
	return ""
}

func decodeImage(b *hugolib.IntegrationTestBuilder, filename string, fs afero.Fs) image.Image {
	b.Helper()
	data, err := afero.ReadFile(fs, filename)
	b.Assert(err, qt.IsNil)
	img, err := b.H.ResourceSpec.Imaging.Codec.Decode(bytes.NewReader(data))
	b.Assert(err, qt.IsNil)
	return img
}

func toNRGBA(img image.Image) *image.NRGBA {
	dst := image.NewNRGBA(img.Bounds())
	draw.Draw(dst, dst.Bounds(), img, img.Bounds().Min, draw.Src)
	return dst
}
