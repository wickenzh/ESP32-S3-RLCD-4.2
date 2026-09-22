import { tr, setText, setAttr, LocalizedError, getLanguage } from './i18n.js';
const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

const MAX_ASSETS_SIZE = 2 * 1024 * 1024;
const MAGIC_WCA1 = 0x31414357;
const TYPE_MAIN_GIF = 1;
const TYPE_GALLERY_IMAGE = 2;
const TYPE_WEATHER_CITY = 3;
const TYPE_OTA_MANIFEST_URL = 4;
const GIF_WIDTH = 84;
const GIF_HEIGHT = 84;
const GIF_FRAMES = 60;
const IMAGE_WIDTH = 220;
const IMAGE_HEIGHT = 208;
const MAX_IMAGES = 24;
const MAX_WEATHER_CITY_BYTES = 31;
const MAX_OTA_MANIFEST_URL_BYTES = 255;
const PARTITION_TABLE_OFFSET = 0x8000;
const PARTITION_TABLE_SIZE = 0x1000;
const FIRMWARE_RELEASES_MANIFEST_URL = "./firmware/releases.json";
const FIRMWARE_RELEASES_SOURCE_URL = "https://github.com/wickenzh/ESP32-S3-RLCD-4.2/releases";
const HOST_WEB_VERSION = "v1.0.2";
const DEFAULT_SUMMARY_NOTE = "资源包支持 GIF、静图和兜底配置。\n写入并重启后，优先加载自定义资源。";
const MERGED_TARGET = {
  value: "merged",
  get label() { return tr("0x0：完整 merged 固件"); },
  kind: "merged",
  partitions: [{ label: "flash", address: 0, size: 0 }],
  remoteImage: "merged"
};

const serialSupport = $("#serialSupport");
const connectSerialBtn = $("#connectSerialBtn");
const baudRate = $("#baudRate");
const serialState = $("#serialState");
const serialDevice = $("#serialDevice");
const serialLog = $("#serialLog");
const clearLogBtn = $("#clearLogBtn");
const saveLogBtn = $("#saveLogBtn");
const sendForm = $("#sendForm");
const serialCommand = $("#serialCommand");
const rxBytes = $("#rxBytes");
const lastLineTime = $("#lastLineTime");
const cacheState = $("#cacheState");
const hostVersion = $("#hostVersion");

if (hostVersion) setText(hostVersion, () => HOST_WEB_VERSION);

let port;
let reader;
let writer;
let keepReading = false;
let receivedBytes = 0;
let convertedGif;
let convertedImages = [];
let generatedAssetPackage;
let selectedFirmware;
let remoteFirmwareManifest;
let remoteFirmwareOptions = [];
let verifiedFirmwareData;
let selectedImagePreviewIndex = 0;
let gifPreviewTimer;
let gifPreviewFrames = [];
let gifOriginalUrl;
let gifPreviewRequest = 0;
let gifFrameCacheFile;
let gifFrameCacheFrames;
let gifRealtimeTimer;
let imageRealtimeTimer;
let assetDevicePort;
let assetPartitionVerified = false;
let assetPartition;
let assetPartitions = [];
let firmwareDevicePort;
let firmwarePartitions = [];
let firmwareAppTargets = [];
let firmwareChipVerified = false;
let firmwareFlashSizeBytes = 0;
let firmwareFlashSizeText = "-";
let firmwareInstallBusy = false;
let firmwareInstallSnapshot;
let nextStepElement;
let nextStepTimer;

function clearNextStepHint() {
  clearTimeout(nextStepTimer);
  nextStepElement?.classList.remove("is-next-step");
  nextStepElement = undefined;
}

function hintNextStep(selector) {
  clearNextStepHint();
  const element = $(selector);
  if (!element || element.disabled || !element.getClientRects().length) return;
  nextStepElement = element;
  element.classList.add("is-next-step");
  nextStepTimer = setTimeout(clearNextStepHint, 2400);
}

function hex(value, width = 0) {
  return `0x${Number(value).toString(16).toUpperCase().padStart(width, "0")}`;
}

function serialId(value) {
  return value === undefined ? "----" : hex(value, 4);
}

function describePort(selectedPort) {
  const info = selectedPort?.getInfo?.() || {};
  if (info.usbVendorId !== undefined || info.usbProductId !== undefined) {
    return `USB ${serialId(info.usbVendorId)}:${serialId(info.usbProductId)}`;
  }
  return tr("已授权串口设备");
}

function resetAssetDeviceState(message = "未核对") {
  assetPartitionVerified = false;
  assetPartition = undefined;
  assetPartitions = [];
  setText($("#assetPartitionState"), () => tr(message));
  updateAssetWriteButtons();
}

async function setGifOriginalPreview(file) {
  const request = ++gifPreviewRequest;
  const img = document.getElementById("gifOriginalPreview");
  if (!(img instanceof HTMLImageElement)) throw new LocalizedError(() => tr("GIF 预览图片元素不可用。"));
  const header = new Uint8Array(await file.slice(0, 6).arrayBuffer());
  if (request !== gifPreviewRequest) return false;
  const signature = String.fromCharCode(...header);
  if (signature !== "GIF87a" && signature !== "GIF89a") {
    img.removeAttribute("src");
    if (gifOriginalUrl) URL.revokeObjectURL(gifOriginalUrl);
    gifOriginalUrl = undefined;
    throw new LocalizedError(() => tr("文件内容不是有效的 GIF，请重新选择 GIF 动图。"));
  }
  // 固定图片 MIME，文件字节不变，避免按上传文件声明的文档类型解释。
  const imageBlob = file.slice(0, file.size, "image/gif");
  const nextUrl = URL.createObjectURL(imageBlob);
  img.src = nextUrl;
  if (gifOriginalUrl) URL.revokeObjectURL(gifOriginalUrl);
  gifOriginalUrl = nextUrl;
  return true;
}

function nowText(date = new Date()) {
  return new Intl.DateTimeFormat(getLanguage(), {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  }).format(date);
}

let serialLogStarted = false;
let writeLogStarted = false;
function appendLog(text) {
  if (!serialLogStarted) { serialLog.textContent = ""; serialLogStarted = true; }
  serialLog.textContent += text;
  serialLog.scrollTop = serialLog.scrollHeight;
  const receivedAt = new Date();
  setText(lastLineTime, () => nowText(receivedAt));
}

function appendWriteLog(text) {
  const log = $("#writeLog");
  if (!writeLogStarted) { log.textContent = ""; writeLogStarted = true; }
  log.textContent += text;
  log.scrollTop = log.scrollHeight;
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}

function setProgress(prefix, written, total) {
  const percent = total ? Math.min(100, Math.round(written / total * 100)) : 0;
  $(`#${prefix}Progress`).value = percent;
  setText($(`#${prefix}Percent`), () => `${percent}%`);
}

function updateAssetWriteButtons() {
  const hasSerial = "serial" in navigator;
  const hasPackage = Boolean(generatedAssetPackage);
  const packageFits = Boolean(generatedAssetPackage && assetPartition && generatedAssetPackage.byteLength <= assetPartition.size);
  $("#writerLockedNotice").hidden = hasPackage;
  $("#assetBaudRate").disabled = !(hasSerial && hasPackage);
  $("#selectAssetDeviceBtn").disabled = !(hasSerial && hasPackage);
  $("#writeAssetsBtn").disabled = !(hasSerial && hasPackage && assetPartitionVerified && packageFits);
  $("#eraseAssetsBtn").disabled = !(hasSerial && hasPackage && assetPartitionVerified);
}

function setSerialSupport() {
  if ("serial" in navigator) {
    setText(serialSupport, () => tr("Web Serial 可用"));
    serialSupport.classList.add("is-ok");
    updateAssetWriteButtons();
    refreshFirmwareTargetState();
    return;
  }
  setText(serialSupport, () => tr("浏览器不支持串口"));
  serialSupport.classList.add("is-warn");
  connectSerialBtn.disabled = true;
  $("#selectAssetDeviceBtn").disabled = true;
  $("#writeAssetsBtn").disabled = true;
  $("#eraseAssetsBtn").disabled = true;
  $("#selectFirmwareDeviceBtn").disabled = true;
  $("#writeFirmwareBtn").disabled = true;
}

async function disconnectSerial() {
  keepReading = false;
  try {
    if (reader) {
      await reader.cancel();
      reader.releaseLock();
    }
  } catch (error) {
    console.warn(error);
  }
  try {
    if (writer) writer.releaseLock();
  } catch (error) {
    console.warn(error);
  }
  try {
    if (port) await port.close();
  } catch (error) {
    console.warn(error);
  }
  reader = undefined;
  writer = undefined;
  port = undefined;
  setText(connectSerialBtn, () => tr("连接串口"));
  setText(serialState, () => tr("未连接"));
  setText(serialDevice, () => tr("未选择"));
  appendLog(tr`\n[${nowText()}] 串口已断开\n`);
}

async function connectSerial() {
  if (port) {
    await disconnectSerial();
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: Number(baudRate.value) });
    writer = port.writable.getWriter();
    keepReading = true;
    setText(connectSerialBtn, () => tr("断开串口"));
    setText(serialState, () => tr("已连接"));
    setText(serialDevice, () => describePort(port));
    appendLog(tr`[${nowText()}] 串口已连接：${describePort(port)}，波特率 ${baudRate.value}\n`);
    readSerialLoop();
  } catch (error) {
    setText(serialState, () => tr("连接失败"));
    appendLog(tr`[${nowText()}] 连接失败：${error.message}\n`);
    port = undefined;
  }
}

async function readSerialLoop() {
  const decoder = new TextDecoder();
  while (port?.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      while (keepReading) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) {
          receivedBytes += value.byteLength;
          setText(rxBytes, () => formatBytes(receivedBytes));
          appendLog(decoder.decode(value, { stream: true }));
        }
      }
    } catch (error) {
      appendLog(tr`[${nowText()}] 读取中断：${error.message}\n`);
    } finally {
      reader.releaseLock();
      reader = undefined;
    }
  }
}

async function sendSerialText(text) {
  if (!writer) {
    appendLog(tr`[${nowText()}] 尚未连接串口，未发送：${text}\n`);
    return false;
  }
  const payload = text.endsWith("\n") ? text : `${text}\n`;
  await writer.write(new TextEncoder().encode(payload));
  appendLog(`[${nowText()}] > ${payload}`);
  return true;
}

function drawFittedImage(ctx, img, fit, width, height) {
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, width, height);
  if (fit === "stretch") {
    ctx.drawImage(img, 0, 0, width, height);
    return;
  }
  const sourceWidth = img.videoWidth || img.naturalWidth || img.width;
  const sourceHeight = img.videoHeight || img.naturalHeight || img.height;
  const scale = fit === "cover" ? Math.max(width / sourceWidth, height / sourceHeight) : Math.min(width / sourceWidth, height / sourceHeight);
  const drawWidth = sourceWidth * scale;
  const drawHeight = sourceHeight * scale;
  ctx.drawImage(img, (width - drawWidth) / 2, (height - drawHeight) / 2, drawWidth, drawHeight);
}

function applyEdgeFade(canvas, fadePixels) {
  const fade = Math.max(0, Math.min(fadePixels, Math.floor(Math.min(canvas.width, canvas.height) / 2)));
  if (fade === 0) return;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
  const { width, height, data } = imageData;
  for (let y = 0; y < height; y += 1) {
    const edgeY = Math.min(y, height - 1 - y);
    for (let x = 0; x < width; x += 1) {
      const edgeX = Math.min(x, width - 1 - x);
      const edgeDistance = Math.min(edgeX, edgeY);
      if (edgeDistance >= fade) continue;
      const t = Math.max(0, edgeDistance / fade);
      const mixWhite = 1 - (t * t * (3 - 2 * t));
      const offset = (y * width + x) * 4;
      data[offset] = data[offset] + (255 - data[offset]) * mixWhite;
      data[offset + 1] = data[offset + 1] + (255 - data[offset + 1]) * mixWhite;
      data[offset + 2] = data[offset + 2] + (255 - data[offset + 2]) * mixWhite;
      data[offset + 3] = 255;
    }
  }
  ctx.putImageData(imageData, 0, 0);
}

function convertCanvasToOneBit(sourceCanvas, previewCanvas, threshold, dither, invert) {
  const sourceCtx = sourceCanvas.getContext("2d", { willReadFrequently: true });
  const previewCtx = previewCanvas.getContext("2d", { willReadFrequently: true });
  const source = sourceCtx.getImageData(0, 0, sourceCanvas.width, sourceCanvas.height);
  const output = previewCtx.createImageData(source.width, source.height);
  const gray = new Float32Array(source.width * source.height);

  for (let i = 0; i < gray.length; i += 1) {
    const offset = i * 4;
    gray[i] = source.data[offset] * 0.299 + source.data[offset + 1] * 0.587 + source.data[offset + 2] * 0.114;
  }

  for (let y = 0; y < source.height; y += 1) {
    for (let x = 0; x < source.width; x += 1) {
      const index = y * source.width + x;
      const oldValue = gray[index];
      const biasedValue = dither ? Math.max(0, Math.min(255, oldValue - (threshold - 128))) : oldValue;
      const monoValue = biasedValue >= (dither ? 128 : threshold) ? 255 : 0;
      let nextValue = monoValue;
      if (invert) nextValue = 255 - nextValue;
      const error = biasedValue - monoValue;

      if (dither) {
        if (x + 1 < source.width) gray[index + 1] += error * 7 / 16;
        if (y + 1 < source.height) {
          if (x > 0) gray[index + source.width - 1] += error * 3 / 16;
          gray[index + source.width] += error * 5 / 16;
          if (x + 1 < source.width) gray[index + source.width + 1] += error * 1 / 16;
        }
      }

      const offset = index * 4;
      output.data[offset] = nextValue;
      output.data[offset + 1] = nextValue;
      output.data[offset + 2] = nextValue;
      output.data[offset + 3] = 255;
    }
  }

  previewCtx.putImageData(output, 0, 0);
  return packOneBit(output);
}

function stopGifPreview() {
  if (gifPreviewTimer) {
    clearInterval(gifPreviewTimer);
    gifPreviewTimer = undefined;
  }
}

function startGifPreview(frames) {
  stopGifPreview();
  gifPreviewFrames = frames;
  if (frames.length === 0) return;
  const ctx = $("#gifPreviewCanvas").getContext("2d");
  let index = 0;
  ctx.putImageData(frames[0], 0, 0);
  gifPreviewTimer = setInterval(() => {
    index = (index + 1) % frames.length;
    ctx.putImageData(frames[index], 0, 0);
  }, 120);
}

function packOneBit(imageData) {
  const rowBytes = Math.ceil(imageData.width / 8);
  const packed = new Uint8Array(rowBytes * imageData.height);
  for (let y = 0; y < imageData.height; y += 1) {
    for (let x = 0; x < imageData.width; x += 1) {
      const pixelIndex = (y * imageData.width + x) * 4;
      if (imageData.data[pixelIndex] < 128) packed[y * rowBytes + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }
  return packed;
}

function packOneBitContinuous(imageData) {
  const bitCount = imageData.width * imageData.height;
  const packed = new Uint8Array(Math.ceil(bitCount / 8));
  for (let i = 0; i < bitCount; i += 1) {
    const pixelIndex = i * 4;
    if (imageData.data[pixelIndex] < 128) packed[i >> 3] |= 0x80 >> (i & 7);
  }
  return packed;
}

function unpackOneBitContinuousToImageData(packed, width, height, ctx) {
  const imageData = ctx.createImageData(width, height);
  for (let i = 0; i < width * height; i += 1) {
    const isBlack = (packed[i >> 3] & (0x80 >> (i & 7))) !== 0;
    const value = isBlack ? 0 : 255;
    const offset = i * 4;
    imageData.data[offset] = value;
    imageData.data[offset + 1] = value;
    imageData.data[offset + 2] = value;
    imageData.data[offset + 3] = 255;
  }
  return imageData;
}

function unpackOneBitRowsToImageData(packed, width, height, ctx) {
  const imageData = ctx.createImageData(width, height);
  const rowBytes = Math.ceil(width / 8);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const isBlack = (packed[y * rowBytes + (x >> 3)] & (0x80 >> (x & 7))) !== 0;
      const value = isBlack ? 0 : 255;
      const offset = (y * width + x) * 4;
      imageData.data[offset] = value;
      imageData.data[offset + 1] = value;
      imageData.data[offset + 2] = value;
      imageData.data[offset + 3] = 255;
    }
  }
  return imageData;
}

function countPackedBlackBits(packed) {
  let count = 0;
  for (const byte of packed) {
    let value = byte;
    while (value) {
      value &= value - 1;
      count += 1;
    }
  }
  return count;
}

async function loadImageBitmapFromFile(file) {
  if ("createImageBitmap" in window) return createImageBitmap(file);
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      resolve(img);
    };
    img.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new LocalizedError(() => tr("图片读取失败")));
    };
    img.src = url;
  });
}

async function decodeGifFrames(file) {
  const bytes = await file.arrayBuffer();
  try {
    return await decodeGifFramesLocally(bytes);
  } catch (error) {
    console.warn(error);
  }
  if ("ImageDecoder" in window) {
    try {
      const decoder = new ImageDecoder({ data: bytes, type: "image/gif" });
      await decoder.tracks.ready;
      const frameCount = decoder.tracks.selectedTrack?.frameCount || GIF_FRAMES;
      const frames = [];
      const sampleIndices = sampleEvenlyByIndex(frameCount, GIF_FRAMES);
      for (const index of sampleIndices) {
        const decoded = await decoder.decode({ frameIndex: index });
        frames.push(decoded.image);
      }
      return frames;
    } catch (error) {
      console.warn(error);
    }
  }
  const fallback = await loadImageBitmapFromFile(file);
  return Array.from({ length: GIF_FRAMES }, () => fallback);
}

async function getGifFrames(file) {
  if (gifFrameCacheFile === file && gifFrameCacheFrames) return gifFrameCacheFrames;
  gifFrameCacheFile = file;
  gifFrameCacheFrames = await decodeGifFrames(file);
  return gifFrameCacheFrames;
}

function sampleEvenlyByIndex(sourceCount, targetCount) {
  if (sourceCount <= 1) return Array.from({ length: targetCount }, () => 0);
  return Array.from({ length: targetCount }, (_item, index) => Math.min(sourceCount - 1, Math.floor(index * sourceCount / targetCount)));
}

function sampleGifTimeline(frames, targetCount) {
  if (frames.length === 0) return [];
  if (frames.length === 1) return Array.from({ length: targetCount }, () => frames[0].image);
  const durations = frames.map((frame) => Math.max(20, frame.durationMs || 100));
  const totalDuration = durations.reduce((sum, duration) => sum + duration, 0);
  const sampled = [];
  let frameIndex = 0;
  let frameEnd = durations[0];

  for (let i = 0; i < targetCount; i += 1) {
    const time = i * totalDuration / targetCount;
    while (frameIndex < frames.length - 1 && time >= frameEnd) {
      frameIndex += 1;
      frameEnd += durations[frameIndex];
    }
    sampled.push(frames[frameIndex].image);
  }

  return sampled;
}

class GifByteReader {
  constructor(buffer) {
    this.data = new Uint8Array(buffer);
    this.offset = 0;
  }

  readByte() {
    if (this.offset >= this.data.length) throw new LocalizedError(() => tr("GIF 文件不完整"));
    return this.data[this.offset++];
  }

  readUnsigned() {
    const low = this.readByte();
    const high = this.readByte();
    return low | (high << 8);
  }

  readBytes(length) {
    if (this.offset + length > this.data.length) throw new LocalizedError(() => tr("GIF 文件不完整"));
    const value = this.data.slice(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }

  readString(length) {
    return String.fromCharCode(...this.readBytes(length));
  }

  readSubBlocks() {
    const chunks = [];
    let total = 0;
    while (true) {
      const size = this.readByte();
      if (size === 0) break;
      const chunk = this.readBytes(size);
      chunks.push(chunk);
      total += chunk.length;
    }
    const output = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      output.set(chunk, offset);
      offset += chunk.length;
    }
    return output;
  }

  skipSubBlocks() {
    while (true) {
      const size = this.readByte();
      if (size === 0) break;
      this.offset += size;
      if (this.offset > this.data.length) throw new LocalizedError(() => tr("GIF 文件不完整"));
    }
  }
}

function readGifColorTable(reader, size) {
  const table = [];
  for (let i = 0; i < size; i += 1) {
    table.push([reader.readByte(), reader.readByte(), reader.readByte()]);
  }
  return table;
}

function decodeGifLzw(minCodeSize, data, expectedLength) {
  const clearCode = 1 << minCodeSize;
  const endCode = clearCode + 1;
  let codeSize = minCodeSize + 1;
  let bitPos = 0;
  let previous;
  const output = [];
  let dictionary = [];

  const resetDictionary = () => {
    dictionary = [];
    for (let i = 0; i < clearCode; i += 1) dictionary[i] = [i];
    dictionary[clearCode] = [];
    dictionary[endCode] = null;
    codeSize = minCodeSize + 1;
    previous = undefined;
  };

  const readCode = () => {
    let code = 0;
    for (let i = 0; i < codeSize; i += 1) {
      const byte = data[bitPos >> 3];
      if (byte & (1 << (bitPos & 7))) code |= 1 << i;
      bitPos += 1;
    }
    return code;
  };

  resetDictionary();
  while (bitPos < data.length * 8 && output.length < expectedLength) {
    const code = readCode();
    if (code === clearCode) {
      resetDictionary();
      continue;
    }
    if (code === endCode) break;

    let entry;
    if (dictionary[code]) {
      entry = dictionary[code].slice();
    } else if (previous) {
      entry = previous.concat(previous[0]);
    } else {
      throw new LocalizedError(() => tr("GIF LZW 数据无效"));
    }

    output.push(...entry);
    if (previous) {
      dictionary.push(previous.concat(entry[0]));
      if (dictionary.length === (1 << codeSize) && codeSize < 12) codeSize += 1;
    }
    previous = entry;
  }

  return output.slice(0, expectedLength);
}

function deinterlaceGifPixels(pixels, width, height) {
  const output = new Uint8Array(width * height);
  let offset = 0;
  const passes = [
    [0, 8],
    [4, 8],
    [2, 4],
    [1, 2]
  ];
  for (const [start, step] of passes) {
    for (let y = start; y < height; y += step) {
      output.set(pixels.slice(offset, offset + width), y * width);
      offset += width;
    }
  }
  return output;
}

async function decodeGifFramesLocally(bytes) {
  const reader = new GifByteReader(bytes);
  const signature = reader.readString(6);
  if (signature !== "GIF87a" && signature !== "GIF89a") throw new LocalizedError(() => tr("不是有效的 GIF 文件"));

  const logicalWidth = reader.readUnsigned();
  const logicalHeight = reader.readUnsigned();
  const packed = reader.readByte();
  const hasGlobalColorTable = (packed & 0x80) !== 0;
  const globalColorTableSize = 1 << ((packed & 0x07) + 1);
  reader.readByte();
  reader.readByte();
  const globalColorTable = hasGlobalColorTable ? readGifColorTable(reader, globalColorTableSize) : [];

  const compose = document.createElement("canvas");
  compose.width = logicalWidth;
  compose.height = logicalHeight;
  const composeCtx = compose.getContext("2d", { willReadFrequently: true });
  const frames = [];
  let gce = { disposal: 0, durationMs: 100, transparentIndex: undefined };

  while (reader.offset < reader.data.length) {
    const introducer = reader.readByte();
    if (introducer === 0x3b) break;

    if (introducer === 0x21) {
      const label = reader.readByte();
      if (label === 0xf9) {
        const blockSize = reader.readByte();
        const block = reader.readBytes(blockSize);
        reader.readByte();
        const delay = block[1] | (block[2] << 8);
        gce = {
          disposal: (block[0] >> 2) & 0x07,
          durationMs: delay > 0 ? delay * 10 : 100,
          transparentIndex: (block[0] & 0x01) ? block[3] : undefined
        };
      } else {
        reader.skipSubBlocks();
      }
      continue;
    }

    if (introducer !== 0x2c) throw new LocalizedError(() => tr("GIF 块格式无效"));

    const left = reader.readUnsigned();
    const top = reader.readUnsigned();
    const width = reader.readUnsigned();
    const height = reader.readUnsigned();
    const imagePacked = reader.readByte();
    const hasLocalColorTable = (imagePacked & 0x80) !== 0;
    const interlaced = (imagePacked & 0x40) !== 0;
    const localColorTableSize = 1 << ((imagePacked & 0x07) + 1);
    const colorTable = hasLocalColorTable ? readGifColorTable(reader, localColorTableSize) : globalColorTable;
    const minCodeSize = reader.readByte();
    const imageBytes = reader.readSubBlocks();
    let indices = decodeGifLzw(minCodeSize, imageBytes, width * height);
    if (interlaced) indices = deinterlaceGifPixels(indices, width, height);

    const beforeFrame = composeCtx.getImageData(0, 0, logicalWidth, logicalHeight);
    const imageData = composeCtx.getImageData(left, top, width, height);
    for (let i = 0; i < indices.length; i += 1) {
      const colorIndex = indices[i];
      if (colorIndex === gce.transparentIndex) continue;
      const color = colorTable[colorIndex] || [255, 255, 255];
      const offset = i * 4;
      imageData.data[offset] = color[0];
      imageData.data[offset + 1] = color[1];
      imageData.data[offset + 2] = color[2];
      imageData.data[offset + 3] = 255;
    }
    composeCtx.putImageData(imageData, left, top);
    frames.push({ image: await createImageBitmap(compose), durationMs: gce.durationMs });

    if (gce.disposal === 2) {
      composeCtx.clearRect(left, top, width, height);
    } else if (gce.disposal === 3) {
      composeCtx.putImageData(beforeFrame, 0, 0);
    }
    gce = { disposal: 0, durationMs: 100, transparentIndex: undefined };
  }

  if (!frames.length) throw new LocalizedError(() => tr("GIF 没有可解码帧"));
  return sampleGifTimeline(frames, GIF_FRAMES);
}

async function convertGif({ realtime = false } = {}) {
  const file = $("#gifInput").files?.[0];
  if (!file) {
    setText($("#assetResult"), () => tr("请先选择一个 GIF 文件。"));
    return;
  }
  if (file.type !== "image/gif" && !file.name.toLowerCase().endsWith(".gif")) {
    setText($("#assetResult"), () => tr("动图区域只支持 GIF 文件。"));
    return;
  }

  if (!await setGifOriginalPreview(file)) return;
  setText($("#assetResult"), () => realtime ? tr("正在实时更新 GIF 预览...") : tr("正在解析并转换 GIF..."));
  const frames = await getGifFrames(file);
  const source = $("#gifSourceCanvas");
  const preview = $("#gifPreviewCanvas");
  const sourceCtx = source.getContext("2d", { willReadFrequently: true });
  const previewCtx = preview.getContext("2d", { willReadFrequently: true });
  const fit = $("#gifFit").value;
  const threshold = Number($("#gifThreshold").value);
  const dither = $("#gifDither").checked;
  const invert = $("#gifInvert").checked;
  const packedFrames = [];
  const previewFrames = [];
  let blackBits = 0;

  for (let i = 0; i < GIF_FRAMES; i += 1) {
    drawFittedImage(sourceCtx, frames[i], fit, GIF_WIDTH, GIF_HEIGHT);
    convertCanvasToOneBit(source, preview, threshold, dither, invert);
    const convertedFrame = previewCtx.getImageData(0, 0, GIF_WIDTH, GIF_HEIGHT);
    const packed = packOneBitContinuous(convertedFrame);
    packedFrames.push(packed);
    previewFrames.push(unpackOneBitContinuousToImageData(packed, GIF_WIDTH, GIF_HEIGHT, previewCtx));
    blackBits += countPackedBlackBits(packed);
  }

  const frameBytes = GIF_WIDTH * GIF_HEIGHT / 8;
  const payload = new Uint8Array(frameBytes * GIF_FRAMES);
  packedFrames.forEach((frame, index) => payload.set(frame, index * frameBytes));
  convertedGif = { type: TYPE_MAIN_GIF, index: 0, width: GIF_WIDTH, height: GIF_HEIGHT, frameCount: GIF_FRAMES, bytesPerRow: 0, data: payload };
  startGifPreview(previewFrames);
  invalidateGeneratedAssets();
  const density = Math.round(blackBits / (GIF_WIDTH * GIF_HEIGHT * GIF_FRAMES) * 100);
  const warning = () => blackBits === 0 ? tr("当前转换结果没有黑色像素，请尝试调高阈值或开启反色。") : tr("右侧预览正在循环播放转换后的效果。");
  setText($("#assetResult"), () => tr`GIF 已转换：${GIF_WIDTH}×${GIF_HEIGHT}，按完整播放区间均匀抽取 ${GIF_FRAMES} 帧，整帧连续 bitstream，${formatBytes(payload.byteLength)}，黑色像素约 ${density}%。${warning()}`);
  if (!realtime) hintNextStep("#buildAssetsBtn");
}

async function previewSelectedGif() {
  stopGifPreview();
  convertedGif = undefined;
  invalidateGeneratedAssets();
  const file = $("#gifInput").files?.[0];
  if (!file) return;
  gifFrameCacheFile = undefined;
  gifFrameCacheFrames = undefined;
  if (file.type !== "image/gif" && !file.name.toLowerCase().endsWith(".gif")) {
    setText($("#assetResult"), () => tr("动图区域只支持 GIF 文件。"));
    return;
  }
  if (!await setGifOriginalPreview(file)) return;
  const frames = await getGifFrames(file);
  const source = $("#gifSourceCanvas");
  const preview = $("#gifPreviewCanvas");
  const sourceCtx = source.getContext("2d", { willReadFrequently: true });
  const previewCtx = preview.getContext("2d", { willReadFrequently: true });
  drawFittedImage(sourceCtx, frames[0], $("#gifFit").value, GIF_WIDTH, GIF_HEIGHT);
  previewCtx.clearRect(0, 0, GIF_WIDTH, GIF_HEIGHT);
  setText($("#assetResult"), () => tr`已载入 GIF：${file.name}。点击“转换 GIF”查看 1-bit 动图预览。`);
  hintNextStep("#previewGifBtn");
}

async function convertImages() {
  const files = getSelectedImageFiles();
  if (files.length === 0) {
    setText($("#assetResult"), () => tr("请先选择静图文件。"));
    return;
  }
  setText($("#assetResult"), () => tr("正在转换静图..."));
  convertedImages = [];
  const source = $("#imageSourceCanvas");
  const preview = $("#imagePreviewCanvas");
  const sourceCtx = source.getContext("2d", { willReadFrequently: true });
  const fit = $("#imageFit").value;
  const threshold = Number($("#imageThreshold").value);
  const edgeFade = Number($("#imageEdgeFade").value);
  const dither = $("#imageDither").checked;
  const invert = $("#imageInvert").checked;

  for (const [index, file] of files.entries()) {
    const image = await loadImageBitmapFromFile(file);
    drawFittedImage(sourceCtx, image, fit, IMAGE_WIDTH, IMAGE_HEIGHT);
    applyEdgeFade(source, edgeFade);
    const packed = convertCanvasToOneBit(source, preview, threshold, dither, invert);
    convertedImages.push({ type: TYPE_GALLERY_IMAGE, index, width: IMAGE_WIDTH, height: IMAGE_HEIGHT, frameCount: 1, bytesPerRow: Math.ceil(IMAGE_WIDTH / 8), data: packed, name: file.name });
  }

  updateImageList(files);
  await previewSelectedImages({ keepConverted: true });
  invalidateGeneratedAssets();
  setText($("#assetResult"), () => tr`静图已转换：${convertedImages.length} 张，每张 ${IMAGE_WIDTH}×${IMAGE_HEIGHT}。预览显示第 ${selectedImagePreviewIndex + 1} 张。`);
  hintNextStep("#buildAssetsBtn");
}

function getSelectedImageFiles() {
  return Array.from($("#imageInput").files || []).slice(0, MAX_IMAGES);
}

function updateSummaryNoteForImages(files = getSelectedImageFiles()) {
  const note = $("#summaryNote");
  if (!note) return;
  if (files.length === 0) {
    setText(note, () => tr(DEFAULT_SUMMARY_NOTE));
    return;
  }
  setText(note, () => tr`已选择 ${files.length} 张静图，最多会转换前 ${MAX_IMAGES} 张。当前预览第 ${selectedImagePreviewIndex + 1} 张，点击“转换静图”查看 1-bit 预览。`);
}

function updateImagePreviewSelect(files) {
  const select = $("#imagePreviewSelect");
  select.innerHTML = "";
  if (files.length === 0) {
    select.disabled = true;
    const option = document.createElement("option");
    option.value = "";
    setText(option, () => tr("尚未选择静图"));
    select.appendChild(option);
    return;
  }
  select.disabled = false;
  files.forEach((file, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    setText(option, () => `${index + 1}. ${file.name}`);
    select.appendChild(option);
  });
  selectedImagePreviewIndex = Math.min(selectedImagePreviewIndex, files.length - 1);
  select.value = String(selectedImagePreviewIndex);
}

function updateImageList(files) {
  if (files.length === 0) {
    setText($("#imageList"), () => tr("尚未选择静图。"));
    updateSummaryNoteForImages(files);
    return;
  }
  const converted = new Set(convertedImages.map((item) => item.index));
  setText($("#imageList"), () => files.map((file, index) => {
    const suffix = converted.has(index) ? tr("已转换") : tr("待转换");
    return `${index + 1}. ${file.name} / ${suffix}`;
  }).join("\n"));
}

function invalidateGeneratedAssets() {
  clearNextStepHint();
  generatedAssetPackage = undefined;
  $("#assetReadyNotice").hidden = true;
  $("#downloadAssetsBtn").disabled = true;
  setText($("#assetWriteState"), () => tr("等待资源包"));
  updateAssetWriteButtons();
}

function clearGifConversion() {
  stopGifPreview();
  convertedGif = undefined;
  invalidateGeneratedAssets();
  const preview = $("#gifPreviewCanvas");
  preview.getContext("2d", { willReadFrequently: true }).clearRect(0, 0, GIF_WIDTH, GIF_HEIGHT);
  setText($("#assetResult"), () => tr("已清除 GIF 转换结果。已选择的 GIF 文件仍保留，可重新转换。"));
}

function clearImageConversions() {
  convertedImages = [];
  invalidateGeneratedAssets();
  const preview = $("#imagePreviewCanvas");
  preview.getContext("2d", { willReadFrequently: true }).clearRect(0, 0, IMAGE_WIDTH, IMAGE_HEIGHT);
  const files = getSelectedImageFiles();
  updateImageList(files);
  updateSummaryNoteForImages(files);
  setText($("#assetResult"), () => tr("已清除静图转换结果。已选择的静图文件仍保留，可重新转换。"));
}

async function updateSelectedImageRealtimePreview({ clearConverted = true, message = true } = {}) {
  const files = getSelectedImageFiles();
  if (files.length === 0) {
    updateImagePreviewSelect(files);
    setText($("#imageList"), () => tr("尚未选择静图。"));
    return;
  }
  if (clearConverted) {
    convertedImages = [];
    invalidateGeneratedAssets();
  }
  updateImagePreviewSelect(files);
  const image = await loadImageBitmapFromFile(files[selectedImagePreviewIndex]);
  const source = $("#imageSourceCanvas");
  const preview = $("#imagePreviewCanvas");
  const sourceCtx = source.getContext("2d", { willReadFrequently: true });
  drawFittedImage(sourceCtx, image, $("#imageFit").value, IMAGE_WIDTH, IMAGE_HEIGHT);
  applyEdgeFade(source, Number($("#imageEdgeFade").value));
  convertCanvasToOneBit(
    source,
    preview,
    Number($("#imageThreshold").value),
    $("#imageDither").checked,
    $("#imageInvert").checked
  );
  updateImageList(files);
  updateSummaryNoteForImages(files);
  if (message) {
    setText($("#assetResult"), () => tr`已按当前参数实时预览第 ${selectedImagePreviewIndex + 1} 张。若要写入设备，请重新点击“转换静图”生成全部静图资源。`);
  }
}

async function previewSelectedImages({ keepConverted = false } = {}) {
  if (!keepConverted) {
    convertedImages = [];
    invalidateGeneratedAssets();
  }
  const files = getSelectedImageFiles();
  if (files.length === 0) {
    updateImagePreviewSelect(files);
    setText($("#imageList"), () => tr("尚未选择静图。"));
    updateSummaryNoteForImages(files);
    return;
  }
  updateImagePreviewSelect(files);
  const image = await loadImageBitmapFromFile(files[selectedImagePreviewIndex]);
  const source = $("#imageSourceCanvas");
  const preview = $("#imagePreviewCanvas");
  const sourceCtx = source.getContext("2d", { willReadFrequently: true });
  const previewCtx = preview.getContext("2d", { willReadFrequently: true });
  drawFittedImage(sourceCtx, image, $("#imageFit").value, IMAGE_WIDTH, IMAGE_HEIGHT);
  applyEdgeFade(source, Number($("#imageEdgeFade").value));
  const converted = convertedImages.find((item) => item.index === selectedImagePreviewIndex);
  if (converted) {
    previewCtx.putImageData(unpackOneBitRowsToImageData(converted.data, IMAGE_WIDTH, IMAGE_HEIGHT, previewCtx), 0, 0);
  } else {
    convertCanvasToOneBit(
      source,
      preview,
      Number($("#imageThreshold").value),
      $("#imageDither").checked,
      $("#imageInvert").checked
    );
  }
  updateImageList(files);
  updateSummaryNoteForImages(files);
  if (!keepConverted) {
    setText($("#assetResult"), () => tr("已载入静图，点击“转换静图”查看 1-bit 预览。"));
    hintNextStep("#previewImagesBtn");
  }
}

function makeCrc32Table() {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let c = i;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[i] = c >>> 0;
  }
  return table;
}

const crcTable = makeCrc32Table();

function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i += 1) crc = crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8);
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function utf8Bytes(text) {
  return new TextEncoder().encode(text);
}

function normalizeWeatherCityInput() {
  const value = ($("#customWeatherCity")?.value || "").trim();
  if (!value) return "";
  if (/[&=?#%/\\<>"'`]/.test(value) || /[\u0000-\u001F\u007F]/.test(value)) {
    throw new LocalizedError(() => tr("天气城市包含不支持的字符，请直接填写城市名，例如：杭州。"));
  }
  const bytes = utf8Bytes(value);
  if (bytes.byteLength > MAX_WEATHER_CITY_BYTES) {
    throw new LocalizedError(() => tr`天气城市过长，UTF-8 编码后不能超过 ${MAX_WEATHER_CITY_BYTES} 字节。`);
  }
  return value;
}

function normalizeOtaManifestInput() {
  const value = ($("#customOtaServer")?.value || "").trim();
  if (!value) return "";
  let url;
  try {
    url = new URL(value);
  } catch (_error) {
    throw new LocalizedError(() => tr("自定义 OTA 服务器必须是 http 或 https 地址。"));
  }
  if (url.protocol !== "https:" && url.protocol !== "http:") {
    throw new LocalizedError(() => tr("自定义 OTA 服务器只支持 http 或 https。"));
  }
  if (!url.pathname.endsWith(".json")) {
    url.pathname = `${url.pathname.replace(/\/+$/, "")}/firmware/latest.json`;
    url.search = "";
    url.hash = "";
  }
  const normalized = url.toString();
  const bytes = utf8Bytes(normalized);
  if (bytes.byteLength > MAX_OTA_MANIFEST_URL_BYTES) {
    throw new LocalizedError(() => tr`自定义 OTA 地址过长，不能超过 ${MAX_OTA_MANIFEST_URL_BYTES} 字节。`);
  }
  return normalized;
}

function makeTextAssetEntry(type, text) {
  return {
    type,
    index: 0,
    width: 0,
    height: 0,
    frameCount: 0,
    bytesPerRow: 0,
    data: utf8Bytes(text)
  };
}

function collectCustomConfigEntries() {
  const entries = [];
  const city = normalizeWeatherCityInput();
  const otaManifestUrl = normalizeOtaManifestInput();
  if (city) entries.push(makeTextAssetEntry(TYPE_WEATHER_CITY, city));
  if (otaManifestUrl) entries.push(makeTextAssetEntry(TYPE_OTA_MANIFEST_URL, otaManifestUrl));
  return entries;
}

function buildAssetPackage() {
  $("#assetReadyNotice").hidden = true;
  let configEntries;
  try {
    configEntries = collectCustomConfigEntries();
  } catch (error) {
    setText($("#assetResult"), () => error.message);
    return;
  }
  const entries = [];
  if (convertedGif) entries.push(convertedGif);
  entries.push(...convertedImages);
  entries.push(...configEntries);
  if (entries.length === 0) {
    setText($("#assetResult"), () => tr("请先转换 GIF、静图，或填写至少一项兜底配置。"));
    return;
  }

  const headerSize = 24 + entries.length * 24;
  const payloadSize = entries.reduce((sum, entry) => sum + entry.data.byteLength, 0);
  const totalSize = headerSize + payloadSize;
  if (totalSize > MAX_ASSETS_SIZE) {
    setText($("#assetResult"), () => tr`资源包超过 WCA1 资源包上限：${formatBytes(totalSize)} / ${formatBytes(MAX_ASSETS_SIZE)}。`);
    return;
  }

  const buffer = new ArrayBuffer(totalSize);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  let offset = headerSize;
  view.setUint32(0, MAGIC_WCA1, true);
  view.setUint16(4, 1, true);
  view.setUint16(6, headerSize, true);
  view.setUint16(8, entries.length, true);
  view.setUint16(10, 0, true);
  view.setUint32(12, totalSize, true);
  view.setUint32(16, 0, true);
  view.setUint32(20, 0, true);

  entries.forEach((entry, entryIndex) => {
    const base = 24 + entryIndex * 24;
    view.setUint16(base, entry.type, true);
    view.setUint16(base + 2, entry.index, true);
    view.setUint16(base + 4, entry.width, true);
    view.setUint16(base + 6, entry.height, true);
    view.setUint16(base + 8, entry.frameCount, true);
    view.setUint16(base + 10, entry.bytesPerRow, true);
    view.setUint32(base + 12, offset, true);
    view.setUint32(base + 16, entry.data.byteLength, true);
    view.setUint32(base + 20, crc32(entry.data), true);
    bytes.set(entry.data, offset);
    offset += entry.data.byteLength;
  });

  const payloadCrc = crc32(bytes.slice(headerSize));
  view.setUint32(20, payloadCrc, true);
  view.setUint32(16, 0, true);
  const headerCrc = crc32(bytes.slice(0, headerSize));
  view.setUint32(16, headerCrc, true);

  generatedAssetPackage = new Uint8Array(buffer);
  if (assetPartitions.length > 0) {
    assetPartition = renderPartitionTable(assetPartitions, {
      tbodyId: "partitionTableBody",
      mode: "assets",
      fileSize: generatedAssetPackage.byteLength
    });
    assetPartitionVerified = Boolean(assetPartition && isAssetsPartition(assetPartition) && generatedAssetPackage.byteLength <= assetPartition.size);
    setText($("#assetPartitionState"), () => assetPartitionVerified ? tr("通过") : tr("未通过"));
  }
  $("#downloadAssetsBtn").disabled = false;
  updateAssetWriteButtons();
  setText($("#assetResult"), () => tr`资源包已生成：${entries.length} 个资源，${formatBytes(totalSize)}。已准备好进行资源写入；也可在此下载 BIN 留存。`);
  setText($("#assetReadyMessage"), () => tr`资源包已生成（${entries.length} 项，${formatBytes(totalSize)}），尚未写入设备。可继续处理图片；全部准备好后，点击“前往资源写入”，核对设备后完成写入。`);
  $("#assetReadyNotice").hidden = false;
  setText($("#assetWriteState"), () => tr`资源包已就绪：${formatBytes(totalSize)}。下一步：① 选择并核对设备，再点击② 串口写入资源。`);
  hintNextStep("#goToWriterBtn");
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

function downloadAssets() {
  if (!generatedAssetPackage) return;
  downloadBlob(new Blob([generatedAssetPackage], { type: "application/octet-stream" }), "custom_assets.bin");
}

function downloadLog() {
  downloadBlob(new Blob([serialLog.textContent], { type: "text/plain;charset=utf-8" }), `weather-clock-serial-${new Date().toISOString().replace(/[:.]/g, "-")}.log`);
}

function hexFromBuffer(buffer) {
  return Array.from(new Uint8Array(buffer), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return hexFromBuffer(digest);
}

function setFirmwareReady(ready) {
  if (!ready) {
    $("#writeFirmwareBtn").disabled = true;
    return;
  }
  updateFirmwareWriteButton();
}

function formatSha(value) {
  if (!value) return "-";
  return `${value.slice(0, 12)}...${value.slice(-8)}`;
}

function truncateMiddle(text, maxLength = 34) {
  const value = String(text || "");
  if (value.length <= maxLength) return value;
  const keepStart = Math.max(8, Math.ceil((maxLength - 3) * 0.58));
  const keepEnd = Math.max(6, maxLength - 3 - keepStart);
  return `${value.slice(0, keepStart)}...${value.slice(-keepEnd)}`;
}

function isValidSha256(value) {
  return /^[a-fA-F0-9]{64}$/.test(String(value || ""));
}

function normalizeFirmwareMirrorAsset(asset, version, kind) {
  if (!asset || typeof asset !== "object") return undefined;
  const name = String(asset.name || asset.assetName || "").trim();
  const url = String(asset.url || "").trim();
  const sha256 = String(asset.sha256 || "").trim().toLowerCase();
  const size = Number(asset.size);
  if (!name || !url || !isValidSha256(sha256) || !Number.isFinite(size) || size <= 0) return undefined;
  const resolvedUrl = new URL(url, window.location.href);
  if (resolvedUrl.origin !== window.location.origin || !resolvedUrl.pathname.includes("/firmware/releases/")) return undefined;
  return {
    kind,
    url: resolvedUrl.href,
    sha256,
    size,
    assetName: name || `weather_clock_${version}_${kind}.bin`
  };
}

function fallbackFirmwareReleaseUrl(version) {
  return `${FIRMWARE_RELEASES_SOURCE_URL}/tag/${encodeURIComponent(String(version || "").trim())}`;
}

function normalizeFirmwareReleaseUrl(value, version) {
  const fallback = fallbackFirmwareReleaseUrl(version);
  try {
    const resolved = new URL(String(value || ""), window.location.href);
    const expectedPath = "/wickenzh/ESP32-S3-RLCD-4.2/releases";
    if (resolved.origin === "https://github.com" && (resolved.pathname === expectedPath || resolved.pathname.startsWith(`${expectedPath}/`))) {
      return resolved.href;
    }
  } catch {}
  return fallback;
}

function firmwareNoteLines(notes, version) {
  const versionText = String(version || "").trim();
  const introPattern = /^ESP32-S3 RLCD 4\.2.*(?:源码发布|source release)/i;
  return String(notes || "")
    .replace(/\r/g, "")
    .split("\n")
    .filter((line) => {
      const trimmed = line.trim();
      if (!trimmed || introPattern.test(trimmed)) return !introPattern.test(trimmed);
      return trimmed !== versionText && trimmed.replace(/^#{1,6}\s*/, "") !== versionText;
    });
}

function formatFirmwareNotes(notes, version) {
  const formatted = firmwareNoteLines(notes, version).join("\n").trim();
  return formatted || tr("暂无详细发布说明");
}

function summarizeFirmwareNotes(notes, version, maxLength = 160) {
  const lines = firmwareNoteLines(notes, version)
    .map((line) => line.trim())
    .filter((line) => line && !/^#{1,6}\s/.test(line))
    .map((line) => line.replace(/^(?:[-*+]\s+|\d+[.)]\s+)/, "").replace(/[`*_]/g, "").replace(/\s+/g, " ").trim())
    .filter((line) => line && !/^v?\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?$/.test(line) && !line.startsWith("固件源码位于"));
  const summary = lines.slice(0, 2).join(" ");
  return summary ? truncateMiddle(summary, maxLength) : tr("暂无详细发布说明");
}

function normalizeFirmwareMirrorItem(item) {
  if (!item || typeof item !== "object") return undefined;
  const version = String(item.version || "").trim();
  if (!version) return undefined;
  const app = normalizeFirmwareMirrorAsset(item.app, version, "app");
  const merged = normalizeFirmwareMirrorAsset(item.merged, version, "merged");
  if (!app || !merged) return undefined;
  return {
    version,
    notes: String(item.notes || "").trim(),
    releaseUrl: normalizeFirmwareReleaseUrl(item.release_url || item.releaseUrl, version),
    app,
    merged
  };
}

function selectedRemoteFirmwareImage(kind = "merged") {
  if (!remoteFirmwareManifest) return undefined;
  return remoteFirmwareManifest[kind];
}

function makeAppTarget(partitions, value, label) {
  return {
    value,
    get label() { return `${partitions.map((partition) => `${hex(partition.address)}: ${partition.label}`).join(" + ")} ${tr(label)}`; },
    kind: "app",
    partitions,
    remoteImage: "app"
  };
}

function renderFirmwareTargets() {
  const select = $("#firmwareTarget");
  const previous = select.value || "merged";
  select.textContent = "";
  const mergedOption = document.createElement("option");
  mergedOption.value = MERGED_TARGET.value;
  setText(mergedOption, () => MERGED_TARGET.label);
  select.appendChild(mergedOption);

  const ota0 = firmwarePartitions.find((partition) => partition.label === "ota_0" && isAppPartition(partition));
  const ota1 = firmwarePartitions.find((partition) => partition.label === "ota_1" && isAppPartition(partition));
  firmwareAppTargets = [];
  if (ota0) firmwareAppTargets.push(makeAppTarget([ota0], "ota0", "App 分区"));
  if (ota1) firmwareAppTargets.push(makeAppTarget([ota1], "ota1", "App 分区"));
  if (ota0 && ota1) firmwareAppTargets.push(makeAppTarget([ota0, ota1], "otaBoth", "同时写入两个 App 槽"));

  firmwareAppTargets.forEach((target) => {
    const option = document.createElement("option");
    option.value = target.value;
    setText(option, () => target.label);
    select.appendChild(option);
  });
  select.value = [...firmwareAppTargets, MERGED_TARGET].some((target) => target.value === previous) ? previous : "merged";
}

function currentFirmwareTarget() {
  if ($("#firmwareTarget").value === "merged") return MERGED_TARGET;
  return firmwareAppTargets.find((target) => target.value === $("#firmwareTarget").value) || MERGED_TARGET;
}

function canUseRemoteFirmwareForTarget(target = currentFirmwareTarget()) {
  return Boolean(target.remoteImage && selectedRemoteFirmwareImage(target.remoteImage));
}

function updateFirmwareDownloadButton() {
  const useRemote = $("#firmwareSource").value === "remote";
  $("#downloadFirmwareBtn").disabled = !(useRemote && canUseRemoteFirmwareForTarget());
}

function selectedFirmwareSize() {
  if (selectedFirmware?.source === "remote" && verifiedFirmwareData) return verifiedFirmwareData.byteLength;
  if (selectedFirmware?.size) return selectedFirmware.size;
  return 0;
}

function isFirmwareTargetReady(target = currentFirmwareTarget(), size = selectedFirmwareSize()) {
  if (target.kind === "merged") return Boolean(firmwareFlashSizeBytes && (!size || size <= firmwareFlashSizeBytes));
  if (!firmwareDevicePort || !firmwareChipVerified || !firmwareFlashSizeBytes || target.kind !== "app" || target.partitions.length === 0) return false;
  if (!size) return true;
  return target.partitions.every((partition) => size <= partition.size);
}

function updateFirmwareWriteButton() {
  const hasSerial = "serial" in navigator;
  const target = currentFirmwareTarget();
  const hasFirmware = selectedFirmware?.source === "remote" ? Boolean(verifiedFirmwareData) : Boolean(selectedFirmware);
  $("#writeFirmwareBtn").disabled = !(hasSerial && firmwareDevicePort && firmwareChipVerified && hasFirmware && isFirmwareTargetReady(target));
}

function normalizeFlashCapacity(value) {
  if (typeof value === "number" && Number.isFinite(value) && value > 0) {
    return value < 1024 ? value * 1024 * 1024 : value;
  }
  const text = String(value || "").trim();
  const match = text.match(/([0-9]+(?:\.[0-9]+)?)\s*(KB|MB|GB|bytes?)/i);
  if (!match) return 0;
  const amount = Number(match[1]);
  const unit = match[2].toLowerCase();
  return unit.startsWith("gb") ? amount * 1024 * 1024 * 1024
    : unit.startsWith("mb") ? amount * 1024 * 1024
      : unit.startsWith("kb") ? amount * 1024 : amount;
}

function updateFirmwareInstallSummary() {
  const manifest = remoteFirmwareManifest;
  setText($("#firmwareInstallVersion"), () => manifest?.version || tr("在线固件加载失败"));
  setText($("#firmwareInstallNotesVersion"), () => manifest?.version || tr("加载中"));
  setText($("#firmwareInstallNotes"), () => manifest?.notes ? formatFirmwareNotes(manifest.notes, manifest.version) : tr("等待固件清单"));
  setAttr($("#firmwareInstallNotes"), "title", () => manifest?.notes || "");
  const notesLink = $("#firmwareInstallNotesLink");
  if (notesLink) {
    notesLink.hidden = !manifest;
    setAttr(notesLink, "href", () => manifest?.releaseUrl || FIRMWARE_RELEASES_SOURCE_URL);
  }
  setText($("#firmwareInstallDevice"), () => firmwareDevicePort
    ? (firmwareFlashSizeText === "-" ? describePort(firmwareDevicePort) : `${describePort(firmwareDevicePort)} / ${firmwareFlashSizeText}`)
    : tr("未连接"));
  const ready = Boolean(firmwareDevicePort && firmwareChipVerified && firmwareFlashSizeBytes > 0 && selectedRemoteFirmwareImage("merged"));
  $("#firmwareInstallConnectBtn").disabled = !("serial" in navigator) || firmwareInstallBusy;
  $("#firmwareInstallBtn").disabled = !ready || firmwareInstallBusy;
}

function setFirmwareInstallBusy(busy) {
  firmwareInstallBusy = busy;
  $("#firmwareInstallBtn").disabled = busy || !firmwareDevicePort || !firmwareChipVerified || !firmwareFlashSizeBytes || !selectedRemoteFirmwareImage("merged");
  $("#firmwareInstallConnectBtn").disabled = busy || !("serial" in navigator);
  $("#firmwareAdvancedPanel").querySelectorAll("input, select, button").forEach((control) => { control.disabled = busy; });
}

function setFirmwareInstallProgress(value, state) {
  $("#firmwareInstallProgress").value = value;
  setText($("#firmwareInstallPercent"), () => `${value}%`);
  if (state) setText($("#firmwareInstallState"), () => state);
}

function showFirmwareInstallConfirm() {
  if (firmwareInstallBusy || !firmwareDevicePort || !firmwareChipVerified || !firmwareFlashSizeBytes) return;
  firmwareInstallSnapshot = {
    version: remoteFirmwareManifest?.version,
    target: "merged",
    image: selectedRemoteFirmwareImage("merged")
  };
  $("#firmwareInstallConfirm").hidden = false;
  $("#firmwareInstallBtn").disabled = true;
}

async function installLatestFirmware() {
  if (firmwareInstallBusy || !firmwareInstallSnapshot?.image) return;
  setFirmwareInstallBusy(true);
  const snapshot = firmwareInstallSnapshot;
  $("#firmwareInstallConfirm").hidden = true;
  setFirmwareInstallProgress(0, tr("正在准备完整安装"));
  try {
    $("#firmwareSource").value = "remote";
    $("#firmwareTarget").value = "merged";
    if (!remoteFirmwareManifest) await loadRemoteFirmwareManifest();
    if (!remoteFirmwareManifest || remoteFirmwareManifest.version !== snapshot.version) throw new LocalizedError(() => tr("在线固件版本已变化，请重新连接并确认。"));
    setText($("#firmwareInstallResult"), () => tr`正在下载并校验 ${snapshot.version} 的完整 merged 固件。`);
    await downloadRemoteFirmware();
    if (!verifiedFirmwareData || selectedFirmware?.version !== snapshot.version) throw new LocalizedError(() => tr("固件校验未通过，未执行写入。"));
    setFirmwareInstallProgress(60, tr("校验通过，准备写入"));
    const success = await writeFirmware();
    if (!success) throw new LocalizedError(() => tr("安装失败，可重试。"));
    setFirmwareInstallProgress(100, tr("安装完成，设备正在复位"));
    setText($("#firmwareInstallResult"), () => tr("安装完成。请按需打开配网或快捷配置；页面不会自动跳转。"));
  } catch (error) {
    setFirmwareInstallProgress(0, tr("安装失败，可重试"));
    setText($("#firmwareInstallResult"), () => error.message);
  } finally {
    firmwareInstallSnapshot = undefined;
    setFirmwareInstallBusy(false);
    updateFirmwareInstallSummary();
  }
}

function firmwareTargetOffsetText(target = currentFirmwareTarget()) {
  return target.partitions.map((partition) => hex(partition.address)).join(" + ");
}

function isLocalFirmwareFileAllowed(file, target = currentFirmwareTarget()) {
  return Boolean(file && file.name.toLowerCase().endsWith(".bin"));
}

function firmwareTargetHint(target = currentFirmwareTarget()) {
  if (target.kind === "merged") return tr("完整 merged 固件只能写入 0x0。首次迁移到 v1.5.x 分区表时必须使用 merged。");
  const sizes = target.partitions.map((partition) => `${partition.label} ${hex(partition.address)} / ${formatBytes(partition.size)}`).join("，");
  return firmwareDevicePort
    ? tr`App 固件只允许写入读取到的 ${sizes}。`
    : tr("App 分区写入前必须先选择设备并读取分区表。");
}

function refreshFirmwareTargetState() {
  const target = currentFirmwareTarget();
  renderPartitionTable(firmwarePartitions, {
    tbodyId: "firmwarePartitionTableBody",
    mode: "firmware",
    fileSize: selectedFirmwareSize()
  });
  updateFirmwareDownloadButton();
  if ($("#firmwareSource").value === "remote") {
    if (remoteFirmwareManifest) setRemoteFirmwareManifest(remoteFirmwareOptions.indexOf(remoteFirmwareManifest));
    if (selectedFirmware?.source === "remote") {
      setFirmwareReady(Boolean(verifiedFirmwareData && isFirmwareTargetReady(target, verifiedFirmwareData.byteLength)));
    }
    return;
  }
  const file = $("#firmwareInput").files?.[0];
  selectedFirmware = file ? { name: file.name, size: file.size, source: "local", file } : undefined;
  setFirmwareReady(Boolean(selectedFirmware && isLocalFirmwareFileAllowed(file, target) && isFirmwareTargetReady(target, file?.size || 0)));
  setText($("#firmwareWriteState"), () => selectedFirmware
    ? `${selectedFirmware.name} / ${formatBytes(selectedFirmware.size)}`
    : tr("等待固件文件"));
  setText($("#flashResult"), () => selectedFirmware
    ? tr`已选择自定义固件文件。${firmwareTargetHint(target)}${target.kind === "app" && !isFirmwareTargetReady(target, selectedFirmware.size) ? tr(" 文件大小超过目标 App 分区或尚未读取分区表。") : ""}`
    : firmwareTargetHint(target));
  updateFirmwareWriteButton();
}

function setRemoteFirmwareManifest(index = 0, note = "") {
  remoteFirmwareManifest = remoteFirmwareOptions[index] || remoteFirmwareOptions[0];
  updateFirmwareInstallSummary();
  verifiedFirmwareData = undefined;
  selectedFirmware = undefined;
  setFirmwareReady(false);
  setProgress("firmwareWrite", 0, 100);
  if (!remoteFirmwareManifest) {
    setText($("#firmwareWriteState"), () => tr("在线固件加载失败"));
    setText($("#flashResult"), () => tr("未找到可用的 GitHub Release 固件版本。"));
    return;
  }
  $("#remoteFirmwareSelect").value = String(remoteFirmwareOptions.indexOf(remoteFirmwareManifest));
  const target = currentFirmwareTarget();
  const firmwareImage = selectedRemoteFirmwareImage(target.remoteImage);
  const merged = selectedRemoteFirmwareImage("merged");
  const app = selectedRemoteFirmwareImage("app");
  updateFirmwareDownloadButton();
  setText($("#firmwareWriteState"), () => firmwareImage
    ? tr`在线固件：${remoteFirmwareManifest.version} / ${target.kind === "merged" ? "merged" : "OTA app"} ${formatBytes(firmwareImage.size)}`
    : tr("当前目标没有可用在线固件"));
  const notes = () => remoteFirmwareManifest.notes ? tr`说明：${summarizeFirmwareNotes(remoteFirmwareManifest.notes, remoteFirmwareManifest.version)}。` : "";
  if (firmwareImage) {
    setText($("#flashResult"), () => tr`${note}已选择 GitHub Release 固件：${remoteFirmwareManifest.version}。当前目标：${target.label}。将自动下载 ${target.kind === "merged" ? tr("merged 完整固件") : tr("OTA app 固件")} ${truncateMiddle(firmwareImage.assetName)}，SHA-256 ${formatSha(firmwareImage.sha256)}。${merged && app ? `merged ${formatBytes(merged.size)}, app ${formatBytes(app.size)}. ` : ""}${notes()}${firmwareTargetHint(target)}点击“下载并校验固件”后，网页会在浏览器内完成下载与校验，通过后可直接烧录。`);
  } else {
    setText($("#flashResult"), () => tr`${note}已读取 GitHub Release 固件：${remoteFirmwareManifest.version}，但当前目标没有可用固件包。`);
  }
}

function renderRemoteFirmwareOptions(note = "") {
  $("#remoteFirmwareSelect").innerHTML = "";
  remoteFirmwareOptions.forEach((manifest, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    const imageText = manifest.merged
      ? `merged ${formatBytes(manifest.merged.size)}`
      : `OTA app ${formatBytes(manifest.app.size)}`;
    const assetName = manifest.merged?.assetName || manifest.app.assetName;
    setText(option, () => `${manifest.version} / ${truncateMiddle(assetName, 30)} / ${imageText}`);
    setAttr(option, 'title', () => `${manifest.version} / ${assetName}${manifest.notes ? ` / ${manifest.notes}` : ""}`);
    $("#remoteFirmwareSelect").appendChild(option);
  });
  setRemoteFirmwareManifest(0, note);
}

function partitionTypeName(type) {
  if (type === 0x00) return "app";
  if (type === 0x01) return "data";
  return hex(type, 2);
}

function partitionSubtypeName(type, subtype) {
  if (type === 0x00) {
    if (subtype === 0x00) return "factory";
    if (subtype >= 0x10 && subtype <= 0x1F) return `ota_${subtype - 0x10}`;
    if (subtype === 0x20) return "test";
  }
  if (type === 0x01) {
    const names = {
      0x00: "ota",
      0x01: "phy",
      0x02: "nvs",
      0x03: "coredump",
      0x04: "nvs_keys",
      0x05: "efuse",
      0x06: "undefined",
      0x80: "spiffs",
      0x81: "fat",
      0x82: "littlefs"
    };
    if (names[subtype]) return names[subtype];
  }
  return hex(subtype, 2);
}

function parsePartitionTable(bytes) {
  if (!bytes || bytes.byteLength === 0) return [];
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const partitions = [];
  for (let offset = 0; offset + 32 <= bytes.byteLength; offset += 32) {
    const magic = view.getUint16(offset, true);
    if (magic === 0xFFFF) break;
    if (magic !== 0x50AA) continue;
    const type = view.getUint8(offset + 2);
    const subtype = view.getUint8(offset + 3);
    const address = view.getUint32(offset + 4, true);
    const size = view.getUint32(offset + 8, true);
    const labelBytes = bytes.slice(offset + 12, offset + 28);
    const nulIndex = labelBytes.indexOf(0);
    const label = new TextDecoder().decode(nulIndex >= 0 ? labelBytes.slice(0, nulIndex) : labelBytes).trim();
    const flags = view.getUint32(offset + 28, true);
    if (!label) continue;
    partitions.push({ label, type, subtype, address, size, flags });
  }
  return partitions;
}

function isAssetsPartition(partition) {
  return Boolean(partition && partition.label === "assets" && partition.type === 0x01 && partition.subtype === 0x40);
}

function isAppPartition(partition) {
  return Boolean(partition && partition.type === 0x00 && (partition.label === "ota_0" || partition.label === "ota_1"));
}

function findAssetsPartition(partitions) {
  return partitions.find((partition) => partition.label === "assets");
}

function findOtaPartition(partitions, label) {
  return partitions.find((partition) => partition.label === label && partition.type === 0x00);
}

function partitionTypeText(partition) {
  return `${partitionTypeName(partition.type)} / ${partitionSubtypeName(partition.type, partition.subtype)}`;
}

function renderPartitionTable(partitions, { tbodyId, mode, fileSize = 0 } = {}) {
  const tbody = $(`#${tbodyId}`);
  tbody.textContent = "";
  if (partitions.length === 0) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 5;
    setText(cell, () => tr("未读取到有效分区表。"));
    row.appendChild(cell);
    tbody.appendChild(row);
    return undefined;
  }
  partitions.forEach((partition) => {
    const row = document.createElement("tr");
    let state = () => "-";
    let rowState = "";
    if (mode === "assets" && partition.label === "assets") {
      const valid = isAssetsPartition(partition);
      const fits = !fileSize || fileSize <= partition.size;
      state = () => valid
        ? (fits ? tr`可写入 ${hex(partition.address)}` : tr`资源包过大：${formatBytes(fileSize)} / ${formatBytes(partition.size)}`)
        : tr("不是 data / subtype 0x40");
      rowState = valid && fits ? "is-ok" : "is-warn";
    }
    if (mode === "firmware") {
      if (isAppPartition(partition)) {
        const fits = !fileSize || fileSize <= partition.size;
        state = () => fits ? tr`App 可写入 ${hex(partition.address)}` : tr`App 过大：${formatBytes(fileSize)} / ${formatBytes(partition.size)}`;
        rowState = fits ? "is-ok" : "is-warn";
      } else if (partition.label === "assets" || partition.label === "model" || partition.label === "nvs") {
        state = () => tr("禁止 App 写入");
        rowState = "is-warn";
      }
    }
    row.className = rowState;
    [
      partition.label,
      partitionTypeText(partition),
      hex(partition.address),
      formatBytes(partition.size),
      state
    ].forEach((value) => {
      const cell = document.createElement("td");
      setText(cell, () => typeof value === 'function' ? value() : value);
      row.appendChild(cell);
    });
    tbody.appendChild(row);
  });
  if (mode === "assets") return findAssetsPartition(partitions);
  return undefined;
}

async function inspectAssetDevice() {
  if (!generatedAssetPackage) {
    appendWriteLog(tr("请先生成资源包，再读取设备分区表。\n"));
    return;
  }
  if (!("serial" in navigator)) {
    appendWriteLog(tr("当前浏览器不支持 Web Serial。请使用 Chrome 或 Edge。\n"));
    return;
  }
  resetAssetDeviceState("核对中");
  $("#selectAssetDeviceBtn").disabled = true;
  setProgress("assetWrite", 0, 100);
  appendWriteLog(tr`[${nowText()}] 请选择要写入资源的设备。\n`);
  let transport;
  let selectedPort;
  try {
    const esptool = await importEsptool();
    selectedPort = await navigator.serial.requestPort();
    assetDevicePort = selectedPort;
    setText($("#assetDeviceName"), () => describePort(selectedPort));
    appendWriteLog(tr`[${nowText()}] 已选择设备：${describePort(selectedPort)}\n`);
    const Transport = esptool.Transport;
    const ESPLoader = esptool.ESPLoader;
    transport = new Transport(selectedPort, true);
    const terminal = { clean: () => {}, writeLine: (line) => appendWriteLog(`${line}\n`), write: (text) => appendWriteLog(text) };
    const loader = new ESPLoader({ transport, baudrate: Number($("#assetBaudRate").value), terminal });
    setText($("#assetWriteState"), () => tr("连接并读取分区表"));
    const chipName = await loader.main();
    const macAddress = await loader.chip.readMac(loader);
    setText($("#assetChipName"), () => chipName || tr("已连接"));
    setText($("#assetMacAddress"), () => macAddress || "-");
    appendWriteLog(tr`[${nowText()}] 正在读取分区表 0x${PARTITION_TABLE_OFFSET.toString(16)}\n`);
    const tableBytes = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, (_chunk, read, total) => {
      setProgress("assetWrite", read, total);
    });
    const partitions = parsePartitionTable(tableBytes);
    assetPartitions = partitions;
    assetPartition = renderPartitionTable(partitions, {
      tbodyId: "partitionTableBody",
      mode: "assets",
      fileSize: generatedAssetPackage?.byteLength || 0
    });
    assetPartitionVerified = Boolean(assetPartition && isAssetsPartition(assetPartition) && (!generatedAssetPackage || generatedAssetPackage.byteLength <= assetPartition.size));
    setText($("#assetPartitionState"), () => assetPartitionVerified ? tr("通过") : tr("未通过"));
    setText($("#assetWriteState"), () => assetPartitionVerified ? tr("分区核对通过") : tr("分区核对未通过"));
    appendWriteLog(assetPartitionVerified
      ? tr`[${nowText()}] 分区核对通过：assets ${hex(assetPartition.address)} / ${formatBytes(assetPartition.size)}\n`
      : tr`[${nowText()}] 分区核对未通过：未找到 data/subtype 0x40 的 assets 分区，或资源包超过分区大小。\n`);
    await resetDeviceAfterFlash(transport, selectedPort, appendWriteLog);
  } catch (error) {
    assetPartitionVerified = false;
    setText($("#assetPartitionState"), () => tr("失败"));
    setText($("#assetWriteState"), () => tr("设备核对失败"));
    appendWriteLog(tr`[${nowText()}] 设备核对失败：${error.message}\n`);
  } finally {
    updateAssetWriteButtons();
    if (transport) {
      try {
        await transport.disconnect();
      } catch (error) {
        console.warn(error);
      }
    }
    if (assetPartitionVerified) hintNextStep("#writeAssetsBtn");
  }
}

function resetFirmwareDeviceState(message = "未读取") {
  firmwareDevicePort = undefined;
  firmwarePartitions = [];
  firmwareAppTargets = [];
  firmwareChipVerified = false;
  firmwareFlashSizeBytes = 0;
  firmwareFlashSizeText = "-";
  setText($("#firmwarePartitionState"), () => tr(message));
  setText($("#firmwareDeviceName"), () => tr("未选择"));
  setText($("#firmwareChipName"), () => tr("等待读取"));
  setText($("#firmwareMacAddress"), () => "-");
  renderFirmwareTargets();
  refreshFirmwareTargetState();
  updateFirmwareInstallSummary();
}

async function inspectFirmwareDevice() {
  if (!("serial" in navigator)) {
    setText($("#flashResult"), () => tr("当前浏览器不支持 Web Serial。请使用 Chrome 或 Edge。"));
    return;
  }
  $("#selectFirmwareDeviceBtn").disabled = true;
  $("#firmwareInstallConnectBtn").disabled = true;
  setText($("#firmwarePartitionState"), () => tr("读取中"));
  setText($("#firmwareWriteState"), () => tr("连接并读取分区表"));
  setProgress("firmwareWrite", 0, 100);
  let transport;
  let selectedPort;
  try {
    const esptool = await importEsptool();
    selectedPort = await navigator.serial.requestPort();
    firmwareDevicePort = selectedPort;
    setText($("#firmwareDeviceName"), () => describePort(selectedPort));
    setText($("#flashResult"), () => tr`已选择设备：${describePort(selectedPort)}。正在读取分区表...`);
    const Transport = esptool.Transport;
    const ESPLoader = esptool.ESPLoader;
    transport = new Transport(selectedPort, true);
    const terminal = { clean: () => {}, writeLine: (line) => { setText($("#flashResult"), () => line); }, write: () => {} };
    const loader = new ESPLoader({ transport, baudrate: Number($("#firmwareBaudRate").value), terminal });
    const chipName = await loader.main();
    const macAddress = await loader.chip.readMac(loader);
    setText($("#firmwareChipName"), () => chipName || tr("已连接"));
    setText($("#firmwareMacAddress"), () => macAddress || "-");
    firmwareChipVerified = /ESP32-S3/i.test(String(chipName || ""));
    const rawFlashSize = typeof loader.chip.getFlashSize === "function"
      ? await loader.chip.getFlashSize(loader)
      : loader.chip.flashSize;
    firmwareFlashSizeBytes = normalizeFlashCapacity(rawFlashSize);
    firmwareFlashSizeText = firmwareFlashSizeBytes ? formatBytes(firmwareFlashSizeBytes) : "-";
    updateFirmwareInstallSummary();
    if (!firmwareChipVerified || !firmwareFlashSizeBytes) {
      setText($("#firmwareInstallConnection"), () => tr("无法验证芯片或 Flash 容量"));
      throw new LocalizedError(() => tr`无法验证设备：芯片 ${chipName || "-"} / Flash ${firmwareFlashSizeText}`);
    }
    setText($("#firmwareInstallConnection"), () => tr`已识别 ESP32-S3 / Flash ${firmwareFlashSizeText}`);
    let tableBytes;
    try {
      tableBytes = await loader.readFlash(PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE, (_chunk, read, total) => {
        setProgress("firmwareWrite", read, total);
      });
    } catch (partitionError) {
      tableBytes = undefined;
      setText($("#firmwarePartitionState"), () => tr("未读取"));
      setText($("#flashResult"), () => tr`未读取分区表。完整安装仍可继续；高级 App 更新需要有效分区表：${partitionError.message}`);
    }
    firmwarePartitions = parsePartitionTable(tableBytes);
    renderPartitionTable(firmwarePartitions, {
      tbodyId: "firmwarePartitionTableBody",
      mode: "firmware",
      fileSize: selectedFirmwareSize()
    });
    renderFirmwareTargets();
    const ota0 = findOtaPartition(firmwarePartitions, "ota_0");
    const ota1 = findOtaPartition(firmwarePartitions, "ota_1");
    const appCount = [ota0, ota1].filter(Boolean).length;
    setText($("#firmwarePartitionState"), () => appCount ? tr`已读取 ${appCount} 个 App 槽` : tr("未找到 App 槽"));
    setText($("#firmwareWriteState"), () => appCount ? tr("App 分区已读取") : tr("未找到 ota_0 / ota_1"));
    setText($("#flashResult"), () => appCount
      ? tr`分区表读取完成：${[ota0, ota1].filter(Boolean).map((partition) => `${partition.label} ${hex(partition.address)} / ${formatBytes(partition.size)}`).join("，")}。App 固件只能写入这些分区；merged 固件仍写入 0x0。`
      : tr("分区表为空或不可用。完整安装可继续；高级 App 更新需要有效分区表。"));
    await resetDeviceAfterFlash(transport, selectedPort, (text) => { setText($("#flashResult"), () => text.trim() || $("#flashResult").textContent); });
  } catch (error) {
    firmwareDevicePort = undefined;
    firmwarePartitions = [];
    firmwareChipVerified = false;
    firmwareFlashSizeBytes = 0;
    firmwareFlashSizeText = "-";
    setText($("#firmwarePartitionState"), () => tr("读取失败"));
    setText($("#firmwareWriteState"), () => tr("设备读取失败"));
    setText($("#flashResult"), () => tr`设备分区表读取失败：${error.message}`);
    renderFirmwareTargets();
  } finally {
    refreshFirmwareTargetState();
    updateFirmwareInstallSummary();
    $("#selectFirmwareDeviceBtn").disabled = !("serial" in navigator);
    $("#firmwareInstallConnectBtn").disabled = !("serial" in navigator);
    if (transport) {
      try {
        await transport.disconnect();
      } catch (error) {
        console.warn(error);
      }
    }
    if (firmwareDevicePort && firmwarePartitions.length) {
      hintNextStep($("#firmwareSource").value === "remote" ? "#downloadFirmwareBtn" : "#firmwareInput");
    }
    updateFirmwareInstallSummary();
  }
}

function showFirmwareOptionMessage(message) {
  const option = document.createElement('option');
  option.value = '';
  setText(option, () => tr(message));
  $("#remoteFirmwareSelect").replaceChildren(option);
}

async function loadRemoteFirmwareManifest() {
  showFirmwareOptionMessage("正在加载 GitHub Release 镜像");
  setText($("#firmwareWriteState"), () => tr("正在加载在线固件清单"));
  $("#downloadFirmwareBtn").disabled = true;
  setFirmwareReady(false);
  verifiedFirmwareData = undefined;
  selectedFirmware = undefined;
  remoteFirmwareOptions = [];
  try {
    const response = await fetch(`${FIRMWARE_RELEASES_MANIFEST_URL}?t=${Date.now()}`, { cache: "no-store" });
    if (!response.ok) throw new LocalizedError(() => tr`在线固件部署清单读取失败：HTTP ${response.status}`);
    const manifest = await response.json();
    if (!manifest || !Array.isArray(manifest.items)) throw new LocalizedError(() => tr("在线固件部署清单格式异常。"));
    remoteFirmwareOptions = manifest.items.map((item) => normalizeFirmwareMirrorItem(item)).filter(Boolean).slice(0, 10);
    if (remoteFirmwareOptions.length === 0) throw new LocalizedError(() => tr("在线固件部署清单中没有通过大小和 SHA256 预检的固件。"));
    renderRemoteFirmwareOptions();
  } catch (error) {
    remoteFirmwareOptions = [];
    showFirmwareOptionMessage("在线固件加载失败");
    setText($("#firmwareWriteState"), () => tr("在线固件加载失败"));
    setText($("#flashResult"), () => tr`${error.message} 请稍后刷新，或切换为自定义固件文件。`);
    updateFirmwareInstallSummary();
  }
}

async function downloadRemoteFirmware() {
  if (!remoteFirmwareManifest) await loadRemoteFirmwareManifest();
  if (!remoteFirmwareManifest) return;
  setProgress("firmwareWrite", 0, 100);
  setFirmwareReady(false);
  const target = currentFirmwareTarget();
  const firmwareImage = target.remoteImage ? selectedRemoteFirmwareImage(target.remoteImage) : undefined;
  if (!firmwareImage) {
    throw new LocalizedError(() => tr("当前在线固件没有可下载的 app 包。"));
  }
  setText($("#firmwareWriteState"), () => tr("正在下载在线固件"));
  setText($("#flashResult"), () => tr`正在下载 ${remoteFirmwareManifest.version} 的 ${target.label} 固件...`);
  const response = await fetch(firmwareImage.url, { cache: "no-store" });
  if (!response.ok) throw new LocalizedError(() => tr`固件下载失败：HTTP ${response.status}。可切换为自定义固件文件后手动选择 bin。`);
  const total = Number(response.headers.get("content-length")) || Number(firmwareImage.size) || 0;
  const chunks = [];
  let received = 0;
  if (response.body?.getReader) {
    const reader = response.body.getReader();
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      received += value.byteLength;
      setProgress("firmwareWrite", received, total);
      setText($("#firmwareWriteState"), () => tr`下载中 ${formatBytes(received)} / ${total ? formatBytes(total) : tr("未知大小")}`);
      if (firmwareInstallBusy) setFirmwareInstallProgress(total ? Math.min(55, Math.round(received / total * 55)) : 5, tr("正在下载并校验完整 merged 固件"));
    }
  } else {
    const buffer = await response.arrayBuffer();
    chunks.push(new Uint8Array(buffer));
    received = buffer.byteLength;
  }
  const data = new Uint8Array(received);
  let offset = 0;
  for (const chunk of chunks) {
    data.set(chunk, offset);
    offset += chunk.byteLength;
  }
  if (firmwareImage.size && data.byteLength !== Number(firmwareImage.size)) {
    verifiedFirmwareData = undefined;
    selectedFirmware = undefined;
    throw Object.assign(new LocalizedError(() => tr`固件大小不匹配，已清除本次下载：${formatBytes(data.byteLength)} / ${formatBytes(Number(firmwareImage.size))}`), { code: 'firmware-size' });
  }
  setText($("#firmwareWriteState"), () => tr("正在校验 SHA-256"));
  const actualSha = await sha256Hex(data);
  if (actualSha.toLowerCase() !== firmwareImage.sha256.toLowerCase()) {
    verifiedFirmwareData = undefined;
    selectedFirmware = undefined;
    throw Object.assign(new LocalizedError(() => tr`SHA-256 校验失败，已清除本次下载，请重新下载：${formatSha(actualSha)} != ${formatSha(firmwareImage.sha256)}`), { code: 'firmware-hash' });
  }
  verifiedFirmwareData = data;
  selectedFirmware = {
    name: `GitHub ${remoteFirmwareManifest.version} ${truncateMiddle(firmwareImage.assetName)}`,
    size: data.byteLength,
    source: "remote",
    sha256: actualSha,
    version: remoteFirmwareManifest.version,
    url: firmwareImage.url
  };
  setProgress("firmwareWrite", 100, 100);
  setFirmwareReady(isFirmwareTargetReady(target, data.byteLength));
  setText($("#firmwareWriteState"), () => tr`校验通过：${selectedFirmware.name}`);
  renderPartitionTable(firmwarePartitions, {
    tbodyId: "firmwarePartitionTableBody",
    mode: "firmware",
    fileSize: data.byteLength
  });
  setText($("#flashResult"), () => tr`${target.label} 固件已下载并通过 SHA-256 校验：${formatSha(actualSha)}。${isFirmwareTargetReady(target, data.byteLength) ? tr`现在可以串口烧录到 ${firmwareTargetOffsetText(target)}。` : tr("目标 App 分区尚未读取或文件超过分区大小，禁止烧录。")}`);
  hintNextStep("#writeFirmwareBtn");
}

async function importEsptool() {
  try {
    return await import("./vendor/esptool-js/0.5.6/bundle.js");
  } catch (error) {
    throw new LocalizedError(() => tr`烧录模块加载失败：${error?.message || tr("本地依赖不可用")}`);
  }
}

function uint8ArrayToBinaryString(bytes) {
  const chunkSize = 0x8000;
  const chunks = [];
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const chunk = bytes.subarray(i, i + chunkSize);
    let text = "";
    for (let j = 0; j < chunk.length; j += 1) text += String.fromCharCode(chunk[j]);
    chunks.push(text);
  }
  return chunks.join("");
}

function wait(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function setSerialSignals(transport, device, signals) {
  if ("dataTerminalReady" in signals && typeof transport.setDTR === "function") {
    await transport.setDTR(Boolean(signals.dataTerminalReady));
  }
  if ("requestToSend" in signals && typeof transport.setRTS === "function") {
    await transport.setRTS(Boolean(signals.requestToSend));
  }
  if (typeof device.setSignals === "function") {
    await device.setSignals(signals);
  }
}

async function resetDeviceAfterFlash(transport, device, log) {
  log(tr("正在复位设备...\n"));
  try {
    await setSerialSignals(transport, device, { dataTerminalReady: false, requestToSend: false });
    await wait(80);
    await setSerialSignals(transport, device, { dataTerminalReady: false, requestToSend: true });
    await wait(120);
    await setSerialSignals(transport, device, { dataTerminalReady: false, requestToSend: false });
    await wait(250);
    log(tr("复位信号已发送。\n"));
  } catch (error) {
    log(tr`复位信号发送失败：${error.message}\n`);
    log(tr("如果设备没有自动启动，请短按 RST 或重新插拔 USB。\n"));
  }
}

async function writeBinaryWithEsptool({ data, offset, baudRateValue, stateId, percentId, progressId, log, eraseSize, devicePort }) {
  if (!("serial" in navigator)) throw new LocalizedError(() => tr("当前浏览器不支持 Web Serial。请使用 Chrome 或 Edge。"));
  const esptool = await importEsptool();
  const device = devicePort || await navigator.serial.requestPort();
  const Transport = esptool.Transport;
  const ESPLoader = esptool.ESPLoader;
  const transport = new Transport(device, true);
  const terminal = { clean: () => {}, writeLine: (line) => log(`${line}\n`), write: (text) => log(text) };
  const loader = new ESPLoader({ transport, baudrate: Number(baudRateValue), terminal });
  try {
    setText($(`#${stateId}`), () => tr("连接设备中"));
    log(tr`目标设备：${describePort(device)}\n`);
    await loader.main();
    setText($(`#${stateId}`), () => tr("写入中"));
    const binary = data instanceof Uint8Array ? data : new Uint8Array(data);
    const binaryString = uint8ArrayToBinaryString(binary);
    const offsets = Array.isArray(offset) ? offset : [offset];
    const progressByFile = new Array(offsets.length).fill(0);
    await loader.writeFlash({
      fileArray: offsets.map((address) => ({ data: binaryString, address })),
      flashSize: "keep",
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        progressByFile[fileIndex] = total ? Math.min(1, written / total) : 0;
        const percent = Math.min(100, Math.round(progressByFile.reduce((sum, value) => sum + value, 0) / offsets.length * 100));
        $(`#${progressId}`).value = percent;
        setText($(`#${percentId}`), () => `${percent}%`);
        const targetText = offsets.length > 1 ? ` ${fileIndex + 1}/${offsets.length} ${hex(offsets[fileIndex])}` : "";
        setText($(`#${stateId}`), () => tr`写入中${targetText} ${formatBytes(written)} / ${formatBytes(total)}`);
        if (firmwareInstallBusy && stateId === "firmwareWriteState") setFirmwareInstallProgress(60 + Math.round(percent * 0.4), tr("正在写入完整 merged 固件"));
      }
    });
    if (eraseSize) offsets.forEach((address) => log(tr`写入范围：${hex(address)} + ${formatBytes(eraseSize)}\n`));
    $(`#${progressId}`).value = 100;
    setText($(`#${percentId}`), () => "100%");
    setText($(`#${stateId}`), () => tr("写入完成，正在复位"));
    await resetDeviceAfterFlash(transport, device, log);
    setText($(`#${stateId}`), () => tr("写入完成，设备已复位"));
  } finally {
    try {
      await transport.disconnect();
    } catch (error) {
      console.warn(error);
    }
  }
}

async function writeAssets() {
  if (!generatedAssetPackage) {
    appendWriteLog(tr("请先生成资源包。\n"));
    return;
  }
  if (!assetPartitionVerified || !assetDevicePort || !assetPartition) {
    appendWriteLog(tr("请先选择设备并核对分区表。\n"));
    return;
  }
  if (generatedAssetPackage.byteLength > assetPartition.size) {
    appendWriteLog(tr`资源包超过 assets 分区大小：${formatBytes(generatedAssetPackage.byteLength)} / ${formatBytes(assetPartition.size)}。\n`);
    updateAssetWriteButtons();
    return;
  }
  setProgress("assetWrite", 0, 100);
  const sha = await sha256Hex(generatedAssetPackage);
  appendWriteLog(tr`[${nowText()}] 写入确认：文件类型 custom_assets.bin，目标分区 assets，地址 ${hex(assetPartition.address)}，分区大小 ${formatBytes(assetPartition.size)}，文件大小 ${formatBytes(generatedAssetPackage.byteLength)}，SHA256 ${sha}\n`);
  appendWriteLog(tr`[${nowText()}] 开始写入 custom_assets.bin 到 ${hex(assetPartition.address)}\n`);
  try {
    await writeBinaryWithEsptool({
      data: generatedAssetPackage,
      offset: assetPartition.address,
      baudRateValue: $("#assetBaudRate").value,
      stateId: "assetWriteState",
      percentId: "assetWritePercent",
      progressId: "assetWriteProgress",
      log: appendWriteLog,
      eraseSize: generatedAssetPackage.byteLength,
      devicePort: assetDevicePort
    });
    appendWriteLog(tr`[${nowText()}] 资源写入完成。\n`);
  } catch (error) {
    setText($("#assetWriteState"), () => tr("写入失败"));
    appendWriteLog(tr`[${nowText()}] 写入失败：${error.message}\n`);
  }
}

async function eraseAssets() {
  if (!generatedAssetPackage) {
    appendWriteLog(tr("请先生成资源包，再进行资源写入操作。\n"));
    return;
  }
  if (!assetPartitionVerified || !assetDevicePort || !assetPartition) {
    appendWriteLog(tr("请先选择设备并核对分区表。\n"));
    return;
  }
  const erasedHeader = new Uint8Array(4096).fill(0xFF);
  setProgress("assetWrite", 0, 100);
  const sha = await sha256Hex(erasedHeader);
  appendWriteLog(tr`[${nowText()}] 写入确认：文件类型 erased header，目标分区 assets，地址 ${hex(assetPartition.address)}，分区大小 ${formatBytes(assetPartition.size)}，文件大小 ${formatBytes(erasedHeader.byteLength)}，SHA256 ${sha}\n`);
  appendWriteLog(tr`[${nowText()}] 开始清空资源分区头部 ${hex(assetPartition.address)}\n`);
  try {
    await writeBinaryWithEsptool({
      data: erasedHeader,
      offset: assetPartition.address,
      baudRateValue: $("#assetBaudRate").value,
      stateId: "assetWriteState",
      percentId: "assetWritePercent",
      progressId: "assetWriteProgress",
      log: appendWriteLog,
      eraseSize: erasedHeader.byteLength,
      devicePort: assetDevicePort
    });
    appendWriteLog(tr`[${nowText()}] 资源分区已清空，设备会回退到内置素材。\n`);
  } catch (error) {
    setText($("#assetWriteState"), () => tr("清空失败"));
    appendWriteLog(tr`[${nowText()}] 清空失败：${error.message}\n`);
  }
}

async function writeFirmware() {
  if (!selectedFirmware) return false;
  const target = currentFirmwareTarget();
  if (target.kind === "app" && (!firmwareDevicePort || target.partitions.length === 0)) {
    setText($("#flashResult"), () => tr("App 分区写入前必须先选择设备并读取分区表，不能使用旧地址或静默兜底。"));
    setFirmwareReady(false);
    return false;
  }
  let data;
  let sha;
  if (selectedFirmware.source === "remote") {
    if (!verifiedFirmwareData) {
      setText($("#flashResult"), () => tr("在线固件尚未下载并校验，请先点击“下载并校验固件”。"));
      return false;
    }
    data = verifiedFirmwareData;
    sha = selectedFirmware.sha256 || await sha256Hex(data);
  } else {
    if (!isLocalFirmwareFileAllowed(selectedFirmware.file)) {
      setText($("#flashResult"), () => tr("请选择 .bin 固件文件。"));
      return;
    }
    data = new Uint8Array(await selectedFirmware.file.arrayBuffer());
    sha = await sha256Hex(data);
  }
  if (!isFirmwareTargetReady(target, data.byteLength)) {
    setText($("#flashResult"), () => target.kind === "app"
      ? tr`禁止烧录：${selectedFirmware.name} 大小 ${formatBytes(data.byteLength)} 超过目标 App 分区，或尚未读取 ota_0/ota_1 分区表。`
      : tr`禁止完整安装：文件 ${formatBytes(data.byteLength)} 超过已识别的 Flash 容量 ${formatBytes(firmwareFlashSizeBytes)}，或设备容量尚未验证。`);
    setFirmwareReady(false);
    return false;
  }
  setText($("#firmwareWriteState"), () => tr`准备写入 ${selectedFirmware.name}`);
  const partitionText = target.kind === "merged"
    ? tr("完整 merged 镜像")
    : target.partitions.map((partition) => `${partition.label} ${hex(partition.address)} / ${formatBytes(partition.size)}`).join("，");
  setText($("#flashResult"), () => tr`写入确认：文件类型 ${target.kind === "merged" ? "merged bin" : "OTA App bin"}；目标分区 ${partitionText}；实际地址 ${firmwareTargetOffsetText(target)}；分区大小 ${target.kind === "merged" ? tr("完整镜像写入 0x0") : target.partitions.map((partition) => formatBytes(partition.size)).join(" + ")}；文件大小 ${formatBytes(data.byteLength)}；SHA256 ${sha}`);
  try {
    await writeBinaryWithEsptool({
      data,
      offset: target.partitions.map((partition) => partition.address),
      baudRateValue: $("#firmwareBaudRate").value,
      stateId: "firmwareWriteState",
      percentId: "firmwareWritePercent",
      progressId: "firmwareWriteProgress",
      log: (text) => { if (text.trim()) setText($("#flashResult"), () => text.trim()); },
      eraseSize: data.byteLength,
      devicePort: target.kind === "app" ? firmwareDevicePort : firmwareDevicePort
    });
    setText($("#flashResult"), () => tr`${target.label} 烧录完成，设备正在重启。`);
    return true;
  } catch (error) {
    setText($("#firmwareWriteState"), () => tr("烧录失败"));
    setText($("#flashResult"), () => tr`烧录失败：${error.message}`);
    return false;
  }
}

function activateTab(tabId, updateAddress = true) {
  if (!$$(".tab").some(tab => tab.dataset.tab === tabId)) tabId = "firmware";
  clearNextStepHint();
  $$(".tab").forEach((tab) => tab.classList.toggle("is-active", tab.dataset.tab === tabId));
  $$(".tab-panel").forEach((panel) => panel.classList.toggle("is-active", panel.id === tabId));
  if (updateAddress && window.location.hash !== `#${tabId}`) {
    window.history.pushState(null, "", `#${tabId}`);
  }
  window.dispatchEvent(new CustomEvent("host-tab-change", { detail: tabId }));
}

function bindTabs() {
  $$(".tab").forEach((tab) => {
    tab.addEventListener("click", () => activateTab(tab.dataset.tab));
  });
  const restoreTab = () => activateTab(window.location.hash.slice(1), false);
  window.addEventListener("hashchange", restoreTab);
  window.addEventListener("popstate", restoreTab);
  restoreTab();
}

async function registerServiceWorker() {
  if (!("serviceWorker" in navigator)) {
    setText(cacheState, () => tr("当前浏览器不支持离线缓存"));
    return;
  }
  try {
    const registration = await navigator.serviceWorker.register("./sw.js");
    await navigator.serviceWorker.ready;
    setText(cacheState, () => registration.active ? tr("离线缓存已启用") : tr("离线缓存已注册"));
  } catch (error) {
    setText(cacheState, () => tr`离线缓存失败：${error.message}`);
  }
}

bindTabs();
document.addEventListener("click", clearNextStepHint, true);
document.addEventListener("input", clearNextStepHint, true);
document.addEventListener("change", clearNextStepHint, true);
renderFirmwareTargets();
setSerialSupport();
registerServiceWorker();
loadRemoteFirmwareManifest().catch((error) => {
  showFirmwareOptionMessage("在线固件加载失败");
  setText($("#firmwareWriteState"), () => tr("在线固件加载失败"));
  setText($("#flashResult"), () => error.message);
});

connectSerialBtn.addEventListener("click", connectSerial);
clearLogBtn.addEventListener("click", () => {
  serialLog.textContent = "";
  receivedBytes = 0;
  setText(rxBytes, () => "0 B");
  setText(lastLineTime, () => "-");
});
saveLogBtn.addEventListener("click", downloadLog);
sendForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const command = serialCommand.value.trim();
  if (!command) return;
  await sendSerialText(command);
  serialCommand.value = "";
});

function scheduleGifRealtimePreview() {
  if (!$("#gifInput").files?.[0]) return;
  clearTimeout(gifRealtimeTimer);
  gifRealtimeTimer = setTimeout(() => {
    convertGif({ realtime: true }).catch((error) => { setText($("#assetResult"), () => tr`GIF 实时预览失败：${error.message}`); });
  }, 220);
}

function scheduleImageRealtimePreview() {
  if (getSelectedImageFiles().length === 0) return;
  clearTimeout(imageRealtimeTimer);
  imageRealtimeTimer = setTimeout(() => {
    updateSelectedImageRealtimePreview().catch((error) => { setText($("#assetResult"), () => tr`静图实时预览失败：${error.message}`); });
  }, 120);
}

$("#gifThreshold").addEventListener("input", () => {
  setText($("#gifThresholdValue"), () => $("#gifThreshold").value);
  scheduleGifRealtimePreview();
});
$("#imageThreshold").addEventListener("input", () => {
  setText($("#imageThresholdValue"), () => $("#imageThreshold").value);
  scheduleImageRealtimePreview();
});
$("#imageEdgeFade").addEventListener("input", () => {
  setText($("#imageEdgeFadeValue"), () => $("#imageEdgeFade").value);
  scheduleImageRealtimePreview();
});
$("#gifInput").addEventListener("change", () => {
  previewSelectedGif().catch((error) => { setText($("#assetResult"), () => tr`GIF 预览失败：${error.message}`); });
});
$("#imageInput").addEventListener("change", () => {
  selectedImagePreviewIndex = 0;
  previewSelectedImages().catch((error) => { setText($("#assetResult"), () => tr`静图预览失败：${error.message}`); });
});
$("#gifFit").addEventListener("change", () => {
  scheduleGifRealtimePreview();
});
$("#imageFit").addEventListener("change", () => {
  scheduleImageRealtimePreview();
});
$("#gifDither").addEventListener("change", scheduleGifRealtimePreview);
$("#gifInvert").addEventListener("change", scheduleGifRealtimePreview);
$("#imageDither").addEventListener("change", scheduleImageRealtimePreview);
$("#imageInvert").addEventListener("change", scheduleImageRealtimePreview);
$("#imagePreviewSelect").addEventListener("change", () => {
  selectedImagePreviewIndex = Number($("#imagePreviewSelect").value) || 0;
  previewSelectedImages({ keepConverted: convertedImages.length > 0 }).catch((error) => { setText($("#assetResult"), () => tr`静图预览失败：${error.message}`); });
});
$("#previewGifBtn").addEventListener("click", () => convertGif().catch((error) => { setText($("#assetResult"), () => tr`GIF 转换失败：${error.message}`); }));
$("#clearGifBtn").addEventListener("click", clearGifConversion);
$("#previewImagesBtn").addEventListener("click", () => convertImages().catch((error) => { setText($("#assetResult"), () => tr`静图转换失败：${error.message}`); }));
$("#clearImagesBtn").addEventListener("click", clearImageConversions);
$("#customWeatherCity").addEventListener("input", invalidateGeneratedAssets);
$("#customOtaServer").addEventListener("input", invalidateGeneratedAssets);
$("#buildAssetsBtn").addEventListener("click", buildAssetPackage);
$("#goToAssetsFromWriterBtn").addEventListener("click", () => {
  activateTab("assets");
  hintNextStep(".gif-asset-card .file-choose");
});
$("#goToWriterBtn").addEventListener("click", () => {
  if (!generatedAssetPackage) return;
  activateTab("writer");
  $("#selectAssetDeviceBtn").focus();
  hintNextStep("#selectAssetDeviceBtn");
});
$("#downloadAssetsBtn").addEventListener("click", downloadAssets);
$("#selectAssetDeviceBtn").addEventListener("click", inspectAssetDevice);
$("#writeAssetsBtn").addEventListener("click", writeAssets);
$("#eraseAssetsBtn").addEventListener("click", eraseAssets);
$("#selectFirmwareDeviceBtn").addEventListener("click", inspectFirmwareDevice);
$("#firmwareInstallConnectBtn").addEventListener("click", inspectFirmwareDevice);
$("#firmwareInstallBtn").addEventListener("click", showFirmwareInstallConfirm);
$("#firmwareInstallCancelBtn").addEventListener("click", () => {
  firmwareInstallSnapshot = undefined;
  $("#firmwareInstallConfirm").hidden = true;
  updateFirmwareInstallSummary();
});
$("#firmwareInstallConfirmBtn").addEventListener("click", installLatestFirmware);
$("#firmwareSource").addEventListener("change", () => {
  const source = $("#firmwareSource").value;
  const useRemote = source === "remote";
  $("#remoteFirmwareSelect").disabled = !useRemote;
  $("#refreshFirmwareBtn").disabled = !useRemote;
  $("#firmwareInput").disabled = useRemote;
  selectedFirmware = undefined;
  verifiedFirmwareData = undefined;
  setFirmwareReady(false);
  setProgress("firmwareWrite", 0, 100);
  if (useRemote) {
    updateFirmwareDownloadButton();
    if (remoteFirmwareManifest) {
      setRemoteFirmwareManifest(remoteFirmwareOptions.indexOf(remoteFirmwareManifest));
    } else {
      loadRemoteFirmwareManifest().catch((error) => { setText($("#flashResult"), () => error.message); });
    }
  } else {
    updateFirmwareDownloadButton();
    setText($("#firmwareWriteState"), () => tr("等待自定义固件文件"));
    setText($("#flashResult"), () => firmwareTargetHint());
  }
  hintNextStep("#selectFirmwareDeviceBtn");
});
$("#firmwareTarget").addEventListener("change", refreshFirmwareTargetState);
$("#remoteFirmwareSelect").addEventListener("change", () => {
  setRemoteFirmwareManifest(Number($("#remoteFirmwareSelect").value) || 0);
  hintNextStep("#selectFirmwareDeviceBtn");
});
$("#refreshFirmwareBtn").addEventListener("click", () => {
  loadRemoteFirmwareManifest().catch((error) => {
    setText($("#firmwareWriteState"), () => tr("在线固件加载失败"));
    setText($("#flashResult"), () => error.message);
  });
});
$("#downloadFirmwareBtn").addEventListener("click", () => {
  downloadRemoteFirmware().catch((error) => {
    setFirmwareReady(false);
    setText($("#firmwareWriteState"), () => error.code === 'firmware-hash' || error.code === 'firmware-size'
      ? tr("固件校验失败")
      : tr("固件下载失败"));
    setText($("#flashResult"), () => error.message);
  });
});
$("#firmwareInput").addEventListener("change", () => {
  const file = $("#firmwareInput").files?.[0];
  const target = currentFirmwareTarget();
  verifiedFirmwareData = undefined;
  selectedFirmware = file ? { name: file.name, size: file.size, source: "local", file } : undefined;
  setFirmwareReady(Boolean(selectedFirmware && isLocalFirmwareFileAllowed(file, target) && isFirmwareTargetReady(target, file?.size || 0)));
  renderPartitionTable(firmwarePartitions, {
    tbodyId: "firmwarePartitionTableBody",
    mode: "firmware",
    fileSize: file?.size || 0
  });
  setText($("#firmwareWriteState"), () => selectedFirmware ? `${selectedFirmware.name} / ${formatBytes(selectedFirmware.size)}` : tr("等待固件文件"));
  setText($("#flashResult"), () => selectedFirmware
    ? tr`已选择自定义固件文件。${firmwareTargetHint(target)}${target.kind === "app" && !isFirmwareTargetReady(target, selectedFirmware.size) ? tr(" 文件大小超过目标 App 分区或尚未读取分区表。") : ""}`
    : firmwareTargetHint(target));
  if (file && isLocalFirmwareFileAllowed(file, target)) hintNextStep("#writeFirmwareBtn");
});
$("#writeFirmwareBtn").addEventListener("click", writeFirmware);
