const sourceCanvas = document.getElementById("sourceCanvas");
const outputCanvas = document.getElementById("outputCanvas");
const sourceCtx = sourceCanvas.getContext("2d");
const outputCtx = outputCanvas.getContext("2d");
const hiddenSourceCanvas = document.createElement("canvas");
const hiddenSourceCtx = hiddenSourceCanvas.getContext("2d");

const controls = {
    imageInput: document.getElementById("imageInput"),
    styleSelect: document.getElementById("styleSelect"),
    strengthRange: document.getElementById("strengthRange"),
    waveRange: document.getElementById("waveRange"),
    swirlRange: document.getElementById("swirlRange"),
    colorRange: document.getElementById("colorRange"),
    grainRange: document.getElementById("grainRange"),
    contrastRange: document.getElementById("contrastRange"),
    generateButton: document.getElementById("generateButton"),
    randomizeButton: document.getElementById("randomizeButton"),
    downloadButton: document.getElementById("downloadButton"),
    openWaveWindow: document.getElementById("openWaveWindow"),
    statusText: document.getElementById("statusText"),
    strengthValue: document.getElementById("strengthValue"),
    waveValue: document.getElementById("waveValue"),
    swirlValue: document.getElementById("swirlValue"),
    colorValue: document.getElementById("colorValue"),
    grainValue: document.getElementById("grainValue"),
    contrastValue: document.getElementById("contrastValue"),
};

const state = {
    ready: false,
    renderTimer: 0,
    fileName: "generated-image",
};

const STYLE_LABELS = {
    aurora: "Aurora Warp",
    contour: "Contour Glow",
    prism: "Prism Pulse",
    bloom: "Pixel Bloom",
};

function clamp(value, min = 0, max = 255) {
    return Math.max(min, Math.min(max, value));
}

function fract(value) {
    return value - Math.floor(value);
}

function setStatus(text) {
    controls.statusText.textContent = text;
}

function syncDisplayValues() {
    controls.strengthValue.textContent = Number(controls.strengthRange.value).toFixed(2);
    controls.waveValue.textContent = Number(controls.waveRange.value).toFixed(2);
    controls.swirlValue.textContent = Number(controls.swirlRange.value).toFixed(2);
    controls.colorValue.textContent = `${Math.round(Number(controls.colorRange.value))}`;
    controls.grainValue.textContent = Number(controls.grainRange.value).toFixed(2);
    controls.contrastValue.textContent = Number(controls.contrastRange.value).toFixed(2);
}

function drawPlaceholder(ctx, width, height, title, subtitle, colors) {
    ctx.clearRect(0, 0, width, height);
    const gradient = ctx.createLinearGradient(0, 0, width, height);
    gradient.addColorStop(0, colors[0]);
    gradient.addColorStop(1, colors[1]);
    ctx.fillStyle = gradient;
    ctx.fillRect(0, 0, width, height);

    for (let i = 0; i < 18; i += 1) {
        const x = width * (0.08 + (i / 18) * 0.84);
        const waveHeight = height * (0.25 + Math.sin(i * 0.7) * 0.09);
        ctx.strokeStyle = `rgba(255,255,255,${0.08 + i * 0.01})`;
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(0, waveHeight);
        for (let px = 0; px <= width; px += 16) {
            const py = waveHeight + Math.sin((px * 0.016) + i * 0.5) * (12 + i);
            ctx.lineTo(px, py);
        }
        ctx.stroke();
        ctx.beginPath();
        ctx.arc(x, height * 0.18, 1.6 + i * 0.18, 0, Math.PI * 2);
        ctx.fillStyle = "rgba(255,255,255,0.08)";
        ctx.fill();
    }

    ctx.fillStyle = "rgba(255,255,255,0.92)";
    ctx.font = "700 28px 'Trebuchet MS', sans-serif";
    ctx.fillText(title, 30, height - 72);
    ctx.font = "500 16px 'Trebuchet MS', sans-serif";
    ctx.fillStyle = "rgba(255,255,255,0.84)";
    ctx.fillText(subtitle, 30, height - 42);
}

function fitSize(width, height, maxSide = 760) {
    const scale = Math.min(1, maxSide / Math.max(width, height));
    return {
        width: Math.max(1, Math.round(width * scale)),
        height: Math.max(1, Math.round(height * scale)),
    };
}

function getPixelIndex(width, height, x, y) {
    const clampedX = Math.max(0, Math.min(width - 1, Math.round(x)));
    const clampedY = Math.max(0, Math.min(height - 1, Math.round(y)));
    return (clampedY * width + clampedX) * 4;
}

function scheduleRender() {
    if (!state.ready) {
        return;
    }
    window.clearTimeout(state.renderTimer);
    state.renderTimer = window.setTimeout(renderOutput, 90);
}

function loadImage(file) {
    if (!file) {
        return;
    }

    const reader = new FileReader();
    reader.onload = () => {
        const image = new Image();
        image.onload = () => {
            const fitted = fitSize(image.width, image.height);
            hiddenSourceCanvas.width = fitted.width;
            hiddenSourceCanvas.height = fitted.height;
            sourceCanvas.width = fitted.width;
            sourceCanvas.height = fitted.height;
            outputCanvas.width = fitted.width;
            outputCanvas.height = fitted.height;

            hiddenSourceCtx.clearRect(0, 0, fitted.width, fitted.height);
            hiddenSourceCtx.drawImage(image, 0, 0, fitted.width, fitted.height);
            sourceCtx.clearRect(0, 0, fitted.width, fitted.height);
            sourceCtx.drawImage(image, 0, 0, fitted.width, fitted.height);
            outputCtx.clearRect(0, 0, fitted.width, fitted.height);

            state.ready = true;
            state.fileName = (file.name || "generated-image").replace(/\.[^.]+$/, "");
            controls.downloadButton.disabled = true;
            setStatus(`Loaded ${fitted.width} x ${fitted.height}. Generating a first pass...`);
            renderOutput();
        };
        image.src = reader.result;
    };
    reader.readAsDataURL(file);
}

function getSettings() {
    return {
        style: controls.styleSelect.value,
        strength: Number(controls.strengthRange.value),
        waveMix: Number(controls.waveRange.value),
        swirl: Number(controls.swirlRange.value),
        colorDrift: Number(controls.colorRange.value),
        grain: Number(controls.grainRange.value),
        contrast: Number(controls.contrastRange.value),
    };
}

function renderOutput() {
    if (!state.ready) {
        setStatus("Upload an image to begin.");
        return;
    }

    const { style, strength, waveMix, swirl, colorDrift, grain, contrast } = getSettings();
    const width = hiddenSourceCanvas.width;
    const height = hiddenSourceCanvas.height;
    const phase = (colorDrift * Math.PI) / 180;
    const sourceImageData = hiddenSourceCtx.getImageData(0, 0, width, height);
    const src = sourceImageData.data;
    const outputImage = outputCtx.createImageData(width, height);
    const out = outputImage.data;

    setStatus(`Generating ${STYLE_LABELS[style]}...`);

    for (let y = 0; y < height; y += 1) {
        const ny = y / height - 0.5;
        for (let x = 0; x < width; x += 1) {
            const nx = x / width - 0.5;
            const radius = Math.hypot(nx, ny);
            const angle = Math.atan2(ny, nx);

            const waveOffsetX =
                Math.sin((ny * 10.5) + phase + radius * 14) * waveMix * 24 +
                Math.cos(angle * 2.8 + radius * 8) * swirl * 14;
            const waveOffsetY =
                Math.cos((nx * 10.8) - phase + radius * 12) * waveMix * 24 +
                Math.sin(angle * 2.2 - radius * 9) * swirl * 14;

            const spiral = (1.15 - radius) * strength * 18;
            const sampleX = x + waveOffsetX + Math.cos(angle * 3 + phase) * spiral;
            const sampleY = y + waveOffsetY + Math.sin(angle * 3 - phase) * spiral;

            const baseIndex = getPixelIndex(width, height, sampleX, sampleY);
            const edgeA = getPixelIndex(width, height, sampleX + 2, sampleY + 2);
            const edgeB = getPixelIndex(width, height, sampleX - 2, sampleY - 2);

            const baseR = src[baseIndex];
            const baseG = src[baseIndex + 1];
            const baseB = src[baseIndex + 2];
            const edgeR = Math.abs(src[edgeA] - src[edgeB]);
            const edgeG = Math.abs(src[edgeA + 1] - src[edgeB + 1]);
            const edgeBValue = Math.abs(src[edgeA + 2] - src[edgeB + 2]);
            const edgeStrength = (edgeR + edgeG + edgeBValue) / 3;
            const luminance = baseR * 0.299 + baseG * 0.587 + baseB * 0.114;

            let red;
            let green;
            let blue;

            if (style === "aurora") {
                red = baseR * contrast + Math.sin(nx * 14 + phase + radius * 18) * 42 * strength + edgeStrength * 0.15;
                green = baseG * contrast + Math.cos(ny * 13 - phase) * 34 * strength + 14;
                blue = baseB * contrast + Math.sin((nx + ny) * 12 + phase) * 48 * waveMix + radius * 65;
            } else if (style === "contour") {
                red = luminance * contrast + edgeStrength * 1.18 + 18;
                green = luminance * 0.82 * contrast + edgeStrength * 0.72 + Math.cos(radius * 24 + phase) * 12;
                blue = luminance * 1.06 * contrast + edgeStrength * 1.34 + 34;
            } else if (style === "prism") {
                const prismR = getPixelIndex(width, height, sampleX + strength * 12, sampleY);
                const prismB = getPixelIndex(width, height, sampleX - strength * 12, sampleY);
                red = src[prismR] * contrast + Math.sin(radius * 18 + phase) * 24;
                green = baseG * contrast + Math.cos((nx - ny) * 11 - phase) * 18;
                blue = src[prismB + 2] * contrast + Math.sin((nx + ny) * 16 + phase) * 32;
            } else {
                const quant = 52 - Math.round(strength * 18);
                red = Math.round(baseR / quant) * quant + Math.sin(radius * 24 + phase) * 22 * waveMix;
                green = Math.round(baseG / quant) * quant + edgeStrength * 0.3 + 10;
                blue = Math.round(baseB / quant) * quant + Math.cos(radius * 20 - phase) * 26 * strength;
            }

            const grainNoise =
                (fract(Math.sin((x + 1) * 127.1 + (y + 1) * 311.7 + phase * 12.3) * 43758.5453) - 0.5) *
                grain *
                58;

            const index = (y * width + x) * 4;
            out[index] = clamp(red + grainNoise);
            out[index + 1] = clamp(green + grainNoise * 0.7);
            out[index + 2] = clamp(blue + grainNoise * 1.15);
            out[index + 3] = 255;
        }
    }

    outputCtx.putImageData(outputImage, 0, 0);
    controls.downloadButton.disabled = false;
    setStatus(`Finished ${STYLE_LABELS[style]}. You can download the PNG now.`);
}

function randomizeControls() {
    controls.styleSelect.selectedIndex = Math.floor(Math.random() * controls.styleSelect.options.length);
    controls.strengthRange.value = (0.25 + Math.random() * 0.9).toFixed(2);
    controls.waveRange.value = (0.15 + Math.random() * 0.95).toFixed(2);
    controls.swirlRange.value = (0.05 + Math.random() * 1.05).toFixed(2);
    controls.colorRange.value = `${Math.round(Math.random() * 180)}`;
    controls.grainRange.value = (Math.random() * 0.45).toFixed(2);
    controls.contrastRange.value = (0.9 + Math.random() * 0.7).toFixed(2);
    syncDisplayValues();
    scheduleRender();
}

function downloadOutput() {
    if (!state.ready) {
        return;
    }
    const link = document.createElement("a");
    const style = controls.styleSelect.value;
    link.href = outputCanvas.toDataURL("image/png");
    link.download = `${state.fileName}-${style}.png`;
    link.click();
}

function openWaveWindow() {
    window.open("waves.html", "_blank", "width=1260,height=900");
}

function bindControls() {
    syncDisplayValues();

    drawPlaceholder(
        sourceCtx,
        sourceCanvas.width,
        sourceCanvas.height,
        "Source preview",
        "Upload an image to start the generator.",
        ["#f36b3d", "#ffb56e"]
    );

    drawPlaceholder(
        outputCtx,
        outputCanvas.width,
        outputCanvas.height,
        "Generated output",
        "Your transformed image will appear here.",
        ["#18a7b5", "#88dae1"]
    );

    controls.imageInput.addEventListener("change", (event) => {
        const [file] = event.target.files;
        loadImage(file);
    });

    [
        controls.styleSelect,
        controls.strengthRange,
        controls.waveRange,
        controls.swirlRange,
        controls.colorRange,
        controls.grainRange,
        controls.contrastRange,
    ].forEach((element) => {
        element.addEventListener("input", () => {
            syncDisplayValues();
            scheduleRender();
        });
    });

    controls.generateButton.addEventListener("click", renderOutput);
    controls.randomizeButton.addEventListener("click", randomizeControls);
    controls.downloadButton.addEventListener("click", downloadOutput);
    controls.openWaveWindow.addEventListener("click", openWaveWindow);
}

bindControls();
