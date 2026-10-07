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

package images

import (
	"bytes"
	"testing"

	qt "github.com/frankban/quicktest"
)

func TestFormatFromImage(t *testing.T) {
	c := qt.New(t)

	for _, test := range []struct {
		name   string
		in     []byte
		expect Format
	}{
		{"jxl codestream", []byte{0xff, 0x0a, 0xfa, 0x1f}, JXL},
		{"jxl container", []byte{0, 0, 0, 0x0c, 'J', 'X', 'L', ' ', 0x0d, 0x0a, 0x87, 0x0a, 0, 0, 0, 0x14}, JXL},
		{"jpeg", []byte{0xff, 0xd8, 0xff, 0xe0}, 0},
		{"gif", []byte("GIF89a......"), GIF},
		{"webp", []byte("RIFF\x00\x00\x00\x00WEBPVP8 "), WEBP},
		{"avif", []byte("\x00\x00\x00\x1cftypavif\x00\x00\x00\x00"), AVIF},
		{"short", []byte{0xff}, 0},
	} {
		c.Run(test.name, func(c *qt.C) {
			f, err := formatFromImage(toPeekReader(bytes.NewReader(test.in)))
			c.Assert(err, qt.IsNil)
			c.Assert(f, qt.Equals, test.expect)
		})
	}
}
