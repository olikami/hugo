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

package warpc

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"io"

	"github.com/gohugoio/hugo/common/himage"
	"github.com/gohugoio/hugo/common/hugio"
)

// Keep in sync with genjxl/Makefile.
const libjxlVersion = "v0.12.0"

var (
	_ SourceProvider      = JxlInput{}
	_ DestinationProvider = JxlInput{}
)

type JxlInput struct {
	Source      hugio.SizeReader `json:"-"`       // Will be sent in a separate stream.
	Destination io.Writer        `json:"-"`       // Will be used to write the result to.
	Options     map[string]any   `json:"options"` // Config options.
	Params      map[string]any   `json:"params"`  // Command params (width, height, etc.).
}

func (j JxlInput) GetSource() hugio.SizeReader {
	return j.Source
}

func (j JxlInput) GetDestination() io.Writer {
	return j.Destination
}

type JxlOutput struct {
	Params CommonImageProcessingParams `json:"params"`
}

type JxlCodec struct {
	d func() (Dispatcher[JxlInput, JxlOutput], error)
}

func (d *JxlCodec) execute(command string, data JxlInput) (Message[JxlOutput], error) {
	dd, err := d.d()
	if err != nil {
		return Message[JxlOutput]{}, err
	}
	responseKinds := []string{MessageKindJSON, MessageKindBlob}
	if data.Destination == nil {
		responseKinds = responseKinds[:1]
	}
	return dd.Execute(context.Background(), Message[JxlInput]{
		Header: Header{
			Version:       1,
			Command:       command,
			RequestKinds:  []string{MessageKindJSON, MessageKindBlob},
			ResponseKinds: responseKinds,
		},
		Data: data,
	})
}

func (d *JxlCodec) DecodeConfig(r io.Reader) (image.Config, error) {
	source, err := hugio.ToSizeReader(r)
	if err != nil {
		return image.Config{}, err
	}
	out, err := d.execute("config", JxlInput{Source: source})
	if err != nil {
		return image.Config{}, err
	}
	return image.Config{
		Width:      out.Data.Params.Width,
		Height:     out.Data.Params.Height,
		ColorModel: color.NRGBAModel,
	}, nil
}

// Decode decodes a JPEG XL image into an *image.NRGBA, an *image.NRGBA64 (more than 8 bits per sample)
// or, for animations and HDR images, an *AnimatedImage.
func (d *JxlCodec) Decode(r io.Reader) (image.Image, error) {
	source, err := hugio.ToSizeReader(r)
	if err != nil {
		return nil, err
	}
	var destination bytes.Buffer
	out, err := d.execute("decode", JxlInput{Source: source, Destination: &destination})
	if err != nil {
		return nil, err
	}

	p := out.Data.Params
	if p.Width == 0 || p.Height == 0 || p.Stride == 0 {
		return nil, fmt.Errorf("received invalid image dimensions: %dx%d stride %d", p.Width, p.Height, p.Stride)
	}

	pix := destination.Bytes()
	frameSize := p.Stride * p.Height
	numFrames := max(1, len(p.FrameDurations))
	if len(pix) != frameSize*numFrames {
		return nil, fmt.Errorf("decoded JXL buffer size %d does not match %d frame(s) of size %d", len(pix), numFrames, frameSize)
	}

	rect := image.Rect(0, 0, p.Width, p.Height)
	frames := make([]image.Image, numFrames)
	for i := range frames {
		framePix := pix[i*frameSize : (i+1)*frameSize]
		if p.Depth > 8 {
			// The decoder writes 16-bit samples in big endian, as expected by Go.
			frames[i] = &image.NRGBA64{Pix: framePix, Stride: p.Stride, Rect: rect}
		} else {
			frames[i] = &image.NRGBA{Pix: framePix, Stride: p.Stride, Rect: rect}
		}
	}

	if len(p.FrameDurations) == 0 && p.TransferCharacteristics == 0 {
		return frames[0], nil
	}

	img := &AnimatedImage{
		frameDurations:          p.FrameDurations,
		loopCount:               jxlLoopCountToGo(p.LoopCount),
		depth:                   p.Depth,
		colorPrimaries:          p.ColorPrimaries,
		transferCharacteristics: p.TransferCharacteristics,
		matrixCoefficients:      p.MatrixCoefficients,
	}
	img.SetFrames(frames)
	return img, nil
}

func (d *JxlCodec) Encode(w io.Writer, src image.Image, options map[string]any) error {
	const (
		commandEncodeNRGBA = "encodeNRGBA"
		commandEncodeGray  = "encodeGray"
	)

	b := src.Bounds()
	params := map[string]any{
		"width":  b.Dx(),
		"height": b.Dy(),
		"depth":  8,
	}
	command := commandEncodeNRGBA
	var pix []byte

	if anim, ok := src.(himage.AnimatedImage); ok {
		frames := anim.GetFrames()
		if len(frames) == 0 {
			return errors.New("jxl: animated image has no frames")
		}
		if cpp, ok := src.(himage.ColorPropertiesProvider); ok && himage.HasColorProperties(src) {
			params["colorPrimaries"] = cpp.GetColorPrimaries()
			params["transferCharacteristics"] = cpp.GetTransferCharacteristics()
		}
		if v, ok := src.(*AnimatedImage); ok && v.depth > 8 {
			params["bitsPerSample"] = v.depth
		}
		if len(frames) == 1 {
			src = frames[0]
			b = src.Bounds()
		} else {
			b = frames[0].Bounds()
			frameSize := 4 * b.Dx() * b.Dy()
			pix = make([]byte, 0, frameSize*len(frames))
			for _, frame := range frames {
				nrgba := convertToNRGBA(frame)
				if nrgba.Stride*nrgba.Rect.Dy() != frameSize {
					return errors.New("jxl: animation frames must have the same size")
				}
				pix = append(pix, nrgba.Pix...)
			}
			params["width"], params["height"], params["stride"] = b.Dx(), b.Dy(), 4*b.Dx()
			params["frameDurations"] = anim.GetFrameDurations()
			params["loopCount"] = goLoopCountToJxl(anim.GetLoopCount())
		}
	}

	if pix == nil {
		switch src.ColorModel() {
		case color.RGBA64Model, color.NRGBA64Model, color.Gray16Model:
			params["depth"] = 16
		}
		switch v := src.(type) {
		case *image.NRGBA:
			pix, params["stride"] = v.Pix[v.PixOffset(b.Min.X, b.Min.Y):], v.Stride
		case *image.NRGBA64:
			pix, params["stride"] = v.Pix[v.PixOffset(b.Min.X, b.Min.Y):], v.Stride
		case *image.Gray:
			pix, params["stride"] = v.Pix[v.PixOffset(b.Min.X, b.Min.Y):], v.Stride
			command = commandEncodeGray
		case *image.Gray16:
			pix, params["stride"] = v.Pix[v.PixOffset(b.Min.X, b.Min.Y):], v.Stride
			command = commandEncodeGray
		default:
			if params["depth"] == 16 {
				dst := image.NewNRGBA64(image.Rect(0, 0, b.Dx(), b.Dy()))
				draw.Draw(dst, dst.Bounds(), src, b.Min, draw.Src)
				pix, params["stride"] = dst.Pix, dst.Stride
			} else {
				dst := convertToNRGBA(src)
				pix, params["stride"] = dst.Pix, dst.Stride
			}
		}
	}

	source, err := hugio.ToSizeReader(bytes.NewReader(pix))
	if err != nil {
		return err
	}
	_, err = d.execute(command, JxlInput{
		Source:      source,
		Destination: w,
		Options:     options,
		Params:      params,
	})
	return err
}

// jxlLoopCountToGo translates libjxl's num_loops (0 = infinite, N = play N
// times) to the convention used by image/gif (0 = infinite, -1 = play once,
// N = play N+1 times).
func jxlLoopCountToGo(numLoops int) int {
	if numLoops == 0 {
		return 0
	}
	return numLoops - 1
}

// goLoopCountToJxl is the inverse of jxlLoopCountToGo.
func goLoopCountToJxl(loopCount int) int {
	if loopCount == 0 {
		return 0
	}
	if loopCount < 0 {
		return 1
	}
	return loopCount + 1
}
