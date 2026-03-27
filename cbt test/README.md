# Waveforge Studio

Waveforge Studio is a small browser app with two parts:

- an image-to-image generator that takes one uploaded image and creates a downloadable output image
- a separate sine/cosine window that shows wave graphs, formulas, and live values

## Files

- `index.html`: main image generation window
- `app.js`: image processing and download logic
- `waves.html`: separate trig window
- `waves.js`: sine/cosine plotting logic
- `styles.css`: shared styling for both windows

## How To Use

1. Open `index.html` in your browser.
2. Upload an image.
3. Adjust the generator controls or use `Randomize Settings`.
4. Click `Generate Output`.
5. Click `Download PNG` to save the result.
6. Click `Open Sin/Cos Window` to open the separate wave viewer.

## Notes

- Everything runs locally in the browser.
- No install step is required.
- The image generator is procedural and uses wave-based distortion plus color remapping, so it works like a compact art generator rather than a trained diffusion model.
