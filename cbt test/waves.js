const waveCanvas = document.getElementById("waveCanvas");
const waveCtx = waveCanvas.getContext("2d");

const waveControls = {
    amplitudeRange: document.getElementById("amplitudeRange"),
    frequencyRange: document.getElementById("frequencyRange"),
    phaseRange: document.getElementById("phaseRange"),
    offsetRange: document.getElementById("offsetRange"),
    markerRange: document.getElementById("markerRange"),
    animateToggle: document.getElementById("animateToggle"),
    amplitudeValue: document.getElementById("amplitudeValue"),
    frequencyValue: document.getElementById("frequencyValue"),
    phaseValue: document.getElementById("phaseValue"),
    offsetValue: document.getElementById("offsetValue"),
    currentXValue: document.getElementById("currentXValue"),
    currentXText: document.getElementById("currentXText"),
    sinValueText: document.getElementById("sinValueText"),
    cosValueText: document.getElementById("cosValueText"),
    sinFormula: document.getElementById("sinFormula"),
    cosFormula: document.getElementById("cosFormula"),
    markerText: document.getElementById("markerText"),
    sampleTableBody: document.getElementById("sampleTableBody"),
    closeWindowButton: document.getElementById("closeWindowButton"),
};

const graphDomain = {
    min: -2 * Math.PI,
    max: 2 * Math.PI,
};

let animationFrame = 0;

function readWaveSettings() {
    return {
        amplitude: Number(waveControls.amplitudeRange.value),
        frequency: Number(waveControls.frequencyRange.value),
        phase: Number(waveControls.phaseRange.value),
        offset: Number(waveControls.offsetRange.value),
        currentX: Number(waveControls.markerRange.value),
    };
}

function formatNumber(value, digits = 2) {
    return Number(value).toFixed(digits);
}

function formatPi(value) {
    const ratio = value / Math.PI;
    if (Math.abs(ratio) < 0.001) {
        return "0";
    }
    const rounded = Math.round(ratio * 2) / 2;
    if (Math.abs(rounded) === 0.5) {
        return `${rounded < 0 ? "-" : ""}pi/2`;
    }
    if (Math.abs(rounded) === 1) {
        return `${rounded < 0 ? "-" : ""}pi`;
    }
    if (Math.abs(rounded % 1) < 0.001) {
        return `${rounded < 0 ? "-" : ""}${Math.abs(rounded)}pi`;
    }
    return `${formatNumber(value, 2)}`;
}

function getFunctions(settings) {
    return {
        sin: (x) => settings.amplitude * Math.sin(settings.frequency * x + settings.phase) + settings.offset,
        cos: (x) => settings.amplitude * Math.cos(settings.frequency * x + settings.phase) + settings.offset,
    };
}

function syncWaveLabels(settings) {
    waveControls.amplitudeValue.textContent = formatNumber(settings.amplitude);
    waveControls.frequencyValue.textContent = formatNumber(settings.frequency);
    waveControls.phaseValue.textContent = formatNumber(settings.phase);
    waveControls.offsetValue.textContent = formatNumber(settings.offset);
    waveControls.currentXValue.textContent = formatNumber(settings.currentX);
    waveControls.currentXText.textContent = formatNumber(settings.currentX);
    waveControls.markerText.textContent = `x = ${formatNumber(settings.currentX)}`;

    waveControls.sinFormula.textContent =
        `y = ${formatNumber(settings.amplitude)} sin(${formatNumber(settings.frequency)}x + ${formatNumber(settings.phase)}) + ${formatNumber(settings.offset)}`;
    waveControls.cosFormula.textContent =
        `y = ${formatNumber(settings.amplitude)} cos(${formatNumber(settings.frequency)}x + ${formatNumber(settings.phase)}) + ${formatNumber(settings.offset)}`;
}

function updateValues(settings, functions) {
    waveControls.sinValueText.textContent = formatNumber(functions.sin(settings.currentX), 4);
    waveControls.cosValueText.textContent = formatNumber(functions.cos(settings.currentX), 4);
}

function rebuildSampleTable(settings, functions) {
    const samplePoints = [-2 * Math.PI, -1.5 * Math.PI, -Math.PI, -0.5 * Math.PI, 0, 0.5 * Math.PI, Math.PI, 1.5 * Math.PI, 2 * Math.PI];
    waveControls.sampleTableBody.innerHTML = samplePoints
        .map((x) => {
            return `
                <tr>
                    <td>${formatPi(x)}</td>
                    <td>${formatNumber(functions.sin(x), 4)}</td>
                    <td>${formatNumber(functions.cos(x), 4)}</td>
                </tr>
            `;
        })
        .join("");
}

function drawGraph() {
    const settings = readWaveSettings();
    const functions = getFunctions(settings);
    syncWaveLabels(settings);
    updateValues(settings, functions);
    rebuildSampleTable(settings, functions);

    const width = waveCanvas.width;
    const height = waveCanvas.height;
    const padding = { top: 24, right: 30, bottom: 46, left: 56 };
    const plotWidth = width - padding.left - padding.right;
    const plotHeight = height - padding.top - padding.bottom;
    const verticalLimit = Math.max(2.6, Math.abs(settings.offset) + settings.amplitude + 0.8);

    waveCtx.clearRect(0, 0, width, height);

    const background = waveCtx.createLinearGradient(0, 0, width, height);
    background.addColorStop(0, "rgba(255,255,255,0.95)");
    background.addColorStop(1, "rgba(245,250,251,0.98)");
    waveCtx.fillStyle = background;
    waveCtx.fillRect(0, 0, width, height);

    const xToPx = (x) => padding.left + ((x - graphDomain.min) / (graphDomain.max - graphDomain.min)) * plotWidth;
    const yToPx = (y) => padding.top + plotHeight - ((y + verticalLimit) / (verticalLimit * 2)) * plotHeight;

    waveCtx.strokeStyle = "rgba(22, 48, 71, 0.08)";
    waveCtx.lineWidth = 1;
    for (let tick = graphDomain.min; tick <= graphDomain.max + 0.01; tick += Math.PI / 2) {
        const x = xToPx(tick);
        waveCtx.beginPath();
        waveCtx.moveTo(x, padding.top);
        waveCtx.lineTo(x, height - padding.bottom);
        waveCtx.stroke();
    }

    for (let yTick = Math.ceil(-verticalLimit); yTick <= Math.floor(verticalLimit); yTick += 1) {
        const y = yToPx(yTick);
        waveCtx.beginPath();
        waveCtx.moveTo(padding.left, y);
        waveCtx.lineTo(width - padding.right, y);
        waveCtx.stroke();
    }

    waveCtx.strokeStyle = "rgba(22, 48, 71, 0.34)";
    waveCtx.lineWidth = 1.5;
    waveCtx.beginPath();
    waveCtx.moveTo(padding.left, yToPx(0));
    waveCtx.lineTo(width - padding.right, yToPx(0));
    waveCtx.stroke();

    waveCtx.beginPath();
    waveCtx.moveTo(xToPx(0), padding.top);
    waveCtx.lineTo(xToPx(0), height - padding.bottom);
    waveCtx.stroke();

    waveCtx.fillStyle = "rgba(95, 115, 134, 0.95)";
    waveCtx.font = "12px 'Trebuchet MS', sans-serif";
    for (let tick = graphDomain.min; tick <= graphDomain.max + 0.01; tick += Math.PI / 2) {
        waveCtx.fillText(formatPi(tick), xToPx(tick) - 10, height - 20);
    }
    for (let yTick = Math.ceil(-verticalLimit); yTick <= Math.floor(verticalLimit); yTick += 1) {
        if (yTick === 0) {
            continue;
        }
        waveCtx.fillText(String(yTick), 18, yToPx(yTick) + 4);
    }

    const plotFunction = (fn, color) => {
        waveCtx.strokeStyle = color;
        waveCtx.lineWidth = 3;
        waveCtx.beginPath();
        for (let step = 0; step <= plotWidth; step += 1) {
            const x = graphDomain.min + (step / plotWidth) * (graphDomain.max - graphDomain.min);
            const y = fn(x);
            const px = xToPx(x);
            const py = yToPx(y);
            if (step === 0) {
                waveCtx.moveTo(px, py);
            } else {
                waveCtx.lineTo(px, py);
            }
        }
        waveCtx.stroke();
    };

    plotFunction(functions.sin, "#f36b3d");
    plotFunction(functions.cos, "#18a7b5");

    const markerX = xToPx(settings.currentX);
    const sinY = yToPx(functions.sin(settings.currentX));
    const cosY = yToPx(functions.cos(settings.currentX));

    waveCtx.setLineDash([6, 8]);
    waveCtx.strokeStyle = "rgba(22, 48, 71, 0.24)";
    waveCtx.lineWidth = 1.5;
    waveCtx.beginPath();
    waveCtx.moveTo(markerX, padding.top);
    waveCtx.lineTo(markerX, height - padding.bottom);
    waveCtx.stroke();
    waveCtx.setLineDash([]);

    waveCtx.fillStyle = "#f36b3d";
    waveCtx.beginPath();
    waveCtx.arc(markerX, sinY, 6, 0, Math.PI * 2);
    waveCtx.fill();

    waveCtx.fillStyle = "#18a7b5";
    waveCtx.beginPath();
    waveCtx.arc(markerX, cosY, 6, 0, Math.PI * 2);
    waveCtx.fill();
}

function tick() {
    if (waveControls.animateToggle.checked) {
        let nextValue = Number(waveControls.markerRange.value) + 0.035;
        if (nextValue > graphDomain.max) {
            nextValue = graphDomain.min;
        }
        waveControls.markerRange.value = formatNumber(nextValue, 3);
        drawGraph();
    }
    animationFrame = window.requestAnimationFrame(tick);
}

function bindWaveControls() {
    [
        waveControls.amplitudeRange,
        waveControls.frequencyRange,
        waveControls.phaseRange,
        waveControls.offsetRange,
        waveControls.markerRange,
        waveControls.animateToggle,
    ].forEach((element) => {
        element.addEventListener("input", drawGraph);
        element.addEventListener("change", drawGraph);
    });

    waveControls.closeWindowButton.addEventListener("click", () => {
        window.close();
    });

    drawGraph();
    animationFrame = window.requestAnimationFrame(tick);
}

window.addEventListener("beforeunload", () => {
    window.cancelAnimationFrame(animationFrame);
});

bindWaveControls();
